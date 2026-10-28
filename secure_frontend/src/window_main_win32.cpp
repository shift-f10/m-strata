// strata-window on Windows (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu ... right click it to select exit or select a model to start and stop"). It runs
// strata-secure-server beside it as its child with no console, signs in with the server's one-time link, shows the
// page in a WebView2 view, and lives in the tray: closing the window hides it; the tray's menu opens it, starts and
// stops the models, restarts the server and exits (the server stops in order first). window_shell.hpp holds the
// portable half; this file is the window, the tray and WebView2.

#include "secure_frontend/first_start.hpp"
#include "secure_frontend/window_shell.hpp"
#include "secure_frontend/win32_prelude.hpp"

#include <objbase.h>
#include <shellapi.h>

#include <WebView2.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace w = secure_frontend::window;

namespace {

constexpr wchar_t kWindowClass[] = L"MStrataWindow";
constexpr wchar_t kTitle[] = L"M-Strata";
constexpr UINT kTrayMessage = WM_APP + 1;  // the tray icon's mouse and keyboard events
constexpr UINT kServerLine = WM_APP + 2;   // lParam: a std::string* the reader thread allocated
constexpr UINT kServerExit = WM_APP + 3;   // wParam: the exit code
constexpr UINT kServerStopped = WM_APP + 4;  // the stopper thread is done (Restart or Exit goes on)
constexpr UINT kShowYourself = WM_APP + 5;   // a second strata-window asks this one to show itself
constexpr UINT kNotice = WM_APP + 6;         // lParam: a std::wstring* for a tray notification
constexpr UINT_PTR kSignInTimer = 1;         // the listening line came: the page's own address unless a link follows
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuRestart = 2;
constexpr UINT kMenuExit = 3;
constexpr UINT kMenuModelBase = 100;
constexpr std::uintmax_t kLogBytes = 4U * 1024U * 1024U;
constexpr std::size_t kRecentLines = 40;

std::wstring wide(const std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(length, 0)), L'\0');
  if (length > 0) (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

std::string narrow(const std::wstring_view text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(length, 0)), '\0');
  if (length > 0) (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
  return out;
}

std::wstring html_escaped(const std::string_view text) {
  std::wstring out;
  for (const wchar_t c : wide(text)) {
    if (c == L'<') out += L"&lt;";
    else if (c == L'>') out += L"&gt;";
    else if (c == L'&') out += L"&amp;";
    else if (c == L'"') out += L"&quot;";
    else out += c;
  }
  return out;
}

// A COM callback object for one WebView2 handler interface, without WRL: its Invoke forwards to a function.
template <typename Interface, typename... Args>
class Handler final : public Interface {
 public:
  explicit Handler(std::function<HRESULT(Args...)> invoke) : invoke_(std::move(invoke)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(Interface)) {
      *out = static_cast<Interface*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return invoke_(args...); }

 private:
  std::atomic<ULONG> references_{1};
  std::function<HRESULT(Args...)> invoke_;
};

