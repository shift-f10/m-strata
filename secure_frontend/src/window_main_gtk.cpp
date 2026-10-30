// strata-window on Linux (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu ... right click it to select exit or select a model to start and stop"). The
// Windows window's twin (window_main_win32.cpp): strata-unified-server beside it as its child, signed in with the
// server's one-time link, the page in a WebKitGTK view, a tray icon (Ayatana AppIndicator) whose menu opens the
// window, shows the server's log, starts and stops the models, restarts the server and exits. Closing the window hides
// it. window_shell.hpp holds the portable half.

#include "secure_frontend/first_start.hpp"
#include "secure_frontend/window_shell.hpp"

#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <webkit2/webkit2.h>
#include <glib-unix.h>

#include <chrono>
#include <csignal>
#include <ctime>
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
constexpr std::size_t kLogViewLines = 2000;  // the Log's lines in memory (as many as strata-tui's Log keeps)
constexpr std::size_t kLogViewBytes = 1024U * 1024U;
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
  // the server's lines (but the sign-in link) and the window's own notes: the Log, and the page shown when it ends
  w::LogLines lines{kLogViewLines, kLogViewBytes};
  GtkWidget* log_window{nullptr};  // the Log: the lines, Copy all, Save as..., Open the logs folder
  GtkWidget* log_view{nullptr};
  GtkWidget* log_note{nullptr};
  GtkTextBuffer* log_buffer{nullptr};  // (the view holds it)
  GtkTextMark* log_end{nullptr};
  std::uint64_t log_shown_first{0};  // the lines its text shows, [first, end)
  std::uint64_t log_shown_end{0};
  guint log_timer{0};  // new lines for the Log's text (a burst of lines is drawn once)
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
  if (app.status == nullptr) return;  // (no window yet in this process)
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

// --------------------------------------------------------------------------- the Log
// The server's lines in a window of their own (the owner: "in the window app version we need to add a log capability,
// the console have it but this one do not so we can troubleshoot"): the newest last, following the newest unless
// scrolled up; Copy all, Save as..., Open the logs folder. The tray's Server log and Ctrl+Shift+L open it; closing it
// hides it (the lines stay).

void log_say(const std::string& text) {
  if (app.log_note != nullptr) gtk_label_set_text(GTK_LABEL(app.log_note), text.c_str());
}

// The Log's text catches up with the lines kept: the new ones added at its end, or all of them again once lines it
// showed were dropped. It follows the newest line unless the user scrolled up.
void update_log_text() {
  if (app.log_buffer == nullptr || !gtk_widget_get_visible(app.log_window)) return;
  const auto from = app.lines.newer_than(app.log_shown_first, app.log_shown_end);
  if (from && *from == app.lines.lines().size()) return;
  GtkAdjustment* bar = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(app.log_view));
  const bool follow =
      bar == nullptr || gtk_adjustment_get_value(bar) + gtk_adjustment_get_page_size(bar) >= gtk_adjustment_get_upper(bar) - 1.0;
  GtkTextIter end{};
  if (!from) {
    const std::string all = app.lines.text();
    gtk_text_buffer_set_text(app.log_buffer, all.c_str(), static_cast<gint>(all.size()));
  } else {
    std::string added;
    for (std::size_t i = *from; i < app.lines.lines().size(); ++i) added += app.lines.lines()[i] + "\n";
    gtk_text_buffer_get_end_iter(app.log_buffer, &end);
    gtk_text_buffer_insert(app.log_buffer, &end, added.c_str(), static_cast<gint>(added.size()));
  }
  app.log_shown_first = app.lines.first();
  app.log_shown_end = app.lines.end();
  if (follow) {
    gtk_text_buffer_get_end_iter(app.log_buffer, &end);
    gtk_text_buffer_move_mark(app.log_buffer, app.log_end, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app.log_view), app.log_end);
  }
}

