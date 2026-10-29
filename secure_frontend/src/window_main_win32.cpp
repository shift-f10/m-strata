// strata-window on Windows (spec docs/superpowers/specs/2026-10-28-server-window-design.md, Part 2; the owner:
// "instead of a console application, we should make it into a window application ... minimized into the tray that
// has the start and stop models menu ... right click it to select exit or select a model to start and stop"). It runs
// strata-unified-server beside it as its child with no console, signs in with the server's one-time link, shows the
// page in a WebView2 view, and lives in the tray: closing the window hides it; the tray's menu opens it, shows the
// server's log, starts and stops the models, restarts the server and exits (the server stops in order first).
// window_shell.hpp holds the portable half; this file is the window, the Log, the tray and WebView2.

#include "secure_frontend/first_start.hpp"
#include "secure_frontend/window_shell.hpp"
#include "secure_frontend/win32_prelude.hpp"

#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>

#include <WebView2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iterator>
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
constexpr UINT kShowLog = WM_APP + 7;        // the page's Ctrl+Shift+L: the Log
constexpr UINT_PTR kSignInTimer = 1;         // the listening line came: the page's own address unless a link follows
constexpr UINT_PTR kLogTimer = 2;            // new lines for the Log's text (a burst of lines is drawn once)
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuRestart = 2;
constexpr UINT kMenuExit = 3;
constexpr UINT kMenuLog = 4;
constexpr UINT kMenuModelBase = 100;
constexpr UINT kSystemMenuLog = 0x0110;  // the title bar's menu (a WM_SYSCOMMAND id: below 0xF000, its low 4 bits clear)
constexpr int kLogCopy = 101;            // the Log's buttons
constexpr int kLogSave = 102;
constexpr int kLogFolder = 103;
constexpr wchar_t kLogClass[] = L"MStrataLog";
constexpr wchar_t kLogTitle[] = L"M-Strata - server log";
constexpr std::uintmax_t kLogBytes = 4U * 1024U * 1024U;
constexpr std::size_t kLogViewLines = 2000;  // the Log's lines in memory (as many as strata-tui's Log keeps)
constexpr std::size_t kLogViewBytes = 1024U * 1024U;
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
  // the server's lines (but the sign-in link) and the window's own notes: the Log, and the page shown when it ends
  w::LogLines lines{kLogViewLines, kLogViewBytes};
  HWND log_window{nullptr};  // the Log: the lines, Copy all, Save as..., Open the logs folder
  HWND log_text{nullptr};
  HWND log_note{nullptr};
  HFONT log_font{nullptr};
  HFONT log_ui_font{nullptr};
  std::uint64_t log_shown_first{0};  // the lines its text shows, [first, end)
  std::uint64_t log_shown_end{0};
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

// --------------------------------------------------------------------------- the Log
// The server's lines in a window of their own (the owner: "in the window app version we need to add a log capability,
// the console have it but this one do not so we can troubleshoot"): the newest last, following the newest unless
// scrolled up; Copy all, Save as..., Open the logs folder. The tray's Server log, the title bar's menu and
// Ctrl+Shift+L open it; closing it hides it (the lines stay).

int log_scaled(const int pixels) { return MulDiv(pixels, static_cast<int>(GetDpiForWindow(app.log_window)), 96); }

void log_say(const std::wstring& text) {
  if (app.log_note != nullptr) SetWindowTextW(app.log_note, text.c_str());
}

void make_log_fonts() {
  const int dpi = static_cast<int>(GetDpiForWindow(app.log_window));
  for (HFONT* font : {&app.log_font, &app.log_ui_font}) {
    if (*font != nullptr) DeleteObject(*font);
  }
  app.log_font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  app.log_ui_font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  for (HWND child : {GetDlgItem(app.log_window, kLogCopy), GetDlgItem(app.log_window, kLogSave), GetDlgItem(app.log_window, kLogFolder),
                     app.log_note}) {
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app.log_ui_font), TRUE);
  }
  SendMessageW(app.log_text, WM_SETFONT, reinterpret_cast<WPARAM>(app.log_font), TRUE);
}

