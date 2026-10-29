// strata-window on Linux (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu ... right click it to select exit or select a model to start and stop"). The
// Windows window's twin (window_main_win32.cpp): strata-unified-server beside it as its child, signed in with the
// server's one-time link, the page in a WebKitGTK view, a tray icon (Ayatana AppIndicator) whose menu opens the
// window, starts and stops the models, restarts the server and exits. Closing the window hides it. window_shell.hpp
// holds the portable half.

#include "secure_frontend/first_start.hpp"
#include "secure_frontend/window_shell.hpp"

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <webkit2/webkit2.h>
#include <glib-unix.h>

#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace w = secure_frontend::window;

namespace {

constexpr std::uintmax_t kLogBytes = 4U * 1024U * 1024U;
constexpr std::size_t kRecentLines = 40;

struct App final {
  GtkApplication* application{nullptr};
  GtkWidget* window{nullptr};
  GtkWidget* status{nullptr};  // the text shown until the web view exists
  GtkWidget* stack{nullptr};
  WebKitWebView* view{nullptr};
  AppIndicator* indicator{nullptr};
  GtkWidget* menu{nullptr};
  w::ServerSetup setup;
  w::ServerChild server;
  std::optional<w::RotatingLog> log;
  std::deque<std::string> recent;  // the server's last lines (but the sign-in link), for the page shown when it ends
  std::string origin;
  std::optional<std::string> key;  // the server's key, in memory only
  bool keyless{false};
  bool own_page{false};
  bool stopping{false};
  bool exiting{false};
  bool restarting{false};
  bool told_about_tray{false};
  bool first_page{true};  // the first page opens on the Monitor; a restart's leaves the view to the page
  guint sign_in_timer{0};
  guint menu_timer{0};
  std::mutex models_mutex;          // the tray's model list, refreshed on a thread of its own
  std::optional<w::TrayModels> models;
  bool refreshing{false};
};
App app;

std::string html_escaped(const std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void notify(const std::string& text) {
  GNotification* note = g_notification_new("M-Strata");
  g_notification_set_body(note, text.c_str());
  g_application_send_notification(G_APPLICATION(app.application), nullptr, note);
  g_object_unref(note);
}

// Work done on another thread comes back to GTK's thread through these.
template <typename Fn>
void on_main_thread(Fn fn) {
  auto* call = new std::function<void()>(std::move(fn));
  g_idle_add(
      [](gpointer data) -> gboolean {
        std::unique_ptr<std::function<void()>> owned{static_cast<std::function<void()>*>(data)};
        (*owned)();
        return G_SOURCE_REMOVE;
      },
      call);
}

void show_status(const std::string& text) {
  gtk_label_set_text(GTK_LABEL(app.status), text.c_str());
  if (app.stack != nullptr) gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "status");
}

void show_window() {
  gtk_widget_show_all(app.window);
  gtk_window_present(GTK_WINDOW(app.window));
}

void navigate(const std::string& url) {
  gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "page");
  webkit_web_view_load_uri(app.view, url.c_str());
}

void open_in_browser(const std::string& url) {
  GError* error = nullptr;
  (void)gtk_show_uri_on_window(GTK_WINDOW(app.window), url.c_str(), GDK_CURRENT_TIME, &error);
  if (error != nullptr) g_error_free(error);
}

void show_server_ended(const int code) {
  std::string page = "<!doctype html><meta charset=utf-8><title>M-Strata</title><body style=\"font:14px sans-serif;"
                     "background:#121417;color:#e8e8e8;margin:24px\"><h2>The M-Strata server stopped</h2><p>It ended with code " +
                     std::to_string(code) + ". Its last lines are below; the whole log is in the data folder's logs/server.log. "
                     "Choose Restart the server in the tray icon's menu.</p><pre style=\"white-space:pre-wrap;background:#1d2024;"
                     "padding:12px;border-radius:8px\">";
  for (const auto& line : app.recent) page += html_escaped(line) + "\n";
  page += "</pre>";
  app.own_page = true;
  gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "page");
  webkit_web_view_load_html(app.view, page.c_str(), nullptr);
}