// A line for the log file and the Log: the server's (never the sign-in link: on_server_line keeps it out) or the
// window's own note ("[strata-window] ...", which places the server's starts and ends among its lines).
void record(const std::string& line) {
  if (!app.log && app.setup.data) {
    std::error_code error;
    if (std::filesystem::is_directory(app.setup.data->logs, error)) app.log.emplace(app.setup.data->logs / "server.log", kLogBytes);
  }
  if (app.log) app.log->write(line);
  app.lines.add(line);
  if (app.log_window != nullptr && gtk_widget_get_visible(app.log_window) && app.log_timer == 0) {
    app.log_timer = g_timeout_add(100, [](gpointer) -> gboolean {
      app.log_timer = 0;
      update_log_text();
      return G_SOURCE_REMOVE;
    }, nullptr);
  }
}

void copy_log() {
  const std::string all = app.lines.text();
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), all.c_str(), static_cast<gint>(all.size()));
  log_say("Copied " + std::to_string(app.lines.lines().size()) + " lines: paste them where you send them.");
}

void save_log() {
  GtkWidget* dialog = gtk_file_chooser_dialog_new("Save the server log", GTK_WINDOW(app.log_window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                  "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
  GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
  gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  (void)localtime_r(&now, &local);
  gtk_file_chooser_set_current_name(chooser, w::saved_log_name(local).c_str());
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    if (gchar* file = gtk_file_chooser_get_filename(chooser); file != nullptr) {
      const std::filesystem::path chosen{file};
      g_free(file);
      const auto failed = app.lines.save(chosen);
      log_say(failed ? *failed + "."
                     : "Saved " + std::to_string(app.lines.lines().size()) + " lines to " + chosen.filename().string() + ".");
    }
  }
  gtk_widget_destroy(dialog);
}

void open_logs_folder() {
  if (!app.setup.data) return;
  GError* error = nullptr;
  if (gchar* uri = g_filename_to_uri(app.setup.data->logs.c_str(), nullptr, &error); uri != nullptr) {
    (void)gtk_show_uri_on_window(GTK_WINDOW(app.log_window), uri, GDK_CURRENT_TIME, &error);
    g_free(uri);
  }
  if (error != nullptr) {
    log_say("The logs folder could not be opened.");
    g_error_free(error);
  }
}

void show_log() {
  if (app.log_window == nullptr) {
    app.log_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_application(GTK_WINDOW(app.log_window), app.application);  // (Ctrl+Shift+L there too)
    gtk_window_set_title(GTK_WINDOW(app.log_window), "M-Strata - server log");
    gtk_window_set_default_size(GTK_WINDOW(app.log_window), 1000, 680);
    gtk_window_set_icon(GTK_WINDOW(app.log_window), gtk_window_get_icon(GTK_WINDOW(app.window)));
    g_signal_connect(app.log_window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);  // the lines stay
    GtkWidget* copy = gtk_button_new_with_label("Copy all");
    g_signal_connect(copy, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { copy_log(); }), nullptr);
    GtkWidget* save = gtk_button_new_with_label("Save as...");
    g_signal_connect(save, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { save_log(); }), nullptr);
    GtkWidget* folder = gtk_button_new_with_label("Open the logs folder");
    g_signal_connect(folder, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) { open_logs_folder(); }), nullptr);
    gtk_widget_set_sensitive(folder, app.setup.data ? TRUE : FALSE);  // (no data folder with --config)
    app.log_note = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(app.log_note), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(app.log_note), 0.0F);
    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(buttons), 8);
    for (GtkWidget* button : {copy, save, folder}) gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), app.log_note, TRUE, TRUE, 0);
    app.log_buffer = gtk_text_buffer_new(nullptr);
    GtkTextIter end{};
    gtk_text_buffer_get_end_iter(app.log_buffer, &end);
    app.log_end = gtk_text_buffer_create_mark(app.log_buffer, nullptr, &end, FALSE);
    app.log_view = gtk_text_view_new_with_buffer(app.log_buffer);
    g_object_unref(app.log_buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app.log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.log_view), GTK_WRAP_WORD_CHAR);
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(scrolled), app.log_view);
    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(column), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), scrolled, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(app.log_window), column);
    log_say(std::string{"The server's newest lines, as its console prints them."} +
            (app.setup.data ? " Every line is in the data folder's logs/server.log." : ""));
  }
  gtk_widget_show_all(app.log_window);
  gtk_window_present(GTK_WINDOW(app.log_window));
  update_log_text();
}