// The tray's and the window's icon, drawn here (no resource file): an "M" on the page's accent colour.
HICON make_icon(const int size) {
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  HDC screen = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (colour == nullptr || dc == nullptr) return LoadIconW(nullptr, IDI_APPLICATION);
  HGDIOBJ previous = SelectObject(dc, colour);
  HBRUSH background = CreateSolidBrush(RGB(52, 211, 153));
  HPEN outline = CreatePen(PS_NULL, 0, 0);
  HGDIOBJ old_brush = SelectObject(dc, background);
  HGDIOBJ old_pen = SelectObject(dc, outline);
  const int radius = size / 4;
  (void)RoundRect(dc, 0, 0, size + 1, size + 1, radius, radius);
  HFONT font = CreateFontW(-(size * 3) / 4, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HGDIOBJ old_font = SelectObject(dc, font);
  (void)SetBkMode(dc, TRANSPARENT);
  (void)SetTextColor(dc, RGB(10, 30, 24));
  RECT box{0, 0, size, size};
  (void)DrawTextW(dc, L"M", 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, old_font);
  SelectObject(dc, old_pen);
  SelectObject(dc, old_brush);
  SelectObject(dc, previous);
  DeleteObject(font);
  DeleteObject(outline);
  DeleteObject(background);
  DeleteDC(dc);
  // GDI leaves alpha at 0: every pixel the rounded square covers becomes opaque, the corners stay clear
  auto* pixels = static_cast<std::uint32_t*>(bits);
  for (int i = 0; i < size * size; ++i) {
    if ((pixels[i] & 0x00FFFFFFU) != 0U) pixels[i] |= 0xFF000000U;
  }
  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{TRUE, 0, 0, mask, colour};
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(colour);
  return icon != nullptr ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

struct App final {
  HINSTANCE instance{nullptr};
  HWND window{nullptr};
  HICON icon_large{nullptr};
  HICON icon_small{nullptr};
  NOTIFYICONDATAW tray{};
  UINT taskbar_created{0};
  ICoreWebView2Environment* environment{nullptr};
  ICoreWebView2Controller* controller{nullptr};
  ICoreWebView2* view{nullptr};
  bool view_requested{false};
  w::ServerSetup setup;
  w::ServerChild server;
  std::optional<w::RotatingLog> log;
  std::deque<std::string> recent;  // the server's last lines (but the sign-in link), for the page shown when it ends
  std::string origin;              // the page's origin, once the server listens
  std::optional<std::string> pending;  // where the view goes once it exists
  std::optional<w::ServerKey> key;
  std::vector<std::string> menu_models;
  std::wstring status{L"Starting the M-Strata server..."};
  bool own_page{false};  // the next navigation is this window's own page (NavigateToString)
  bool stopping{false};
  bool exiting{false};
  bool restarting{false};
  bool told_about_tray{false};
  bool first_page{true};  // the first page opens on the Monitor; a restart's leaves the view to the page
};
App app;

void notify(const std::wstring& text) {
  NOTIFYICONDATAW note = app.tray;
  note.uFlags = NIF_INFO;
  note.dwInfoFlags = NIIF_NONE;
  (void)wcsncpy_s(note.szInfoTitle, kTitle, _TRUNCATE);
  (void)wcsncpy_s(note.szInfo, text.c_str(), _TRUNCATE);
  (void)Shell_NotifyIconW(NIM_MODIFY, &note);
}

void post_notice(std::wstring text) { PostMessageW(app.window, kNotice, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(text)))); }

void show_status(std::wstring text) {
  app.status = std::move(text);
  InvalidateRect(app.window, nullptr, TRUE);
}