// The page's origin stays; other web addresses open in the default browser; anything else is refused.
gboolean on_decide_policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
  WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  WebKitURIRequest* request = webkit_navigation_action_get_request(action);
  const char* uri = webkit_uri_request_get_uri(request);
  const std::string target = uri != nullptr ? uri : "";
  const bool own = app.own_page;
  app.own_page = false;
  if (own && (target == "about:blank" || target.rfind("data:", 0) == 0)) {
    webkit_policy_decision_use(decision);
    return TRUE;
  }
  const auto where = w::navigation_for(target, app.origin);
  if (where == w::Navigation::stay && type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
    webkit_policy_decision_use(decision);
    return TRUE;
  }
  webkit_policy_decision_ignore(decision);  // never a second view: the page's origin here, any other address in the browser
  if (where == w::Navigation::stay) webkit_web_view_load_uri(app.view, target.c_str());
  if (where == w::Navigation::browser) open_in_browser(target);
  return TRUE;
}

void start_server();

void on_server_line(std::string line) {
  if (!w::loggable(line)) {
    if (const auto link = w::sign_in_url(line)) {
      if (app.sign_in_timer != 0) g_source_remove(app.sign_in_timer);
      app.sign_in_timer = 0;
      navigate(w::page_address(link, app.origin, app.first_page));
      app.first_page = false;
    }
    return;  // the one-time link goes to the view only: never to the log, the recent lines or the screen
  }
  if (!app.log && app.setup.data) {
    std::error_code error;
    if (std::filesystem::is_directory(app.setup.data->logs, error)) app.log.emplace(app.setup.data->logs / "server.log", kLogBytes);
  }
  if (app.log) app.log->write(line);
  app.recent.push_back(line);
  if (app.recent.size() > kRecentLines) app.recent.pop_front();
  if (const auto origin = w::listening_origin(line)) {
    app.origin = *origin;
    const auto key = w::server_key(app.setup);
    app.key = key ? key->key : std::nullopt;
    app.keyless = key && key->keyless;
    show_status("Opening the page...");
    // the sign-in line follows at once when the server prints one; without it, the page's own sign-in
    app.sign_in_timer = g_timeout_add(1500, [](gpointer) -> gboolean {
      app.sign_in_timer = 0;
      if (!app.origin.empty()) {  // no link: the page's own sign-in
        navigate(w::page_address(std::nullopt, app.origin, app.first_page));
        app.first_page = false;
      }
      return G_SOURCE_REMOVE;
    }, nullptr);
  }
}

void start_server() {
  app.origin.clear();
  app.key.reset();
  app.recent.clear();
  // From GTK's (main) thread: the server's parent-death signal follows the thread that started it.
  const auto failed = app.server.start(
      app.setup.server, app.setup.arguments, app.setup.environment,
      [](std::string line) { on_main_thread([line = std::move(line)]() mutable { on_server_line(std::move(line)); }); },
      [](const int code) {
        on_main_thread([code] {
          if (app.stopping) return;
          show_server_ended(code);
          notify("The M-Strata server stopped. Choose Restart the server in the tray icon's menu.");
        });
      });
  show_status(failed ? *failed : std::string{"Starting the M-Strata server..."});
}

// Stops the server on a thread of its own (an orderly stop can take a while: the model stops first), then restarts it
// or quits on GTK's thread.
void stop_server_then(const bool restart) {
  if (app.stopping) return;
  app.stopping = true;
  app.restarting = restart;
  show_status(restart ? "Restarting the M-Strata server..." : "Stopping the M-Strata server...");
  std::thread([] {
    (void)app.server.stop(std::chrono::seconds(60));
    on_main_thread([] {
      app.stopping = false;
      if (app.exiting) {
        g_application_quit(G_APPLICATION(app.application));
      } else if (app.restarting) {
        app.restarting = false;
        start_server();
      }
    });
  }).detach();
}