void layout_log() {
  if (app.log_text == nullptr) return;  // (sizes sent while it is made)
  RECT box{};
  GetClientRect(app.log_window, &box);
  const int gap = log_scaled(8);
  const int height = log_scaled(28);
  int x = gap;
  for (const auto& [id, width] : {std::pair{kLogCopy, 96}, std::pair{kLogSave, 96}, std::pair{kLogFolder, 150}}) {
    MoveWindow(GetDlgItem(app.log_window, id), x, gap, log_scaled(width), height, TRUE);
    x += log_scaled(width) + gap;
  }
  MoveWindow(app.log_note, x, gap + log_scaled(6), std::max(0, static_cast<int>(box.right) - x - gap), height - log_scaled(6), TRUE);
  const int top = gap * 2 + height;
  MoveWindow(app.log_text, 0, top, box.right, std::max(0, static_cast<int>(box.bottom) - top), TRUE);
}

// The Log's text catches up with the lines kept: the new ones added at its end (the user's selection stays), or all of
// them again once lines it showed were dropped. It follows the newest line unless the user scrolled up.
void update_log_text() {
  if (app.log_text == nullptr || !IsWindowVisible(app.log_window)) return;
  const auto from = app.lines.newer_than(app.log_shown_first, app.log_shown_end);
  if (from && *from == app.lines.lines().size()) return;
  SCROLLINFO bar{};
  bar.cbSize = sizeof bar;
  bar.fMask = SIF_ALL;
  const bool follow = GetScrollInfo(app.log_text, SB_VERT, &bar) == FALSE || bar.nPos + static_cast<int>(bar.nPage) >= bar.nMax;
  const auto top = SendMessageW(app.log_text, EM_GETFIRSTVISIBLELINE, 0, 0);
  DWORD start = 0;
  DWORD end = 0;
  SendMessageW(app.log_text, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
  SendMessageW(app.log_text, WM_SETREDRAW, FALSE, 0);
  if (!from) {
    SetWindowTextW(app.log_text, wide(app.lines.text("\r\n")).c_str());
  } else {
    std::string added;
    for (std::size_t i = *from; i < app.lines.lines().size(); ++i) added += app.lines.lines()[i] + "\r\n";
    const int length = GetWindowTextLengthW(app.log_text);
    SendMessageW(app.log_text, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    SendMessageW(app.log_text, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wide(added).c_str()));
    SendMessageW(app.log_text, EM_SETSEL, start, static_cast<LPARAM>(end));
  }
  app.log_shown_first = app.lines.first();
  app.log_shown_end = app.lines.end();
  if (follow) {
    SendMessageW(app.log_text, WM_VSCROLL, SB_BOTTOM, 0);
  } else {
    SendMessageW(app.log_text, EM_LINESCROLL, 0, top - SendMessageW(app.log_text, EM_GETFIRSTVISIBLELINE, 0, 0));
  }
  SendMessageW(app.log_text, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(app.log_text, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE);
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
  if (app.log_text != nullptr && IsWindowVisible(app.log_window)) SetTimer(app.log_window, kLogTimer, 100, nullptr);
}

void copy_log() {
  const std::wstring text = wide(app.lines.text("\r\n"));
  bool copied = false;
  if (OpenClipboard(app.log_window)) {
    (void)EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
      if (void* target = GlobalLock(memory); target != nullptr) {
        std::memcpy(target, text.c_str(), bytes);
        (void)GlobalUnlock(memory);
        copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;  // the clipboard owns it now
      }
      if (!copied) (void)GlobalFree(memory);
    }
    (void)CloseClipboard();
  }
  log_say(copied ? L"Copied " + std::to_wstring(app.lines.lines().size()) + L" lines: paste them where you send them."
                 : L"The lines could not be copied.");
}

void save_log() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  (void)localtime_s(&local, &now);
  wchar_t file[4096] = {};
  (void)wcsncpy_s(file, wide(w::saved_log_name(local)).c_str(), _TRUNCATE);
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof dialog;
  dialog.hwndOwner = app.log_window;
  dialog.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
  dialog.lpstrFile = file;
  dialog.nMaxFile = static_cast<DWORD>(std::size(file));
  dialog.lpstrDefExt = L"txt";
  dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&dialog)) return;  // cancelled
  const std::filesystem::path chosen{std::wstring{file}};
  const auto failed = app.lines.save(chosen);
  log_say(failed ? wide(*failed) + L"."
                 : L"Saved " + std::to_wstring(app.lines.lines().size()) + L" lines to " + chosen.filename().wstring() + L".");
}