void show_window() {
  ShowWindow(app.window, IsIconic(app.window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(app.window);
}

void open_in_browser(const std::wstring& url) { (void)ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void navigate(const std::string& url) {
  if (app.view == nullptr) {
    app.pending = url;
    return;
  }
  (void)app.view->Navigate(wide(url).c_str());
}

// The page this window shows when the server ended: what it said last (the sign-in link is never kept).
void show_server_ended(const int code) {
  std::wstring page = L"<!doctype html><meta charset=utf-8><title>M-Strata</title><body style=\"font:14px Segoe UI,sans-serif;"
                      L"background:#121417;color:#e8e8e8;margin:24px\"><h2>The M-Strata server stopped</h2><p>It ended with code " +
                      std::to_wstring(code) + L". Its last lines are below; the whole log is in the data folder's logs\\server.log. "
                      L"Right-click the tray icon and choose Restart the server.</p><pre style=\"white-space:pre-wrap;"
                      L"background:#1d2024;padding:12px;border-radius:8px\">";
  for (const auto& line : app.recent) page += html_escaped(line) + L"\n";
  page += L"</pre>";
  if (app.view != nullptr) {
    app.own_page = true;
    (void)app.view->NavigateToString(page.c_str());
  } else {
    show_status(L"The M-Strata server stopped (code " + std::to_wstring(code) + L"). Right-click the tray icon to restart it.");
  }
}

void resize_view() {
  if (app.controller == nullptr) return;
  RECT bounds{};
  GetClientRect(app.window, &bounds);
  (void)app.controller->put_Bounds(bounds);
}

void on_navigation_starting(ICoreWebView2NavigationStartingEventArgs* args) {
  LPWSTR uri = nullptr;
  if (FAILED(args->get_Uri(&uri)) || uri == nullptr) {
    (void)args->put_Cancel(TRUE);
    return;
  }
  const std::wstring target{uri};
  CoTaskMemFree(uri);
  // this window's own page (NavigateToString): an about:blank or data: navigation, right after it asked for one
  const bool own = app.own_page;
  app.own_page = false;
  if (own && (target == L"about:blank" || target.starts_with(L"data:"))) return;
  switch (w::navigation_for(narrow(target), app.origin)) {
    case w::Navigation::stay: return;
    case w::Navigation::browser:
      (void)args->put_Cancel(TRUE);
      open_in_browser(target);
      return;
    case w::Navigation::refuse: (void)args->put_Cancel(TRUE); return;
  }
}

void on_new_window(ICoreWebView2NewWindowRequestedEventArgs* args) {
  (void)args->put_Handled(TRUE);  // never a second web view: the page's origin here, any other address in the browser
  LPWSTR uri = nullptr;
  if (FAILED(args->get_Uri(&uri)) || uri == nullptr) return;
  const std::wstring target{uri};
  CoTaskMemFree(uri);
  const auto where = w::navigation_for(narrow(target), app.origin);
  if (where == w::Navigation::stay) (void)app.view->Navigate(target.c_str());
  if (where == w::Navigation::browser) open_in_browser(target);
}

using EnvironmentDone = Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using ControllerDone = Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>;
using NavigationStarting = Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>;
using NewWindow = Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>;

void view_ready(ICoreWebView2Controller* controller) {
  app.controller = controller;
  app.controller->AddRef();
  (void)app.controller->get_CoreWebView2(&app.view);
  if (ICoreWebView2Settings* settings = nullptr; SUCCEEDED(app.view->get_Settings(&settings)) && settings != nullptr) {
    (void)settings->put_IsStatusBarEnabled(FALSE);
    (void)settings->put_AreDevToolsEnabled(FALSE);
    (void)settings->put_IsZoomControlEnabled(TRUE);
    settings->Release();
  }
  EventRegistrationToken token{};
  auto* starting = new NavigationStarting([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
    on_navigation_starting(args);
    return S_OK;
  });
  (void)app.view->add_NavigationStarting(starting, &token);
  starting->Release();
  auto* opening = new NewWindow([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
    on_new_window(args);
    return S_OK;
  });
  (void)app.view->add_NewWindowRequested(opening, &token);
  opening->Release();
  resize_view();
  if (app.pending) {
    const std::string url = *app.pending;
    app.pending.reset();
    navigate(url);
  }
}

// The web view, created once the server listens: its profile lives in the data folder's private state folder
// (state/window), which exists by then (the server makes the data folder at its first start).
void create_view() {
  if (app.view_requested) return;
  app.view_requested = true;
  std::wstring profile;
  if (app.setup.data) {
    const auto folder = app.setup.data->state / "window";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (!error) profile = folder.wstring();
  }
  auto* environment_done = new EnvironmentDone([](HRESULT result, ICoreWebView2Environment* environment) {
    if (FAILED(result) || environment == nullptr) {
      show_status(L"The page needs the Microsoft Edge WebView2 Runtime, which is part of Windows 10 and 11 (install it from "
                  L"Microsoft if it was removed). Meanwhile the server runs: open " + wide(app.origin) + L" in a browser.");
      return S_OK;
    }
    app.environment = environment;
    app.environment->AddRef();
    auto* controller_done = new ControllerDone([](HRESULT made, ICoreWebView2Controller* controller) {
      if (SUCCEEDED(made) && controller != nullptr) view_ready(controller);
      else show_status(L"The page could not be shown here: open " + wide(app.origin) + L" in a browser.");
      return S_OK;
    });
    (void)app.environment->CreateCoreWebView2Controller(app.window, controller_done);
    controller_done->Release();
    return S_OK;
  });
  const HRESULT created = CreateCoreWebView2EnvironmentWithOptions(nullptr, profile.empty() ? nullptr : profile.c_str(), nullptr,
                                                                   environment_done);
  environment_done->Release();
  if (FAILED(created)) {
    show_status(L"The page needs the Microsoft Edge WebView2 Runtime, which is part of Windows 10 and 11 (install it from "
                L"Microsoft if it was removed). Meanwhile the server runs: open " + wide(app.origin) + L" in a browser.");
  }
}

void start_server() {
  app.origin.clear();
  app.key.reset();
  app.recent.clear();
  HWND window = app.window;
  const auto failed = app.server.start(
      app.setup.server, app.setup.arguments, app.setup.environment,  // the window's first environment, every start
      [window](std::string line) { PostMessageW(window, kServerLine, 0, reinterpret_cast<LPARAM>(new std::string(std::move(line)))); },
      [window](const int code) { PostMessageW(window, kServerExit, static_cast<WPARAM>(code), 0); });
  if (failed) {
    show_status(wide(*failed));
    return;
  }
  show_status(L"Starting the M-Strata server...");
}

void on_server_line(std::string line) {
  if (!w::loggable(line)) {
    if (const auto link = w::sign_in_url(line)) {
      KillTimer(app.window, kSignInTimer);
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
    app.key = w::server_key(app.setup);
    show_status(L"Opening the page...");
    create_view();
    SetTimer(app.window, kSignInTimer, 1500, nullptr);  // the sign-in line follows at once when the server prints one
  }
}

// Stops the server on a thread of its own (an orderly stop can take a while: the model stops first), then tells the
// window, which restarts it or quits.
void stop_server_then(const bool restart) {
  if (app.stopping) return;
  app.stopping = true;
  app.restarting = restart;
  show_status(restart ? L"Restarting the M-Strata server..." : L"Stopping the M-Strata server...");
  HWND window = app.window;
  std::thread([window] {
    (void)app.server.stop(std::chrono::seconds(60));
    PostMessageW(window, kServerStopped, 0, 0);
  }).detach();
}

// The tray's model requests run on a thread of their own and come back as a notification.
void model_request(std::string path, std::string body, std::wstring done) {
  const std::string origin = app.origin;
  const std::optional<std::string> key = app.key && app.key->key ? app.key->key : std::nullopt;
  std::thread([origin, key, path = std::move(path), body = std::move(body), done = std::move(done)] {
    const auto answer = w::loopback_request(origin, "POST", path, key ? &*key : nullptr, body, std::chrono::seconds(20));
    if (!answer) post_notice(L"The server did not answer.");
    else if (answer->status / 100 != 2) post_notice(L"The server refused: " + wide(answer->body.substr(0, 200)));
    else post_notice(done);
  }).detach();
}

void show_tray_menu() {
  HMENU menu = CreatePopupMenu();
  (void)AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open M-Strata");
  (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  app.menu_models.clear();
  std::optional<w::TrayModels> models;
  if (!app.origin.empty() && app.server.running() && app.key && (app.key->keyless || app.key->key)) {
    const auto answer = w::loopback_request(app.origin, "GET", "/v1/strata/models", app.key->key ? &*app.key->key : nullptr, "",
                                            std::chrono::milliseconds(1500));
    if (answer && answer->status == 200) models = w::tray_models(answer->body);
  }
  if (!models) {
    (void)AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, app.server.running() ? L"Models: open the page" : L"The server is not running");
  } else if (models->models.empty()) {
    (void)AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No models yet: add one on the Models tab");
  } else {
    for (std::size_t i = 0; i < models->models.size() && i < 50U; ++i) {
      const auto& model = models->models[i];
      app.menu_models.push_back(model.name);
      const UINT id = kMenuModelBase + static_cast<UINT>(i);
      if (model.state == "running") {
        (void)AppendMenuW(menu, MF_STRING | MF_CHECKED, id, (L"Stop " + wide(model.name)).c_str());
      } else if (model.state == "starting" || model.state == "loading" || model.state == "calibrating") {
        (void)AppendMenuW(menu, MF_STRING | MF_GRAYED, id, (wide(model.name) + L" (loading...)").c_str());
      } else {
        (void)AppendMenuW(menu, MF_STRING, id, (L"Start " + wide(model.name)).c_str());
      }
    }
  }
  (void)AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const UINT available = app.stopping ? static_cast<UINT>(MF_STRING | MF_GRAYED) : static_cast<UINT>(MF_STRING);
  (void)AppendMenuW(menu, available, kMenuRestart, L"Restart the server");
  (void)AppendMenuW(menu, available, kMenuExit, L"Exit");
  POINT at{};
  GetCursorPos(&at);
  SetForegroundWindow(app.window);  // so the menu closes when the user clicks elsewhere
  const UINT chosen = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, app.window, nullptr));
  DestroyMenu(menu);
  if (chosen == kMenuOpen) show_window();
  else if (chosen == kMenuRestart) stop_server_then(true);
  else if (chosen == kMenuExit) {
    app.exiting = true;
    ShowWindow(app.window, SW_HIDE);
    stop_server_then(false);
  } else if (chosen >= kMenuModelBase && chosen - kMenuModelBase < app.menu_models.size() && models) {
    const auto& name = app.menu_models[chosen - kMenuModelBase];
    const auto& model = models->models[chosen - kMenuModelBase];
    if (model.state == "running") model_request("/v1/strata/models/stop", "", L"Stopping " + wide(name));
    else model_request("/v1/strata/models/start", w::start_body(name), L"Starting " + wide(name) + L": the page's Monitor shows it load");
  }
  PostMessageW(app.window, WM_NULL, 0, 0);
}

void add_tray_icon() {
  app.tray.cbSize = sizeof(app.tray);
  app.tray.hWnd = app.window;
  app.tray.uID = 1;
  app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  app.tray.uCallbackMessage = kTrayMessage;
  app.tray.hIcon = app.icon_small;
  (void)wcsncpy_s(app.tray.szTip, kTitle, _TRUNCATE);
  (void)Shell_NotifyIconW(NIM_ADD, &app.tray);
  app.tray.uVersion = NOTIFYICON_VERSION_4;
  (void)Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == app.taskbar_created && app.taskbar_created != 0) {  // Explorer restarted: the icon again
    add_tray_icon();
    return 0;
  }
  switch (message) {
    case WM_SIZE: resize_view(); return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT box{};
      GetClientRect(window, &box);
      HBRUSH background = CreateSolidBrush(RGB(18, 20, 23));
      FillRect(dc, &box, background);
      DeleteObject(background);
      if (app.view == nullptr) {
        InflateRect(&box, -32, -32);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(232, 232, 232));
        HGDIOBJ font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, app.status.c_str(), -1, &box, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, font);
      }
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CLOSE:
      // closing keeps the server and the tray: Exit is on the tray's menu
      ShowWindow(window, SW_HIDE);
      if (!app.told_about_tray) {
        app.told_about_tray = true;
        notify(L"M-Strata keeps running in the tray. Right-click its icon to start or stop a model, or to exit.");
      }
      return 0;
    case WM_TIMER:
      if (wparam == kSignInTimer) {
        KillTimer(window, kSignInTimer);
        if (!app.origin.empty()) {  // no link: the page's own sign-in
          navigate(w::page_address(std::nullopt, app.origin, app.first_page));
          app.first_page = false;
        }
      }
      return 0;
    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP: show_window(); break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: show_tray_menu(); break;
        default: break;
      }
      return 0;
    case kServerLine: {
      std::unique_ptr<std::string> line{reinterpret_cast<std::string*>(lparam)};
      on_server_line(std::move(*line));
      return 0;
    }
    case kServerExit:
      if (!app.stopping) {
        show_server_ended(static_cast<int>(wparam));
        notify(L"The M-Strata server stopped. Right-click the tray icon to restart it.");
      }
      return 0;
    case kServerStopped:
      app.stopping = false;
      if (app.exiting) {
        DestroyWindow(window);
      } else if (app.restarting) {
        app.restarting = false;
        start_server();
      }
      return 0;
    case kShowYourself: show_window(); return 0;
    case kNotice: {
      std::unique_ptr<std::wstring> text{reinterpret_cast<std::wstring*>(lparam)};
      notify(*text);
      return 0;
    }
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
      // the PC shuts down or signs out: the server stops in order (unless Restart or Exit is stopping it already)
      if (wparam != 0 && !app.stopping) (void)app.server.stop(std::chrono::seconds(10));
      return 0;
    case WM_DESTROY:
      (void)Shell_NotifyIconW(NIM_DELETE, &app.tray);
      if (app.controller != nullptr) {
        (void)app.controller->Close();
        app.controller->Release();
        app.controller = nullptr;
      }
      if (app.view != nullptr) app.view->Release();
      app.view = nullptr;
      if (app.environment != nullptr) app.environment->Release();
      app.environment = nullptr;
      PostQuitMessage(0);
      return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
  }
}