void model_request(std::string path, std::string body, std::string done) {
  const std::string origin = app.origin;
  const std::optional<std::string> key = app.key;
  std::thread([origin, key, path = std::move(path), body = std::move(body), done = std::move(done)] {
    const auto answer = w::loopback_request(origin, "POST", path, key ? &*key : nullptr, body, std::chrono::seconds(20));
    std::string text = !answer ? std::string{"The server did not answer."}
                       : answer->status / 100 != 2 ? "The server refused: " + answer->body.substr(0, 200)
                                                     : done;
    on_main_thread([text = std::move(text)] { notify(text); });
  }).detach();
}

// The tray's menu: AppIndicator exports a fixed menu, so it is rebuilt whenever the model list changes.
void rebuild_menu();

void add_item(const std::string& label, const bool sensitive, void (*activate)(GtkMenuItem*, gpointer), gpointer data) {
  GtkWidget* item = gtk_menu_item_new_with_label(label.c_str());
  gtk_widget_set_sensitive(item, sensitive ? TRUE : FALSE);
  if (activate != nullptr) g_signal_connect(item, "activate", G_CALLBACK(activate), data);
  gtk_menu_shell_append(GTK_MENU_SHELL(app.menu), item);
}

void add_separator() { gtk_menu_shell_append(GTK_MENU_SHELL(app.menu), gtk_separator_menu_item_new()); }

void rebuild_menu() {
  GList* children = gtk_container_get_children(GTK_CONTAINER(app.menu));
  for (GList* entry = children; entry != nullptr; entry = entry->next) gtk_widget_destroy(GTK_WIDGET(entry->data));
  g_list_free(children);
  add_item("Open M-Strata", true, [](GtkMenuItem*, gpointer) { show_window(); }, nullptr);
  add_separator();
  std::optional<w::TrayModels> models;
  {
    std::lock_guard lock(app.models_mutex);
    models = app.models;
  }
  if (!models) {
    add_item(app.server.running() ? "Models: open the page" : "The server is not running", false, nullptr, nullptr);
  } else if (models->models.empty()) {
    add_item("No models yet: add one on the Models tab", false, nullptr, nullptr);
  } else {
    for (std::size_t i = 0; i < models->models.size() && i < 50U; ++i) {
      const auto& model = models->models[i];
      auto* name = g_strdup(model.name.c_str());  // freed with the item
      if (model.state == "running") {
        add_item("Stop " + model.name, true, [](GtkMenuItem*, gpointer data) {
          model_request("/v1/strata/models/stop", "", std::string{"Stopping "} + static_cast<const char*>(data));
        }, name);
      } else if (model.state == "starting" || model.state == "loading" || model.state == "calibrating") {
        add_item(model.name + " (loading...)", false, nullptr, name);
      } else {
        add_item("Start " + model.name, true, [](GtkMenuItem*, gpointer data) {
          const std::string card = static_cast<const char*>(data);
          model_request("/v1/strata/models/start", w::start_body(card), "Starting " + card + ": the page's Monitor shows it load");
        }, name);
      }
      GList* items = gtk_container_get_children(GTK_CONTAINER(app.menu));
      g_object_set_data_full(G_OBJECT(g_list_last(items)->data), "card", name, g_free);
      g_list_free(items);
    }
  }
  add_separator();
  add_item("Restart the server", !app.stopping, [](GtkMenuItem*, gpointer) { stop_server_then(true); }, nullptr);
  add_item("Exit", !app.stopping, [](GtkMenuItem*, gpointer) {
    app.exiting = true;
    gtk_widget_hide(app.window);
    stop_server_then(false);
  }, nullptr);
  gtk_widget_show_all(app.menu);
  // a middle click on the icon opens the window (the menu's first item)
  GList* items = gtk_container_get_children(GTK_CONTAINER(app.menu));
  if (items != nullptr) app_indicator_set_secondary_activate_target(app.indicator, GTK_WIDGET(items->data));
  g_list_free(items);
}