LRESULT CALLBACK log_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE: layout_log(); return 0;
    case WM_TIMER:
      if (wparam == kLogTimer) {
        KillTimer(window, kLogTimer);
        update_log_text();
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kLogCopy) copy_log();
      else if (LOWORD(wparam) == kLogSave) save_log();
      else if (LOWORD(wparam) == kLogFolder && app.setup.data) {
        (void)ShellExecuteW(nullptr, L"open", app.setup.data->logs.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      return 0;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                   suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      make_log_fonts();
      layout_log();
      return 0;
    }
    case WM_CTLCOLORSTATIC:
      if (reinterpret_cast<HWND>(lparam) == app.log_text) {  // read-only text on the window colour, not the dialog grey
        SetTextColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
      }
      return DefWindowProcW(window, message, wparam, lparam);
    case WM_CLOSE: ShowWindow(window, SW_HIDE); return 0;  // the lines stay: Server log shows them again
    default: return DefWindowProcW(window, message, wparam, lparam);
  }
}

void show_log() {
  if (app.log_window == nullptr) {
    const int dpi = static_cast<int>(GetDpiForWindow(app.window));
    app.log_window = CreateWindowExW(0, kLogClass, kLogTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1000, dpi, 96),
                                     MulDiv(680, dpi, 96), nullptr, nullptr, app.instance, nullptr);
    if (app.log_window == nullptr) return;
    for (const auto& [id, text] : {std::pair{kLogCopy, L"Copy all"}, std::pair{kLogSave, L"Save as..."},
                                   std::pair{kLogFolder, L"Open the logs folder"}}) {
      HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, app.log_window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.instance, nullptr);
      if (id == kLogFolder) EnableWindow(button, app.setup.data ? TRUE : FALSE);  // (no data folder with --config)
    }
    app.log_note = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, app.log_window,
                                   nullptr, app.instance, nullptr);
    app.log_text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL, 0, 0, 0,
                                   0, app.log_window, nullptr, app.instance, nullptr);
    SendMessageW(app.log_text, EM_SETLIMITTEXT, 0, 0);  // no 32K limit on the lines added
    log_say(std::wstring{L"The server's newest lines, as its console prints them."} +
            (app.setup.data ? L" Every line is in the data folder's logs\\server.log." : L""));
    make_log_fonts();
    layout_log();
  }
  ShowWindow(app.log_window, IsIconic(app.log_window) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(app.log_window);
  update_log_text();
}

// Ctrl+Shift+L: the Log.
bool log_keys(const UINT key) {
  return key == 'L' && GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState(VK_MENU) >= 0;
}