std::vector<std::string> utf8_arguments() {
  int count = 0;
  LPWSTR* list = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> arguments;
  for (int i = 1; list != nullptr && i < count; ++i) arguments.push_back(narrow(list[i]));
  if (list != nullptr) LocalFree(list);
  return arguments;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  if (const auto set_awareness =
          reinterpret_cast<SetAwareness>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))) {
    (void)set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
  const auto arguments = utf8_arguments();
  auto setup = w::server_setup(arguments, secure_frontend::executable_directory());
  if (const auto* problem = std::get_if<std::string>(&setup)) {
    MessageBoxW(nullptr, wide(*problem).c_str(), kTitle, MB_OK | MB_ICONERROR);
    return 1;
  }
  app.setup = std::get<w::ServerSetup>(std::move(setup));
  // One window per data folder: a second start shows the first one's window instead of a second server.
  const std::wstring instance_name =
      L"Local\\M-Strata-window-" + std::to_wstring(std::hash<std::wstring>{}(app.setup.data ? app.setup.data->root.wstring()
                                                                                              : app.setup.server.wstring()));
  HANDLE single = CreateMutexW(nullptr, TRUE, instance_name.c_str());
  if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kWindowClass, nullptr)) PostMessageW(other, kShowYourself, 0, 0);
    CloseHandle(single);
    return 0;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  app.instance = instance;
  app.icon_large = make_icon(GetSystemMetrics(SM_CXICON));
  app.icon_small = make_icon(GetSystemMetrics(SM_CXSMICON));
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_procedure;
  window_class.hInstance = instance;
  window_class.hIcon = app.icon_large;
  window_class.hIconSm = app.icon_small;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) return 1;
  app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  app.window = CreateWindowExW(0, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr,
                               instance, nullptr);
  if (app.window == nullptr) return 1;
  add_tray_icon();
  ShowWindow(app.window, show);
  UpdateWindow(app.window);
  start_server();
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (app.server.running()) (void)app.server.stop(std::chrono::seconds(30));
  CoUninitialize();
  if (single != nullptr) CloseHandle(single);
  return static_cast<int>(message.wParam);
}