// Every 3 s, the model list on a thread of its own; the menu is rebuilt when it changed.
gboolean refresh_models(gpointer) {
  if (app.refreshing || app.origin.empty() || !app.server.running() || (!app.key && !app.keyless)) return G_SOURCE_CONTINUE;
  app.refreshing = true;
  const std::string origin = app.origin;
  const std::optional<std::string> key = app.key;
  std::thread([origin, key] {
    const auto answer = w::loopback_request(origin, "GET", "/v1/strata/models", key ? &*key : nullptr, "", std::chrono::milliseconds(1500));
    std::optional<w::TrayModels> models;
    if (answer && answer->status == 200) models = w::tray_models(answer->body);
    on_main_thread([models = std::move(models)] {
      app.refreshing = false;
      bool changed = false;
      {
        std::lock_guard lock(app.models_mutex);
        const auto same = [](const std::optional<w::TrayModels>& a, const std::optional<w::TrayModels>& b) {
          if (a.has_value() != b.has_value()) return false;
          if (!a) return true;
          if (a->models.size() != b->models.size()) return false;
          for (std::size_t i = 0; i < a->models.size(); ++i) {
            if (a->models[i].name != b->models[i].name || a->models[i].state != b->models[i].state) return false;
          }
          return true;
        };
        changed = !same(app.models, models);
        app.models = models;
      }
      if (changed) rebuild_menu();
    });
  }).detach();
  return G_SOURCE_CONTINUE;
}

// The tray's and the window's icon, drawn here: an "M" on the page's accent colour.
GdkPixbuf* make_icon(const int size) {
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t* cr = cairo_create(surface);
  const double r = size / 4.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, size - r, r, r, -G_PI / 2, 0);
  cairo_arc(cr, size - r, size - r, r, 0, G_PI / 2);
  cairo_arc(cr, r, size - r, r, G_PI / 2, G_PI);
  cairo_arc(cr, r, r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path(cr);
  cairo_set_source_rgb(cr, 52 / 255.0, 211 / 255.0, 153 / 255.0);
  cairo_fill(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size * 0.72);
  cairo_text_extents_t extents;
  cairo_text_extents(cr, "M", &extents);
  cairo_move_to(cr, (size - extents.width) / 2 - extents.x_bearing, (size - extents.height) / 2 - extents.y_bearing);
  cairo_set_source_rgb(cr, 10 / 255.0, 30 / 255.0, 24 / 255.0);
  cairo_show_text(cr, "M");
  cairo_destroy(cr);
  GdkPixbuf* pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
  cairo_surface_destroy(surface);
  return pixbuf;
}