// The page this window shows when the server ended: what it said last (the sign-in link is never kept).
void show_server_ended(const int code) {
  std::wstring page = L"<!doctype html><meta charset=utf-8><title>M-Strata</title><body style=\"font:14px Segoe UI,sans-serif;"
                      L"background:#121417;color:#e8e8e8;margin:24px\"><h2>The M-Strata server stopped</h2><p>It ended with code " +
                      std::to_wstring(code) + L". Its last lines are below; every line is in the tray menu's Server log and in "
                      L"the data folder's logs\\server.log. Right-click the tray icon and choose Restart the server.</p><pre "
                      L"style=\"white-space:pre-wrap;background:#1d2024;padding:12px;border-radius:8px\">";
  for (const auto& line : app.lines.last(kRecentLines)) page += html_escaped(line) + L"\n";
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
using AcceleratorKey =
    Handler<ICoreWebView2AcceleratorKeyPressedEventHandler, ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs*>;

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
  auto* keys = new AcceleratorKey([](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
    COREWEBVIEW2_KEY_EVENT_KIND kind{};
    UINT key = 0;
    if (SUCCEEDED(args->get_KeyEventKind(&kind)) && kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && SUCCEEDED(args->get_VirtualKey(&key)) &&
        log_keys(key)) {
      (void)args->put_Handled(TRUE);
      PostMessageW(app.window, kShowLog, 0, 0);  // (not from inside the view's event)
    }
    return S_OK;
  });
  (void)app.controller->add_AcceleratorKeyPressed(keys, &token);
  keys->Release();
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
  record("[strata-window] starting the server");
  HWND window = app.window;
  const auto failed = app.server.start(
      app.setup.server, app.setup.arguments, app.setup.environment,  // the window's first environment, every start
      [window](std::string line) { PostMessageW(window, kServerLine, 0, reinterpret_cast<LPARAM>(new std::string(std::move(line)))); },
      [window](const int code) { PostMessageW(window, kServerExit, static_cast<WPARAM>(code), 0); });
  if (failed) {
    record("[strata-window] " + *failed);
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
    return;  // the one-time link goes to the view only: never to the log file, the Log or the screen
  }
  record(line);
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
  record(restart ? "[strata-window] restarting the server" : "[strata-window] stopping the server (Exit)");
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
  (void)AppendMenuW(menu, MF_STRING, kMenuLog, L"Server log");
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
  else if (chosen == kMenuLog) show_log();
  else if (chosen == kMenuRestart) stop_server_then(true);
  else if (chosen == kMenuExit) {
    app.exiting = true;
    ShowWindow(app.window, SW_HIDE);
    if (app.log_window != nullptr) ShowWindow(app.log_window, SW_HIDE);
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
      record("[strata-window] the server ended (exit code " + std::to_string(static_cast<int>(wparam)) + ")");
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
    case kShowLog: show_log(); return 0;
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0U) == kSystemMenuLog) {
        show_log();
        return 0;
      }
      return DefWindowProcW(window, message, wparam, lparam);
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
      if (app.log_window != nullptr) DestroyWindow(app.log_window);
      app.log_window = nullptr;
      app.log_text = nullptr;
      app.log_note = nullptr;
      for (HFONT font : {app.log_font, app.log_ui_font}) {
        if (font != nullptr) DeleteObject(font);
      }
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
  WNDCLASSEXW log_class = window_class;
  log_class.lpfnWndProc = log_procedure;
  log_class.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  log_class.lpszClassName = kLogClass;
  if (RegisterClassExW(&log_class) == 0) return 1;
  app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  app.window = CreateWindowExW(0, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860, nullptr, nullptr,
                               instance, nullptr);
  if (app.window == nullptr) return 1;
  if (HMENU system = GetSystemMenu(app.window, FALSE); system != nullptr) {  // the title bar's menu: the Log
    (void)AppendMenuW(system, MF_SEPARATOR, 0, nullptr);
    (void)AppendMenuW(system, MF_STRING, kSystemMenuLog, L"Server log\tCtrl+Shift+L");
  }
  add_tray_icon();
  ShowWindow(app.window, show);
  UpdateWindow(app.window);
  start_server();
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_KEYDOWN) {
      // Ctrl+Shift+L in this process's windows (the page's own keys come through the view's AcceleratorKeyPressed)
      if (log_keys(static_cast<UINT>(message.wParam))) {
        show_log();
        continue;
      }
      // Ctrl+A in the Log's text selects all of it
      if (message.hwnd == app.log_text && message.wParam == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(app.log_text, EM_SETSEL, 0, -1);
        continue;
      }
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (app.server.running()) (void)app.server.stop(std::chrono::seconds(30));
  CoUninitialize();
  if (single != nullptr) CloseHandle(single);
  return static_cast<int>(message.wParam);
}