void show_server_ended(const int code) {
  std::string page = "<!doctype html><meta charset=utf-8><title>M-Strata</title><body style=\"font:14px sans-serif;"
                     "background:#121417;color:#e8e8e8;margin:24px\"><h2>The M-Strata server stopped</h2><p>It ended with code " +
                     std::to_string(code) + ". Its last lines are below; every line is in the tray menu's Server log and in the "
                     "data folder's logs/server.log. Choose Restart the server in the tray icon's menu.</p><pre "
                     "style=\"white-space:pre-wrap;background:#1d2024;padding:12px;border-radius:8px\">";
  for (const auto& line : app.lines.last(kRecentLines)) page += html_escaped(line) + "\n";
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
    return;  // the one-time link goes to the view only: never to the log file, the Log or the screen
  }
  record(line);
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
  record("[strata-window] starting the server");
  // From GTK's (main) thread: the server's parent-death signal follows the thread that started it.
  const auto failed = app.server.start(
      app.setup.server, app.setup.arguments, app.setup.environment,
      [](std::string line) { on_main_thread([line = std::move(line)]() mutable { on_server_line(std::move(line)); }); },
      [](const int code) {
        on_main_thread([code] {
          record("[strata-window] the server ended (exit code " + std::to_string(code) + ")");
          if (app.stopping) return;
          show_server_ended(code);
          notify("The M-Strata server stopped. Choose Restart the server in the tray icon's menu.");
        });
      });
  if (failed) record("[strata-window] " + *failed);
  show_status(failed ? *failed : std::string{"Starting the M-Strata server..."});
}

// Stops the server on a thread of its own (an orderly stop can take a while: the model stops first), then restarts it
// or quits on GTK's thread.
void stop_server_then(const bool restart) {
  if (app.stopping) return;
  app.stopping = true;
  app.restarting = restart;
  record(restart ? "[strata-window] restarting the server" : "[strata-window] stopping the server (Exit)");
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
  add_item("Server log", true, [](GtkMenuItem*, gpointer) { show_log(); }, nullptr);
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
    if (app.log_window != nullptr) gtk_widget_hide(app.log_window);
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
  // make --debugging (owner 2026-10-29): a debugging build says what it logs and starts the server only on Yes (No, the
  // default, returns before the application is held, so it ends before anything starts)
  if (w::asks_before_starting()) {
    const std::string notice = std::string{w::kDebuggingNotice} + "\n\nStart the server?";
    GtkWidget* ask = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO, "%s", notice.c_str());
    gtk_window_set_title(GTK_WINDOW(ask), "M-Strata - debugging build");
    gtk_dialog_set_default_response(GTK_DIALOG(ask), GTK_RESPONSE_NO);
    const gint answer = gtk_dialog_run(GTK_DIALOG(ask));
    gtk_widget_destroy(ask);
    if (answer != GTK_RESPONSE_YES) return;
  }
  app.window = gtk_application_window_new(application);
  // the first start only: the tray keeps it running while the window is hidden (a second start's activation is sent
  // here instead, so that process holds nothing and ends once it has shown this window)
  g_application_hold(G_APPLICATION(application));
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
  // Ctrl+Shift+L in the window (and the Log): the Log
  GSimpleAction* log_action = g_simple_action_new("show-log", nullptr);
  g_signal_connect(log_action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer) { show_log(); }), nullptr);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(log_action));
  g_object_unref(log_action);
  const gchar* const log_keys[] = {"<Primary><Shift>l", nullptr};
  gtk_application_set_accels_for_action(application, "app.show-log", log_keys);
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
    if (app.window == nullptr) {  // no window and no server here (before the first activation)
      g_application_quit(G_APPLICATION(app.application));
      return G_SOURCE_CONTINUE;
    }
    app.exiting = true;
    stop_server_then(false);
    return G_SOURCE_CONTINUE;
  };
  g_unix_signal_add(SIGTERM, on_signal, nullptr);
  g_unix_signal_add(SIGINT, on_signal, nullptr);
  char* only_name[] = {argv[0], nullptr};               // the window parses its own arguments (the server's)
  const int status = g_application_run(G_APPLICATION(app.application), 1, only_name);
  if (app.server.running()) (void)app.server.stop(std::chrono::seconds(30));
  g_object_unref(app.application);
  return status;
}