void activate(GtkApplication* application, gpointer) {
  if (app.window != nullptr) {  // a second start: this window again
    show_window();
    return;
  }
  app.window = gtk_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(app.window), "M-Strata");
  gtk_window_set_default_size(GTK_WINDOW(app.window), 1280, 860);
  GdkPixbuf* icon = make_icon(64);
  gtk_window_set_icon(GTK_WINDOW(app.window), icon);
  g_signal_connect(app.window, "delete-event", G_CALLBACK(+[](GtkWidget* window, GdkEvent*, gpointer) -> gboolean {
    gtk_widget_hide(window);  // closing keeps the server and the tray: Exit is on the tray's menu
    if (!app.told_about_tray) {
      app.told_about_tray = true;
      notify("M-Strata keeps running in the tray. Use its icon's menu to start or stop a model, or to exit.");
    }
    return TRUE;
  }), nullptr);
  // the web view's data in the data folder's private state folder (state/window), which the server made at its first start
  WebKitWebsiteDataManager* data = nullptr;
  if (app.setup.data) {
    const auto folder = (app.setup.data->state / "window").string();
    data = webkit_website_data_manager_new("base-data-directory", folder.c_str(), "base-cache-directory", folder.c_str(), nullptr);
  } else {
    data = webkit_website_data_manager_new_ephemeral();
  }
  WebKitWebContext* context = webkit_web_context_new_with_website_data_manager(data);
  g_object_unref(data);
  app.view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(context));
  g_object_unref(context);
  WebKitSettings* settings = webkit_web_view_get_settings(app.view);
  webkit_settings_set_enable_developer_extras(settings, FALSE);
  g_signal_connect(app.view, "decide-policy", G_CALLBACK(on_decide_policy), nullptr);
  app.status = gtk_label_new("Starting the M-Strata server...");
  gtk_label_set_line_wrap(GTK_LABEL(app.status), TRUE);
  app.stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(app.stack), app.status, "status");
  gtk_stack_add_named(GTK_STACK(app.stack), GTK_WIDGET(app.view), "page");
  gtk_container_add(GTK_CONTAINER(app.window), app.stack);
  // the tray icon: the drawn icon written beside the web view's data, or a theme icon
  std::string icon_name = "network-server";
  if (app.setup.data) {
    const auto file = (app.setup.data->state / "window" / "m-strata.png").string();
    std::error_code error;
    std::filesystem::create_directories(app.setup.data->state / "window", error);
    if (!error && gdk_pixbuf_save(icon, file.c_str(), "png", nullptr, nullptr)) icon_name = file;
  }
  g_object_unref(icon);
  app.indicator = app_indicator_new("m-strata", icon_name.c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  app_indicator_set_status(app.indicator, APP_INDICATOR_STATUS_ACTIVE);
  app_indicator_set_title(app.indicator, "M-Strata");
  app.menu = gtk_menu_new();
  app_indicator_set_menu(app.indicator, GTK_MENU(app.menu));
  rebuild_menu();
  app.menu_timer = g_timeout_add_seconds(3, refresh_models, nullptr);
  gtk_widget_show_all(app.window);
  gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "status");
  start_server();
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
  auto setup = w::server_setup(arguments, secure_frontend::executable_directory());
  if (const auto* problem = std::get_if<std::string>(&setup)) {
    gtk_init(&argc, &argv);
    GtkWidget* dialog = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", problem->c_str());
    gtk_window_set_title(GTK_WINDOW(dialog), "M-Strata");
    (void)gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return 1;
  }
  app.setup = std::get<w::ServerSetup>(std::move(setup));
  // One window per data folder: a second start shows the first one's window (GApplication's unique name).
  const auto folder = app.setup.data ? app.setup.data->root.string() : app.setup.server.string();
  const std::string id = "io.github.mstrata.Window.h" + std::to_string(std::hash<std::string>{}(folder));
#if GLIB_CHECK_VERSION(2, 74, 0)
  app.application = gtk_application_new(id.c_str(), G_APPLICATION_DEFAULT_FLAGS);
#else
  app.application = gtk_application_new(id.c_str(), G_APPLICATION_FLAGS_NONE);
#endif
  g_signal_connect(app.application, "activate", G_CALLBACK(activate), nullptr);
  // SIGTERM / SIGINT (a session's end, a service manager): the server stops in order, then the window
  const auto on_signal = [](gpointer) -> gboolean {
    app.exiting = true;
    stop_server_then(false);
    return G_SOURCE_CONTINUE;
  };
  g_unix_signal_add(SIGTERM, on_signal, nullptr);
  g_unix_signal_add(SIGINT, on_signal, nullptr);
  g_application_hold(G_APPLICATION(app.application));  // the tray keeps it running while the window is hidden
  char* only_name[] = {argv[0], nullptr};               // the window parses its own arguments (the server's)
  const int status = g_application_run(G_APPLICATION(app.application), 1, only_name);
  if (app.server.running()) (void)app.server.stop(std::chrono::seconds(30));
  g_object_unref(app.application);
  return status;
}
