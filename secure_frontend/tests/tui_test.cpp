// strata-tui (spec docs/superpowers/specs/2026-10-28-server-tui-design.md; the owner: "a TUI with the same settings and
// options ... monitoring the default view in a nice graph dashboard"): the screen and its VT output, the keys, the views
// drawn from the server's answers, and the keys' requests.
#include "secure_frontend/tui_app.hpp"
#include "secure_frontend/tui_input.hpp"
#include "secure_frontend/tui_screen.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

namespace t = secure_frontend::tui;

// A row of the screen as UTF-8, as the terminal shows it (a wide character's right half is drawn with it).
std::string row_text(const t::Screen& screen, const int y) {
  std::string out;
  for (int x = 0; x < screen.width(); ++x) {
    const t::Cell& cell = screen.at(x, y);
    if (cell.glyph == t::kWideTail) continue;
    out += t::utf8(cell.glyph);
    for (const char32_t mark : cell.marks) {
      if (mark != 0) out += t::utf8(mark);
    }
  }
  return out;
}

std::string all_text(const t::Screen& screen) {
  std::string out;
  for (int y = 0; y < screen.height(); ++y) out += row_text(screen, y) + "\n";
  return out;
}

// The first row whose text holds `part` (-1: none).
int row_with(const t::Screen& screen, const std::string& part) {
  for (int y = 0; y < screen.height(); ++y) {
    if (row_text(screen, y).find(part) != std::string::npos) return y;
  }
  return -1;
}

// The boxes whose top edge is row y: each one's left and right corner columns.
std::vector<std::pair<int, int>> boxes_on(const t::Screen& screen, const int y) {
  std::vector<std::pair<int, int>> out;
  int left = -1;
  for (int x = 0; y >= 0 && x < screen.width(); ++x) {
    const char32_t glyph = screen.at(x, y).glyph;
    if (glyph == char32_t{0x256D}) left = x;
    if (glyph == char32_t{0x256E} && left >= 0) {
      out.emplace_back(left, x);
      left = -1;
    }
  }
  return out;
}

// The value on a panel's labelled line: what follows its 15-cell label column, up to the panel's right edge.
std::string value_of(const t::Screen& screen, const std::string& label) {
  const int y = row_with(screen, label);
  if (y < 0) return "(no " + label + ")";
  const std::string row = row_text(screen, y);
  std::string rest = row.substr(row.find(label) + 15U);
  rest = rest.substr(0, rest.find("\xE2\x94\x82"));  // (U+2502, the panel's edge)
  while (!rest.empty() && rest.back() == ' ') rest.pop_back();
  return rest;
}

// The text inside the box whose top edge holds `title`, its lines joined by single spaces (a wrapped question read whole).
std::string box_words(const t::Screen& screen, const std::string& title) {
  const int top = row_with(screen, title);
  const auto boxes = boxes_on(screen, top);
  if (top < 0 || boxes.size() != 1U) return "(no box " + title + ")";
  const auto [left, right] = boxes.front();
  std::string words;
  for (int y = top + 1; y < screen.height() && screen.at(left, y).glyph != char32_t{0x2570}; ++y) {
    for (int x = left + 1; x < right; ++x) {
      const char32_t glyph = screen.at(x, y).glyph;
      if (glyph == char32_t{' '} && (words.empty() || words.back() == ' ')) continue;
      words += t::utf8(glyph);
    }
    if (!words.empty() && words.back() != ' ') words += ' ';
  }
  while (!words.empty() && words.back() == ' ') words.pop_back();
  return words;
}

bool has_braille(const t::Screen& screen) {
  for (int y = 0; y < screen.height(); ++y) {
    for (int x = 0; x < screen.width(); ++x) {
      const auto glyph = static_cast<std::uint32_t>(screen.at(x, y).glyph);
      if (glyph > 0x2800U && glyph <= 0x28FFU) return true;
    }
  }
  return false;
}

void the_screen_draws_text_boxes_and_gauges() {
  t::Screen screen(12, 4);
  expect(screen.text(1, 0, "M-Strata server", t::Style{}) == 11 && row_text(screen, 0) == " M-Strata se", "text is clipped at the edge");
  screen.field(0, 1, 6, "abcdefgh", t::Style{});
  expect(row_text(screen, 1).substr(0, 8) == "abcde" + t::utf8(char32_t{0x2026}), "a field too long ends in an ellipsis: " + row_text(screen, 1));
  screen.field(0, 2, 6, "ab", t::Style{});
  expect(row_text(screen, 2).substr(0, 6) == "ab    ", "a short field is padded");
  t::Screen boxed(8, 3);
  boxed.box(t::Rect{0, 0, 8, 3}, "Hi", t::Style{}, t::Style{});
  expect(boxed.at(0, 0).glyph == char32_t{0x256D} && boxed.at(7, 0).glyph == char32_t{0x256E} && boxed.at(0, 2).glyph == char32_t{0x2570} &&
             boxed.at(7, 2).glyph == char32_t{0x256F} && boxed.at(2, 0).glyph == U'H',
         "a rounded box with its title in the top edge: " + row_text(boxed, 0));
  t::Screen bar(4, 1);
  bar.gauge(0, 0, 4, 0.5, t::Style{}, t::Style{});
  expect(bar.at(0, 0).glyph == char32_t{0x2588} && bar.at(1, 0).glyph == char32_t{0x2588} && bar.at(2, 0).glyph == char32_t{0x00B7},
         "half a gauge is two whole cells of four: " + row_text(bar, 0));
  bar.gauge(0, 0, 4, 0.5 + 1.0 / 8.0, t::Style{}, t::Style{});
  expect(bar.at(2, 0).glyph == char32_t{0x258C}, "and a half cell more is a half block: " + row_text(bar, 0));
  // text from the server's answers cannot carry a terminal sequence: C0, DEL and C1 controls (U+009B is a one-byte
  // CSI on terminals that honour 8-bit controls) are drawn as spaces
  t::Screen hostile(8, 2);
  hostile.text(0, 0, "a\x1b[2Jb", t::Style{});
  hostile.field(0, 1, 8, "c\xC2\x9B" "2Jd\x7f", t::Style{});
  const std::string drawn = t::render(hostile, nullptr, t::Capabilities{});
  expect(row_text(hostile, 0) == "a [2Jb  " && row_text(hostile, 1) == "c 2Jd   " && drawn.find("\x1b[2Jb") == std::string::npos &&
             drawn.find("\xC2\x9B") == std::string::npos,
         "controls are spaces: [" + row_text(hostile, 0) + "] [" + row_text(hostile, 1) + "]");
  expect(t::code_points("\xE2\x96\x88x") == std::u32string{char32_t{0x2588}, U'x'} && t::text_width("\xC3\xA9t\xC3\xA9") == 3 &&
             t::code_points("\xFF") == std::u32string{char32_t{0xFFFD}},
         "UTF-8 in and out");
  // a wide character (CJK, an emoji) takes two cells and a combining mark none, as on the terminal, so a row stays
  // aligned: U+4E2D, then e with U+0301
  const std::string han = "\xE4\xB8\xAD";
  t::Screen wide(6, 1);
  expect(wide.text(0, 0, han + "ae\xCC\x81" "b", t::Style{}) == 5 && wide.at(0, 0).glyph == char32_t{0x4E2D} &&
             wide.at(1, 0).glyph == t::kWideTail && wide.at(2, 0).glyph == U'a' && wide.at(3, 0).glyph == U'e' &&
             wide.at(3, 0).marks[0] == char32_t{0x0301} && wide.at(4, 0).glyph == U'b' && wide.at(5, 0).glyph == U' ',
         "two cells for U+4E2D, none for U+0301: " + row_text(wide, 0));
  expect(t::text_width(han + "ae\xCC\x81") == 4 && t::text_width("\xF0\x9F\x98\x80") == 2 && t::text_width("\xE2\x80\x8D") == 0,
         "the widths: a wide character, a combining mark, an emoji (U+1F600), a zero-width joiner");
  wide.text(1, 0, "z", t::Style{});
  expect(wide.at(0, 0).glyph == U' ' && wide.at(1, 0).glyph == U'z', "writing over half of a wide character blanks its other half");
  t::Screen edge(3, 1);
  expect(edge.text(2, 0, han, t::Style{}) == 0 && edge.at(2, 0).glyph == U' ', "a wide character does not fit in the last cell");
  t::Screen cut(5, 1);
  cut.field(0, 0, 5, han + han + han, t::Style{});
  expect(row_text(cut, 0) == han + han + t::utf8(char32_t{0x2026}), "a field is cut by cells: " + row_text(cut, 0));
}

void charts_are_braille_or_blocks() {
  // two values a cell: the bottom row's left dot (dot 7) for 0, the top row's right dot (dot 4) for the top
  t::Screen screen(1, 1);
  const std::vector<double> values{0.0, 10.0};
  screen.chart(t::Rect{0, 0, 1, 1}, values, 10.0, t::Style{}, t::Capabilities{true, true});
  const auto glyph = static_cast<std::uint32_t>(screen.at(0, 0).glyph);
  expect(glyph >= 0x2800U && (glyph & 0x40U) != 0U && (glyph & 0x08U) != 0U, "braille: the low left dot and the high right one");
  t::Screen blocks(2, 2);
  blocks.chart(t::Rect{0, 0, 2, 2}, std::vector<double>{5.0, 10.0}, 10.0, t::Style{}, t::Capabilities{true, false});
  expect(blocks.at(0, 1).glyph == char32_t{0x2588} && blocks.at(0, 0).glyph == U' ' && blocks.at(1, 0).glyph == char32_t{0x2588},
         "blocks: half is the lower cell, full the column: " + all_text(blocks));
  t::Screen empty(3, 1);
  empty.chart(t::Rect{0, 0, 3, 1}, std::vector<double>{}, 0.0, t::Style{}, t::Capabilities{});
  expect(row_text(empty, 0) == "   ", "no values, no dots");
  // an unknown sample (null in /metrics, NaN here) draws nothing, never a 0, and the line does not join across it
  const double unknown = std::numeric_limits<double>::quiet_NaN();
  t::Screen gap(1, 1);
  gap.chart(t::Rect{0, 0, 1, 1}, std::vector<double>{unknown, 10.0}, 10.0, t::Style{}, t::Capabilities{true, true});
  const auto gap_glyph = static_cast<std::uint32_t>(gap.at(0, 0).glyph);
  expect(gap_glyph == 0x2800U + 0x08U, "braille: no dot for the unknown sample, only the known one at the top");
  t::Screen gap_blocks(2, 2);
  gap_blocks.chart(t::Rect{0, 0, 2, 2}, std::vector<double>{unknown, 10.0}, 10.0, t::Style{}, t::Capabilities{true, false});
  expect(gap_blocks.at(0, 0).glyph == U' ' && gap_blocks.at(0, 1).glyph == U' ' && gap_blocks.at(1, 1).glyph == char32_t{0x2588},
         "blocks: an empty column for the unknown sample: " + all_text(gap_blocks));
}

void only_the_changes_are_written() {
  t::Screen before(4, 2);
  before.text(0, 0, "abcd", t::Style{});
  const std::string full = t::render(before, nullptr, t::Capabilities{});
  expect(full.find("\x1b[2J") != std::string::npos && full.find("abcd") != std::string::npos, "a first frame clears and writes all");
  t::Screen after = before;
  after.text(2, 1, "Z", t::Style{196, -1, true});
  const std::string diff = t::render(after, &before, t::Capabilities{});
  expect(diff == "\x1b[2;3H\x1b[0;1;38;5;196mZ\x1b[0m",
         "the next frame moves to the changed cell and writes it with its style (bold, colour 196) only: " + std::to_string(diff.size()) +
             " bytes");
  expect(t::render(after, &after, t::Capabilities{}).empty(), "no change, nothing written");
  const std::string plain = t::render(after, &before, t::Capabilities{false, true});
  expect(plain == "\x1b[2;3H\x1b[0;1mZ\x1b[0m", "NO_COLOR: bold, no colours");
  // a wide character is written once for its two cells; writing over one half rewrites the other half as a space, so
  // the terminal keeps no stale half
  t::Screen letters(4, 1);
  letters.text(0, 0, "abcd", t::Style{});
  t::Screen han_over = letters;
  han_over.text(0, 0, "\xE4\xB8\xAD", t::Style{});
  const std::string han_full = t::render(han_over, nullptr, t::Capabilities{});
  expect(han_full.find("\xE4\xB8\xAD" "cd") != std::string::npos && han_full.find('\0') == std::string::npos,
         "a full frame: the wide character, then the cells after its two");
  expect(t::render(han_over, &letters, t::Capabilities{}) == "\x1b[1;1H\x1b[0m\xE4\xB8\xAD\x1b[0m", "the wide character over two letters");
  t::Screen x_over = han_over;
  x_over.text(1, 0, "x", t::Style{});
  expect(t::render(x_over, &han_over, t::Capabilities{}) == "\x1b[1;1H\x1b[0m x\x1b[0m", "its right half overwritten: both cells written");
}

void keys_decode_from_vt_sequences() {
  using K = t::Key::Kind;
  t::KeyDecoder decoder;
  const auto keys = decoder.feed("\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1b[5~\x1b[6~\x1b[3~\x1b[Z\x1bOP\x1b[15~\x1b[24~\r\n\t\x7f\x03q");
  const std::vector<t::Key> expected{{K::up, {}, 0},        {K::down, {}, 0},    {K::right, {}, 0},     {K::left, {}, 0},
                                     {K::home, {}, 0},      {K::end, {}, 0},     {K::page_up, {}, 0},   {K::page_down, {}, 0},
                                     {K::del, {}, 0},       {K::back_tab, {}, 0}, {K::function, {}, 1}, {K::function, {}, 5},
                                     {K::function, {}, 12}, {K::enter, {}, 0},   {K::tab, {}, 0},       {K::backspace, {}, 0},
                                     {K::ctrl, "c", 0},     {K::character, "q", 0}};
  expect(keys == expected, "the arrows, Home/End, PageUp/Down, Delete, Shift-Tab, F1/F5/F12, Enter, Tab, Backspace, Ctrl-C, q: " +
                               std::to_string(keys.size()) + " keys");
  // a sequence and a character split across reads
  t::KeyDecoder split;
  expect(split.feed("\x1b[").empty() && split.holding(), "half a sequence is held");
  const auto rest = split.feed("1~\xC3");
  expect(rest.size() == 1U && rest[0].kind == K::home && split.holding(), "then finished; half a character held");
  const auto e = split.feed("\xA9");
  expect(e.size() == 1U && e[0].kind == K::character && e[0].text == "\xC3\xA9", "then the whole character");
  // the mouse wheel (SGR reports) and a lone Escape
  const auto wheel = split.feed("\x1b[<64;10;5M\x1b[<65;10;5M\x1b[<0;3;3M");
  expect(wheel.size() == 2U && wheel[0].kind == K::wheel_up && wheel[1].kind == K::wheel_down, "the wheel up and down; a click is nothing");
  expect(split.feed("\x1b").empty() && split.flush() == std::vector<t::Key>{{K::escape, {}, 0}}, "Escape alone, after a pause");
}

const std::string kMetrics =
    R"({"engine":{"model":"Qwen3.8-IQ3_S","max_context":131072,"health":"running"},)"
    R"("live":{"state":"generating","queued":0,"prompt_tokens":812,"generated":40,"max_tokens":512,"elapsed_s":3.5,"tok_s":21.4,"prefill_tok_s":640.0},)"
    // the last requests, the newest first (as serve_monitor keeps them)
    R"("requests":[{"time":1761700002,"model":"Qwen3.8-IQ3_S","finish":"stop","prompt_tokens":100,"reused":0,"output_tokens":64,)"
    R"("prompt_ms":200.0,"decode_ms":3000.0,"decode_tok_s":21.3,"duration_s":3.2},)"
    R"({"time":1761700001,"model":"Qwen3.8-IQ3_S","finish":"stop","prompt_tokens":90,"reused":0,"output_tokens":32,)"
    R"("prompt_ms":180.0,"decode_ms":1800.0,"decode_tok_s":18.0,"duration_s":2.0},)"
    R"({"time":1761700000,"model":"Qwen3.8-IQ3_S","finish":"length","prompt_tokens":80,"reused":0,"output_tokens":16,)"
    R"("prompt_ms":160.0,"decode_ms":1000.0,"decode_tok_s":15.0,"duration_s":1.2}],"requests_kept":3,)"
    R"("totals":{"since":1761699000,"requests":3,"prompt_tokens":270,"output_tokens":112,"reused":0},)"
    R"("hardware":{"cpu":35.0,"ram_used":40000000000,"ram_total":96000000000,"gpu_util":88.0,"gpu_mem_used":14000000000,)"
    R"("gpu_mem_total":17171480576,"gpu_temp":61.0,"gpu_power":245.0,"gpu_power_limit":320.0,"gpu_pcie_gen":4,"gpu_pcie_gen_max":4,"gpu_pcie_width":16},)"
    R"("hardware_static":{"gpu_name":"NVIDIA GeForce RTX 4080 SUPER","gpu_note":null,"threads":24},)"
    R"("history":{"tok_s":[0,5,12,20,21.4],"cpu":[10,20,30,35,35],"gpu_util":[50,70,80,88,88],"gpu_mem_used":[1e9,5e9,1e10,1.4e10,1.4e10],)"
    R"("gpu_temp":[40,50,55,60,61],"gpu_power":[50,100,200,240,245]},"time":1761700003})";

// /metrics with the readings added after the first one: the disks, the engine's memory and whether it is loaded, the
// conversation cache
const std::string kMetricsFull =
    R"({"engine":{"model":"Qwen3.8-IQ3_S","max_context":131072,"health":"generating","loaded":true,"unloaded":false,)"
    R"("memory":{"experts_ram":120,"experts_ram_bytes":37795712205,"experts_vram":40,"process_bytes":13207024435}},)"
    R"("live":{"state":"generating","queued":1,"prompt_tokens":812,"generated":40,"max_tokens":512,"elapsed_s":3.5,"tok_s":21.4,)"
    R"("prefill_tok_s":640.0},)"
    R"("requests":[{"time":1761700002,"model":"Qwen3.8-IQ3_S","finish":"stop","prompt_tokens":100,"reused":64,"output_tokens":64,)"
    R"("prompt_ms":200.0,"decode_ms":3000.0,"decode_tok_s":21.3,"duration_s":3.2}],"requests_kept":1,)"
    R"("totals":{"since":1761699000,"requests":1,"prompt_tokens":100,"output_tokens":64,"reused":64},)"
    R"("hardware":{"cpu":35.0,"ram_used":40000000000,"ram_total":96000000000,"gpu_util":88.0,"gpu_mem_used":14000000000,)"
    R"("gpu_mem_total":17171480576,"gpu_temp":61.0,"gpu_power":245.0,"gpu_power_limit":320.0,"gpu_pcie_gen":4,"gpu_pcie_gen_max":4,)"
    R"("gpu_pcie_width":16,"disk_read_mb":120.4,"disk_write_mb":3.2},)"
    R"("hardware_static":{"gpu_name":"NVIDIA GeForce RTX 4080 SUPER","gpu_note":null,"threads":24},)"
    R"("history":{"tok_s":[0,21.4],"cpu":[30,35],"gpu_util":[80,88],"gpu_mem_used":[1.3e10,1.4e10],"gpu_temp":[60,61],)"
    R"("gpu_power":[240,245],"disk_read_mb":[2000,120.4]},)"
    R"("conversation_cache":{"enabled":true,"budget_mib":4096,"slots":4,"supported":true,"parked":2,"bytes":1288490189,)"
    R"("evictions":1,"parks":5,"restores":3,"last_event":"restored","last_tokens":512,"last_at":1761699991,"requests":10,)"
    R"("requests_reused":3,"reused_tokens":1234,"prompt_tokens":3000,"last_reused":512,"last_prompt":600},"time":1761700003})";

// The engine unloaded after idling (no context, no memory figures) and the cache with no engine to ask.
const std::string kMetricsUnloaded =
    R"({"engine":{"model":"Qwen3.8-IQ3_S","max_context":null,"health":"unloaded","loaded":false,"unloaded":true},)"
    R"("live":{"state":"unloaded","queued":0},"hardware":{"cpu":5.0,"disk_read_mb":null,"disk_write_mb":null},)"
    R"("history":{"tok_s":[0],"disk_read_mb":[null]},)"
    R"("conversation_cache":{"enabled":true,"budget_mib":4096,"slots":null,"supported":null,"parked":0,"bytes":0,"evictions":0,)"
    R"("parks":0,"restores":0,"last_event":null,"last_tokens":null,"last_at":null,"requests":0,"requests_reused":0,)"
    R"("reused_tokens":0,"prompt_tokens":0,"last_reused":null,"last_prompt":null},"time":5})";

const std::string kModels =
    R"({"object":"list","running":"Qwen3.8","state":"running","error":null,"data":[)"
    R"({"name":"Qwen3.8","profile":"qwen","state":"running","error":null,"page_card":true,"folder":"D:/models/q","options":[)"
    R"({"name":"thinking","kind":"choice","label":"Thinking","help":"","default":"high","choices":["none","low","high"],"value":"high"},)"
    R"({"name":"web_search","kind":"boolean","label":"Web search","help":"","default":true,"value":true},)"
    R"({"name":"max_context","kind":"integer","label":"Context length","help":"","default":131072,"min":4096,"max":262144,"value":131072}],)"
    R"("calibration":{"state":"fresh","first_start":false,"running":null,"before":80.0,"after":96.0,"date":"2026-10-29T01:00:00Z","args":["--spec","6"],"auto":false,"error":null}},)"
    R"({"name":"MiniMax","profile":"minimax","state":"stopped","error":null,"options":[],)"
    R"("calibration":{"state":"missing","first_start":false,"running":null,"args":[],"auto":false,"error":"the engine did not start",)"
    R"("report":{"file":"calibration-MiniMax-20261030-041646.md","date":"2026-10-30T04:16:46Z"},)"
    R"("prefill":{"state":"done","chunk":1024,"gemm":"blas","tok_s":14.9,"default_tok_s":14.9,"date":"2026-10-29T01:14:17Z"}}}]})";

t::Key character(const std::string& text) { return t::Key{t::Key::Kind::character, text, 0}; }
t::Key plain(const t::Key::Kind kind) { return t::Key{kind, {}, 0}; }

void the_monitor_draws_the_servers_readings() {
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  app.answer({"metrics", "GET", "/metrics", {}}, 200, kMetrics);
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  expect(app.view() == t::View::monitor, "the Monitor is the first view");
  t::Screen screen(160, 48);
  app.draw(screen, 1761700003);
  const std::string text = all_text(screen);
  for (const char* part : {"M-Strata", "http://127.0.0.1:8080", "Qwen3.8 (running)", "1 Monitor", "2 Models", "Decode", "21.4 tok/s",
                           "GPU use", "88%", "CPU", "35%", "GPU memory", "GPU temperature", "61 C", "GPU power", "245 W", "Engine",
                           "Qwen3.8-IQ3_S", "generating", "Machine", "RTX 4080 SUPER", "Last requests", "21.3"}) {
    expect(text.find(part) != std::string::npos, std::string{"the dashboard shows "} + part);
  }
  expect(row_text(screen, 47).rfind("  q  Quit   Tab  View", 0) == 0U, "the key bar: " + row_text(screen, 47));
  expect(has_braille(screen), "its charts are drawn in braille");
  // the last requests: the newest on the first line under the panel's header
  int header = -1;
  for (int y = 0; y < screen.height(); ++y) {
    const std::string line = row_text(screen, y);
    if (line.find("time") != std::string::npos && line.find("prefill") != std::string::npos && line.find("finish") != std::string::npos) {
      header = y;
    }
  }
  expect(header >= 0 && row_text(screen, header + 1).find("21.3") != std::string::npos &&
             row_text(screen, header + 2).find("18.0") != std::string::npos && row_text(screen, header + 3).find("15.0") != std::string::npos,
         "the newest request first: " + (header >= 0 ? row_text(screen, header + 1) : std::string{"no header"}));
  // a failed poll (a refusal, no answer) keeps the last readings on screen, and says so on the status line until that
  // poll answers again
  app.answer({"metrics", "GET", "/metrics", {}}, 503, R"({"error":{"message":"busy","type":"server_error","param":null,"code":null}})");
  expect(app.status() == "metrics: busy (HTTP 503)", "a refusal: " + app.status());
  app.answer({"metrics", "GET", "/metrics", {}}, 0, "");
  expect(app.status() == "No answer from the server at http://127.0.0.1:8080", "no answer: " + app.status());
  t::Screen again(160, 48);
  app.draw(again, 1761700003);
  expect(all_text(again).find("21.4 tok/s") != std::string::npos, "a failed poll keeps the last readings");
  app.answer({"models", "GET", "/v1/strata/models", {}}, 401,
             R"({"error":{"message":"invalid API key","type":"authentication_error","param":null,"code":null}})");
  expect(app.status() == "The server refused the API key (HTTP 401)", "the key refused: " + app.status());
  app.answer({"metrics", "GET", "/metrics", {}}, 200, kMetrics);
  expect(app.status() == "The server refused the API key (HTTP 401)", "said until the refused poll answers: " + app.status());
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  expect(app.status().empty(), "every poll answering clears it: " + app.status());
  // a server on an address that is not this PC's loopback cannot be reached from here: said plainly
  t::App away(t::Capabilities{}, std::nullopt);
  away.server_state("http://203.0.113.5:8080", true, false);
  away.answer({"metrics", "GET", "/metrics", {}}, 0, "");
  t::Screen away_screen(120, 30);
  away.draw(away_screen, 0);
  expect(away.status().find("cannot reach the server at http://203.0.113.5:8080") != std::string::npos &&
             all_text(away_screen).find("cannot reach") != std::string::npos,
         "an unreachable server: " + away.status());
  // a key that could not be read: the refusal says why
  t::App unread(t::Capabilities{}, std::nullopt);
  unread.server_state("http://127.0.0.1:8080", true);
  unread.set_key(std::nullopt, false);
  unread.answer({"metrics", "GET", "/metrics", {}}, 401, "");
  expect(unread.status() == "The server refused the API key (HTTP 401): strata-tui could not read the key it checks",
         "the key unread: " + unread.status());
  // a poll's failure stays until that poll answers; time alone does not clear it
  unread.tick(1000);
  unread.tick(1100);
  expect(!unread.status().empty(), "a poll's failure outlasts the tick");
  // two polls failing in their own words, round after round: the status line keeps one, it does not flicker
  t::App two(t::Capabilities{}, std::nullopt);
  two.server_state("http://127.0.0.1:8080", true);
  for (int round = 0; round < 3; ++round) {
    two.answer({"metrics", "GET", "/metrics", {}}, 200, "not json");
    two.answer({"models", "GET", "/v1/strata/models", {}}, 500, R"({"error":{"message":"broken"}})");
  }
  two.answer({"metrics", "GET", "/metrics", {}}, 200, "not json");
  expect(two.status() == "models: broken (HTTP 500)", "the same failures again change nothing: " + two.status());
  // the charts keep their own history (about five minutes; the server keeps a minute): the server's history seeds it,
  // and each answer adds the seconds since the one before, the newest decode reading the live one
  const t::Request metrics_poll{"metrics", "GET", "/metrics", {}};
  const auto metrics_at = [](const long long time, const std::string& tok, const std::string& live) {
    return R"({"live":{"state":"generating","tok_s":)" + live + R"(},"hardware":{"cpu":1.0,"ram_used":)" + std::to_string(time) +
           R"(},"history":{"tok_s":[)" + tok + R"(],"cpu":[1,1,1]},"time":)" + std::to_string(time) + "}";
  };
  t::App charts(t::Capabilities{}, std::nullopt);
  charts.server_state("http://127.0.0.1:8080", true);
  charts.answer(metrics_poll, 200, metrics_at(100, "0,5,12", "12"));
  charts.answer(metrics_poll, 200, metrics_at(101, "5,12,20", "21"));
  charts.answer(metrics_poll, 200, metrics_at(103, "20,30,31", "32"));
  charts.answer(metrics_poll, 200, metrics_at(103, "20,30,31", "33"));
  const auto seen = charts.history("tok_s");
  std::string seen_text;
  for (const double v : seen) seen_text += std::to_string(v) + " ";
  expect(seen == std::vector<double>{0.0, 5.0, 12.0, 21.0, 30.0, 32.0}, "seeded, then a second, then two, then none: " + seen_text);
  expect(charts.history("cpu") == std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0, 1.0}, "every series alike");
  // RAM: the server keeps no series of it, so the reading itself, once a second gone by
  expect(charts.history("ram_used") == std::vector<double>{100.0, 101.0, 103.0, 103.0}, "RAM's own history");
  // a long gap (polls failing for minutes): RAM takes no more of it than the server's minute, as the other series
  t::App gap(t::Capabilities{}, std::nullopt);
  gap.answer(metrics_poll, 200, metrics_at(0, "1,2,3", "3"));
  gap.answer(metrics_poll, 200, metrics_at(200, "1,2,3", "3"));
  expect(gap.history("ram_used").size() == 61U, "RAM after a gap: " + std::to_string(gap.history("ram_used").size()));
  for (long long i = 0; i < 400; ++i) charts.answer(metrics_poll, 200, metrics_at(104 + i, "1,2,3", "7"));
  expect(charts.history("tok_s").size() == 300U && charts.history("tok_s").back() == 7.0,
         "five minutes of samples kept: " + std::to_string(charts.history("tok_s").size()));
  // no GPU: its charts say so instead of a scale over nothing
  t::App cpu_only(t::Capabilities{}, std::nullopt);
  cpu_only.server_state("http://127.0.0.1:8080", true);
  cpu_only.answer(metrics_poll, 200,
                  R"({"live":{"state":"idle"},"hardware":{"cpu":12.0,"ram_used":1000000000,"ram_total":8000000000},)"
                  R"("hardware_static":{"gpu_name":null,"gpu_note":"no NVIDIA GPU","threads":8},)"
                  R"("history":{"tok_s":[0,0],"cpu":[10,12],"gpu_util":[null,null],"gpu_mem_used":[null,null],"gpu_temp":[null,null],)"
                  R"("gpu_power":[null,null]},"time":5})");
  t::Screen cpu_screen(160, 48);
  cpu_only.draw(cpu_screen, 0);
  const std::string cpu_text = all_text(cpu_screen);
  expect(cpu_text.find("no GPU") != std::string::npos && cpu_text.find("100 C") == std::string::npos &&
             cpu_text.find("100 W") == std::string::npos && cpu_text.find("12%") != std::string::npos,
         "no GPU:\n" + cpu_text);
  // the readings added since (the disks, the engine's memory, the conversation cache): eight charts, three even panels,
  // and the last requests across the whole width
  t::App full(t::Capabilities{}, std::nullopt);
  full.server_state("http://127.0.0.1:8080", true);
  full.answer(metrics_poll, 200, kMetricsFull);
  t::Screen wide(160, 48);
  full.draw(wide, 1761700003);
  const std::string wide_text = all_text(wide);
  for (const char* title : {"Decode", "GPU use", "CPU", "RAM", "GPU memory", "GPU temperature", "GPU power", "Disk read"}) {
    expect(wide_text.find(std::string{" "} + title + " ") != std::string::npos, std::string{"the chart "} + title);
  }
  const auto chart_row = boxes_on(wide, row_with(wide, "Decode"));
  expect(chart_row.size() == 4U && chart_row[0].second - chart_row[0].first == chart_row[3].second - chart_row[3].first &&
             row_with(wide, "Disk read") == row_with(wide, "GPU memory") && row_with(wide, "Disk read") > row_with(wide, "Decode"),
         "two rows of four charts from 120 columns");
  expect(wide_text.find("37.3 / 89.4 GB") != std::string::npos && wide_text.find("120 MB/s") != std::string::npos &&
             wide_text.find("peak 1.95 GB/s") != std::string::npos,
         "RAM against its total, the disks' read rate and its peak (GB/s past 1000 MB/s):\n" + wide_text);
  const auto panels = boxes_on(wide, row_with(wide, "Conversation cache"));
  expect(panels.size() == 3U && panels[0].second - panels[0].first == panels[1].second - panels[1].first &&
             panels[1].second - panels[1].first == panels[2].second - panels[2].first && row_with(wide, "Engine") == row_with(wide, "Machine"),
         "Engine, Machine and Conversation cache side by side, even: " + std::to_string(panels.size()));
  const auto requests_box = boxes_on(wide, row_with(wide, "Last requests"));
  expect(requests_box.size() == 1U && requests_box[0].first == 0 && requests_box[0].second == 159 &&
             row_with(wide, "Last requests") > row_with(wide, "Conversation cache"),
         "the last requests across the whole width, below the panels");
  expect(value_of(wide, "Experts (RAM)") == "120, 35.2 GB" && value_of(wide, "Experts (VRAM)") == "40" &&
             value_of(wide, "Process") == "12.3 GB",
         "the engine's memory: " + value_of(wide, "Experts (RAM)") + " | " + value_of(wide, "Experts (VRAM)") + " | " + value_of(wide, "Process"));
  expect(value_of(wide, "Disk reads") == "120 MB/s" && value_of(wide, "Disk writes") == "3.2 MB/s",
         "the disks: " + value_of(wide, "Disk reads") + " | " + value_of(wide, "Disk writes"));
  expect(value_of(wide, "Now") == "generating, 1 queued" && value_of(wide, "Served") == "1 request" &&
             value_of(wide, "Tokens out") == "64",
         "the engine's work: " + value_of(wide, "Now") + " | " + value_of(wide, "Served") + " | " + value_of(wide, "Tokens out"));
  // a request's prefill speed counts the tokens it read, not those the cache gave back (as the page): (100 - 64) / 0.2 s
  const int requests_header = row_with(wide, "prefill  finish");
  expect(requests_header >= 0 && row_text(wide, requests_header + 1).find(" 180  stop") != std::string::npos,
         "the prefill of the tokens read: " + (requests_header >= 0 ? row_text(wide, requests_header + 1) : std::string{}));
  expect(value_of(wide, "Cache") == "on (4 slots, 4.0 GB)" && value_of(wide, "Parked").find("2 of 4") != std::string::npos &&
             value_of(wide, "Held").find("1.2 / 4.0 GB") != std::string::npos &&
             value_of(wide, "Parks") == "5" && value_of(wide, "Restores") == "3" && value_of(wide, "Evictions") == "1" &&
             value_of(wide, "Last event") == "restored 512 tokens" && value_of(wide, "Last event at") == "12 s ago" &&
             value_of(wide, "Reused tokens") == "1234 of 3000 (41%)" && value_of(wide, "Requests") == "3 of 10 reused",
         "the conversation cache: " + value_of(wide, "Cache") + " | " + value_of(wide, "Parked") + " | " + value_of(wide, "Held") + " | " +
             value_of(wide, "Parks") + " | " + value_of(wide, "Last event") + " | " + value_of(wide, "Last event at") + " | " +
             value_of(wide, "Reused tokens") + " | " + value_of(wide, "Requests"));
  expect(value_of(wide, "Answer") == "40 of 512 tokens" && value_of(wide, "Prompt") == "812 at 640.0 tok/s" &&
             value_of(wide, "Elapsed") == "3.5 s",
         "the request under way: " + value_of(wide, "Answer") + " | " + value_of(wide, "Prompt") + " | " + value_of(wide, "Elapsed"));
  // a common 120 x 30 console: the eight charts and the even panels; no room left for the last requests
  t::Screen common(120, 30);
  full.draw(common, 1761700003);
  const auto common_panels = boxes_on(common, row_with(common, "Conversation cache"));
  expect(boxes_on(common, row_with(common, "Decode")).size() == 4U && all_text(common).find("Disk read") != std::string::npos &&
             common_panels.size() == 3U && common_panels[0].second - common_panels[0].first == common_panels[2].second - common_panels[2].first &&
             all_text(common).find("Last requests") == std::string::npos,
         "120 x 30:\n" + all_text(common));
  // 80 x 24: four rows of two charts, no panels
  t::Screen console(80, 24);
  full.draw(console, 1761700003);
  expect(boxes_on(console, row_with(console, "Decode")).size() == 2U && all_text(console).find("Disk read") != std::string::npos &&
             all_text(console).find("Machine") == std::string::npos,
         "80 x 24:\n" + all_text(console));
  // unloaded after idling: said plainly, and none of the loaded engine's figures
  t::App idle(t::Capabilities{}, std::nullopt);
  idle.server_state("http://127.0.0.1:8080", true);
  idle.answer(metrics_poll, 200, kMetricsUnloaded);
  t::Screen idle_screen(160, 48);
  idle.draw(idle_screen, 0);
  expect(value_of(idle_screen, "Status").find("unloaded") != std::string::npos && value_of(idle_screen, "Context") == "-" &&
             value_of(idle_screen, "Experts (RAM)") == "-" && value_of(idle_screen, "Process") == "-" &&
             value_of(idle_screen, "Disk reads") == "-" && value_of(idle_screen, "Cache") == "no model running",
         "unloaded: " + value_of(idle_screen, "Status") + " | " + value_of(idle_screen, "Cache"));
  // an engine without the cache
  std::string unsupported = kMetricsFull;
  unsupported.replace(unsupported.find(R"("supported":true)"), 16, R"("supported":false)");
  t::App plain_engine(t::Capabilities{}, std::nullopt);
  plain_engine.server_state("http://127.0.0.1:8080", true);
  plain_engine.answer(metrics_poll, 200, unsupported);
  t::Screen plain_screen(160, 48);
  plain_engine.draw(plain_screen, 0);
  expect(value_of(plain_screen, "Cache") == "Not used by this engine", "unsupported: " + value_of(plain_screen, "Cache"));
  // no model running: the engine said stopped, as the server's live state says it (its health is "none")
  std::string no_model = kMetricsUnloaded;
  no_model.replace(no_model.find(R"("health":"unloaded","loaded":false,"unloaded":true)"), 50,
                   R"("health":"none","loaded":false,"unloaded":false)");
  no_model.replace(no_model.find(R"("state":"unloaded")"), 18, R"("state":"stopped")");
  t::App no_engine(t::Capabilities{}, std::nullopt);
  no_engine.server_state("http://127.0.0.1:8080", true);
  no_engine.answer(metrics_poll, 200, no_model);
  t::Screen no_engine_screen(160, 48);
  no_engine.draw(no_engine_screen, 0);
  expect(value_of(no_engine_screen, "Status") == "stopped", "no model running: " + value_of(no_engine_screen, "Status"));
  // the server stopped: its last readings are not shown as if live (the header's model neither)
  full.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  full.server_state("stopped (exit 1)", false);
  t::Screen stopped_screen(160, 48);
  full.draw(stopped_screen, 0);
  expect(value_of(stopped_screen, "Status") == "stopped" && value_of(stopped_screen, "Experts (RAM)") == "-" &&
             value_of(stopped_screen, "Now") == "-" && row_text(stopped_screen, 0).find("(running)") == std::string::npos,
         "stopped: " + value_of(stopped_screen, "Status") + " | " + value_of(stopped_screen, "Now") + " | " + row_text(stopped_screen, 0));
  // big figures still fit a 120-column console's panels
  std::string big = kMetricsFull;
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {R"("generated":40,"max_tokens":512)", R"("generated":1500,"max_tokens":32768)"},
           {R"("prompt_tokens":812,)", R"("prompt_tokens":120000,)"},
           {R"("requests":1,"prompt_tokens":100,"output_tokens":64)", R"("requests":1234,"prompt_tokens":100,"output_tokens":5678901)"},
           {R"("reused_tokens":1234,"prompt_tokens":3000)", R"("reused_tokens":98765,"prompt_tokens":123456)"},
           {R"("last_tokens":512)", R"("last_tokens":65536)"}}) {
    big.replace(big.find(from), from.size(), to);
  }
  t::App busy(t::Capabilities{}, std::nullopt);
  busy.server_state("http://127.0.0.1:8080", true);
  busy.answer(metrics_poll, 200, big);
  t::Screen busy_screen(120, 30);
  busy.draw(busy_screen, 1761700003);
  const int busy_top = row_with(busy_screen, "Conversation cache");
  for (int y = busy_top + 1; busy_top >= 0 && y < busy_screen.height() - 2; ++y) {
    expect(row_text(busy_screen, y).find("\xE2\x80\xA6") == std::string::npos, "a big value cut at 120 columns: " + row_text(busy_screen, y));
  }
  expect(value_of(busy_screen, "Served") == "1234 requests" && value_of(busy_screen, "Tokens out") == "5.7M" &&
             value_of(busy_screen, "Answer") == "1500 of 32.8k tokens",
         "big figures: " + value_of(busy_screen, "Served") + " | " + value_of(busy_screen, "Tokens out") + " | " + value_of(busy_screen, "Answer"));
  // narrower than 120 columns, no panels (their values would not fit); the last requests still, when the height allows,
  // and never past their box
  full.server_state("http://127.0.0.1:8080", true);
  t::Screen mid(100, 40);
  full.draw(mid, 0);
  expect(all_text(mid).find("Machine") == std::string::npos && all_text(mid).find("Last requests") != std::string::npos,
         "100 x 40:\n" + all_text(mid));
  t::Screen tall(60, 70);
  full.draw(tall, 0);
  const int tall_top = row_with(tall, "Last requests");
  bool inside = tall_top >= 0;
  for (int y = tall_top + 1; tall_top >= 0 && y < tall.height() - 2; ++y) inside = inside && tall.at(59, y).glyph == char32_t{0x2502};
  expect(inside, "the last requests keep inside their box on a narrow console:\n" + all_text(tall));
  // an older server, without these fields: every one of their figures "-", never 0
  t::Screen older(160, 48);
  app.draw(older, 1761700003);
  expect(value_of(older, "Disk reads") == "-" && value_of(older, "Disk writes") == "-" && value_of(older, "Experts (RAM)") == "-" &&
             value_of(older, "Process") == "-" &&
             value_of(older, "Cache") == "-" && value_of(older, "Parked") == "-" && value_of(older, "Reused tokens") == "-" &&
             value_of(older, "Last event at") == "-" &&
             all_text(older).find("0 MB/s") == std::string::npos && all_text(older).find("Machine") != std::string::npos,
         "an older server: " + value_of(older, "Disk reads") + " | " + value_of(older, "Cache") + " | " + value_of(older, "Parked"));
  // the values fit the panels' value column at 120 columns (23 cells): none is cut
  const int panel_top = row_with(common, "Conversation cache");
  for (int y = panel_top + 1; panel_top >= 0 && y < common.height() - 2; ++y) {
    expect(row_text(common, y).find("\xE2\x80\xA6") == std::string::npos, "a value cut at 120 columns: " + row_text(common, y));
  }  // a narrow terminal still draws, one chart a row, without the panels
  t::Screen narrow(70, 20);
  app.draw(narrow, 1761700003);
  expect(all_text(narrow).find("Decode") != std::string::npos, "a narrow terminal still shows the charts");
  // before the server answers
  t::App waiting(t::Capabilities{}, std::nullopt);
  t::Screen blank(80, 24);
  waiting.draw(blank, 0);
  expect(all_text(blank).find("starting") != std::string::npos, "the server's state while it starts");
}

void the_views_switch_and_keys_become_requests() {
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  // the polls: the dashboard's, then the view's
  expect(app.polls() == std::vector<t::Request>{{"metrics", "GET", "/metrics", {}}, {"models", "GET", "/v1/strata/models", {}}},
         "the Monitor polls /metrics and the models");
  (void)app.key(character("3"));
  expect(app.view() == t::View::downloads && app.polls().size() == 3U && app.polls().back().path == "/v1/strata/downloads",
         "3 is Downloads, which polls the downloads");
  // a download not started has no progress bar; one under way has its bar
  app.answer({"downloads", "GET", "/v1/strata/downloads", {}}, 200,
             R"({"disk":{"root":"D:","total_bytes":1000,"free_bytes":500},"active":null,"data":[)"
             R"({"id":"a","name":"Waiting","state":"available","size_bytes":100,"bytes":0,"total":100,"error":null,"note":""},)"
             R"({"id":"b","name":"Halfway","state":"paused","size_bytes":100,"bytes":50,"total":100,"error":null,"note":""}]})");
  t::Screen list(120, 30);
  app.draw(list, 0);
  const auto row_of = [&list](const std::string& name) {
    for (int y = 0; y < list.height(); ++y) {
      if (row_text(list, y).find(name) != std::string::npos) return row_text(list, y);
    }
    return std::string{};
  };
  const std::string track = t::utf8(char32_t{0x00B7});
  const std::string block = t::utf8(char32_t{0x2588});
  expect(row_of("Waiting").find(track) == std::string::npos && row_of("Waiting").find(block) == std::string::npos,
         "no bar before a download starts: " + row_of("Waiting"));
  expect(row_of("Halfway").find(block) != std::string::npos && row_of("Halfway").find(track) != std::string::npos,
         "a bar once it has bytes: " + row_of("Halfway"));
  // the mouse wheel moves the selection
  (void)app.key(plain(t::Key::Kind::wheel_down));
  const auto picked = app.key(plain(t::Key::Kind::enter));
  expect(picked.requests.size() == 1U && picked.requests[0].body == R"({"id":"b"})",
         "the wheel moves the selection: " + (picked.requests.empty() ? std::string{} : picked.requests[0].body));
  // a view's own poll failing is said while that view polls; leaving the view takes it off the status line
  app.answer({"downloads", "GET", "/v1/strata/downloads", {}}, 500, "");
  expect(app.status() == "downloads: HTTP 500", "the downloads poll failed: " + app.status());
  (void)app.key(plain(t::Key::Kind::back_tab));
  expect(app.view() == t::View::models, "Shift-Tab goes back to Models");
  expect(app.status().empty(), "and the downloads poll's failure goes with its view: " + app.status());
  app.answer({"downloads", "GET", "/v1/strata/downloads", {}}, 500, "");
  expect(app.status().empty(), "an answer still in flight from that view says nothing after it: " + app.status());
  // Enter asks first while a card runs, as the page does: restart the running one, or stop it and start another
  const auto asked = [&app](const std::string& question) {
    t::Screen screen(120, 30);
    app.draw(screen, 0);
    return all_text(screen).find(question) != std::string::npos;
  };
  auto out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.empty() && asked("Restart Qwen3.8 with these options?"), "Enter on the running card asks first");
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/start" &&
             out.requests[0].body == R"({"name":"Qwen3.8","options":{"thinking":"high","web_search":true,"max_context":131072}})",
         "y starts the card with its options: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  // the options: o focuses them, Right changes the focused one (a choice cycles), Down the next (a switch flips)
  (void)app.key(character("o"));
  (void)app.key(plain(t::Key::Kind::right));
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::right));
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::left));
  (void)app.key(plain(t::Key::Kind::enter));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U &&
             out.requests[0].body == R"({"name":"Qwen3.8","options":{"thinking":"none","web_search":false,"max_context":126976}})",
         "the changed options go with the start: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  out = app.key(character("s"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/stop" && out.requests[0].body == "{}", "s stops");
  (void)app.key(plain(t::Key::Kind::down));
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.empty() && asked("Stop Qwen3.8 and start MiniMax?"), "Enter on another card asks to stop the running one");
  expect(app.key(character("n")).requests.empty() && !asked("Stop Qwen3.8"), "n keeps the running one");
  (void)app.key(plain(t::Key::Kind::wheel_up));
  out = app.key(character("c"));
  expect(out.requests.size() == 1U && out.requests[0].body.find("\"name\":\"Qwen3.8\"") != std::string::npos, "the wheel moves the card too");
  (void)app.key(plain(t::Key::Kind::wheel_down));
  out = app.key(character("c"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/calibrate" &&
             out.requests[0].body.find("\"name\":\"MiniMax\"") != std::string::npos,
         "c calibrates the selected card");
  t::Screen screen(140, 40);
  app.draw(screen, 0);
  const std::string text = all_text(screen);
  expect(text.find("Prompt reading: chunks of 1024 tokens") != std::string::npos && text.find("Not calibrated") != std::string::npos,
         "the card's calibration and MiniMax's prompt reading are said");
  // the owner (2026-10-30): a calibration with an error made a report for a bug report - where it is
  expect(text.find("Report for a bug report: logs/calibration-reports/calibration-MiniMax-20261030-041646.md") != std::string::npos,
         "where the calibration's report is: " + text);
  // an action's refusal is said in the server's words
  app.answer({"calibrate MiniMax", "POST", "/v1/strata/models/calibrate", {}}, 409,
             R"({"error":{"message":"another start or stop is in progress","type":"invalid_request_error","param":null,"code":"switch_in_progress"}})");
  expect(app.status() == "calibrate MiniMax: another start or stop is in progress", "the refusal's words: " + app.status());
  // Del takes the selected card off the list, its model files kept (owner 2026-10-28, "remove an active model card but
  // not delete files"), asked first; the running card's model is stopped first; a card from the server's config stays
  out = app.key(plain(t::Key::Kind::del));
  expect(out.requests.empty() && app.status() == "MiniMax's card is in the server's config: remove it there",
         "the operator's card is not removed here: " + app.status());
  (void)app.key(plain(t::Key::Kind::up));
  out = app.key(plain(t::Key::Kind::del));
  expect(out.requests.empty() && asked("Stop Qwen3.8 and remove its card? Its model files stay."), "Del asks first");
  expect(app.key(character("n")).requests.empty() && !asked("remove its card"), "n keeps the card");
  (void)app.key(plain(t::Key::Kind::del));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/cards/remove" && out.requests[0].body == R"({"name":"Qwen3.8"})",
         "y removes the card: " + (out.requests.empty() ? std::string{} : out.requests[0].path + " " + out.requests[0].body));
  // the Log scrolls up to its first page and no further
  for (int i = 0; i < 100; ++i) app.log_line("line " + std::to_string(i));
  (void)app.key(character("5"));
  // an action's words go with a view switch, or after ten seconds
  expect(app.status().empty(), "a view switch clears an action's words: " + app.status());
  app.answer({"stop", "POST", "/v1/strata/models/stop", "{}"}, 200, "{}");
  app.tick(2000);
  app.tick(2009);
  expect(app.status() == "stop: done", "said for ten seconds: " + app.status());
  app.tick(2010);
  expect(app.status().empty(), "then cleared: " + app.status());
  t::Screen logged(80, 24);  // 19 lines in its box
  app.draw(logged, 0);
  expect(all_text(logged).find("line 99 ") != std::string::npos && all_text(logged).find("line 80 ") == std::string::npos, "the newest page");
  (void)app.key(plain(t::Key::Kind::home));
  app.draw(logged, 0);
  expect(all_text(logged).find("line 0 ") != std::string::npos && all_text(logged).find("line 18 ") != std::string::npos &&
             all_text(logged).find("line 19 ") == std::string::npos,
         "Home: the oldest page, full:\n" + all_text(logged));
  for (int i = 0; i < 30; ++i) (void)app.key(plain(t::Key::Kind::up));
  (void)app.key(plain(t::Key::Kind::down));
  app.draw(logged, 0);
  expect(all_text(logged).find("line 19 ") != std::string::npos && all_text(logged).find("line 0 ") == std::string::npos,
         "Up stops at the oldest page, one Down moves one line:\n" + all_text(logged));
  // About: each reading by its path, a size in bytes as GB
  (void)app.key(character("6"));
  app.answer({"about", "GET", "/v1/strata/about", {}}, 200,
             R"({"version":"1.2.3","gpus":[{"name":"RTX","memory_bytes":17179869184}],"disk_free_bytes":1073741824})");
  t::Screen about(120, 30);
  app.draw(about, 0);
  const std::string about_text = all_text(about);
  expect(about_text.find("gpus[0].memory_bytes") != std::string::npos && about_text.find("16.0 GB") != std::string::npos &&
             about_text.find("1.0 GB") != std::string::npos && about_text.find("17179869184") == std::string::npos &&
             about_text.find("1.2.3") != std::string::npos,
         "About:\n" + about_text);
  // the options as the page sends them: a value or else the default, the kinds it knows (one it does not is shown,
  // never sent), the GPUs choice only once changed; a number moves within the bounds it has (none: free), and an
  // "or auto" one goes below its minimum to auto and back
  t::App opts(t::Capabilities{}, std::nullopt);
  opts.answer({"models", "GET", "/v1/strata/models", {}}, 200,
              R"({"object":"list","running":null,"state":"idle","error":null,"data":[{"name":"Gemma","profile":"gemma","state":"stopped",)"
              R"("error":null,"options":[{"name":"n_gpu","kind":"integer_or_auto","label":"GPU layers","default":"auto","value":"auto",)"
              R"("min":0,"max":99},{"name":"temp","kind":"decimal","label":"Temperature","default":0.5,"value":null},)"
              R"({"name":"mystery","kind":"future_kind","label":"Mystery","value":1},)"
              R"({"name":"gpus","kind":"devices","label":"GPUs","value":"auto","max":2},)"
              R"({"name":"flash","kind":"boolean","label":"Flash attention","default":true,"value":null}],"calibration":null}]})");
  (void)opts.key(character("2"));
  const auto start_body = [&opts] {
    const auto started = opts.key(plain(t::Key::Kind::enter));
    return started.requests.empty() ? std::string{"(no request)"} : started.requests[0].body;
  };
  t::Screen options_screen(120, 30);
  opts.draw(options_screen, 0);
  const auto option_row = [&options_screen](const std::string& label) {
    for (int y = 0; y < options_screen.height(); ++y) {
      if (row_text(options_screen, y).find(label) != std::string::npos) return row_text(options_screen, y);
    }
    return std::string{};
  };
  expect(option_row("Temperature").find("0.5") != std::string::npos && option_row("Flash attention").find(" on ") != std::string::npos,
         "an option with no value shows the default it starts with: " + option_row("Temperature"));
  std::string body = start_body();
  expect(body == R"({"name":"Gemma","options":{"n_gpu":"auto","temp":0.5,"flash":true}})", "the page's options: " + body);
  (void)opts.key(character("o"));
  (void)opts.key(plain(t::Key::Kind::right));
  (void)opts.key(plain(t::Key::Kind::right));
  body = start_body();
  expect(body == R"({"name":"Gemma","options":{"n_gpu":2,"temp":0.5,"flash":true}})", "auto, then numbers by the range's step: " + body);
  (void)opts.key(character("o"));
  (void)opts.key(plain(t::Key::Kind::left));
  (void)opts.key(plain(t::Key::Kind::left));
  (void)opts.key(plain(t::Key::Kind::down));
  (void)opts.key(plain(t::Key::Kind::right));
  for (int i = 0; i < 3; ++i) (void)opts.key(plain(t::Key::Kind::down));
  (void)opts.key(plain(t::Key::Kind::right));
  body = start_body();
  expect(body == R"({"name":"Gemma","options":{"n_gpu":"auto","temp":0.55,"flash":false}})",
         "below the minimum is auto; no bounds is free; a switch flips from its default: " + body);
  // many options: the one in focus is drawn
  std::string many = R"({"object":"list","running":null,"state":"idle","error":null,"data":[{"name":"Big","profile":"x","state":"stopped","options":[)";
  for (int i = 0; i < 16; ++i) {
    many += (i == 0 ? "" : ",") + std::string{R"({"name":"o)"} + std::to_string(i) + R"(","kind":"boolean","label":"Option )" +
            std::to_string(i) + R"( here","value":false})";
  }
  many += "]}]}";
  t::App big(t::Capabilities{}, std::nullopt);
  big.answer({"models", "GET", "/v1/strata/models", {}}, 200, many);
  (void)big.key(character("2"));
  (void)big.key(character("o"));
  for (int i = 0; i < 15; ++i) (void)big.key(plain(t::Key::Kind::down));
  t::Screen big_screen(120, 24);
  big.draw(big_screen, 0);
  expect(all_text(big_screen).find("Option 15 here") != std::string::npos, "the focused option is in view:\n" + all_text(big_screen));
}

const std::string kSettings =
    R"({"editable":true,"settings":[{"key":"listener.port","type":"integer","value":8080,"default":8080,"min":1,"max":65535,"help":"The port"},)"
    R"({"key":"listener.allow_remote","type":"boolean","value":null,"default":false,"help":"Remote clients"},)"
    R"({"key":"listener.host_aliases","type":"string array","value":["box"],"default":[],"help":"Names"},)"
    R"({"key":"models.control_remote_networks","type":"string array","value":null,"default":[],"help":"Networks"}],"key":{"rotatable":true}})";

// Owner 2026-10-28 ("Locally Provided vs Downloaded ... so the user knows what they already have in the system"): the
// Downloads view says what this PC already has of each entry, from the list's "have" (the server's one rule, as the page
// shows it), names the selected one's card, and Enter downloads none of those.
void downloads_say_what_is_on_this_pc() {
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  (void)app.key(character("3"));
  app.answer({"downloads", "GET", "/v1/strata/downloads", {}}, 200,
             R"({"disk":{"root":"D:","total_bytes":1000,"free_bytes":500},"active":null,"data":[)"
             R"({"id":"a","name":"Mine","state":"failed","size_bytes":100,"bytes":0,"total":100,"error":"an earlier try failed","note":"after the download","have":"local","have_card":"my-card"},)"
             R"({"id":"b","name":"Fetched","state":"ready","size_bytes":100,"bytes":100,"total":100,"error":null,"note":"after the download","have":"downloaded","have_card":"b-card"},)"
             R"({"id":"c","name":"Fresh","state":"available","size_bytes":100,"bytes":0,"total":100,"error":null,"note":"after the download","have":null,"have_card":null},)"
             R"({"id":"d","name":"Older","state":"available","size_bytes":100,"bytes":0,"total":100,"error":null,"note":"after the download","have":"other","have_card":"d-card"},)"
             R"({"id":"e","name":"Unset","state":"installed","size_bytes":100,"bytes":100,"total":100,"error":null,"note":"after the download","have":null,"have_card":null}]})");
  t::Screen list(120, 30);
  app.draw(list, 0);
  const auto row_of = [&list](const std::string& name) {
    for (int y = 0; y < list.height(); ++y) {
      if (row_text(list, y).find(name) != std::string::npos) return row_text(list, y);
    }
    return std::string{};
  };
  expect(row_of("Mine").find("Locally provided") != std::string::npos && row_of("Fetched").find("Downloaded") != std::string::npos &&
             row_of("Fresh").find("available") != std::string::npos,
         "each row says what this PC has of it: " + row_of("Mine") + " | " + row_of("Fetched") + " | " + row_of("Fresh"));
  expect(all_text(list).find("Already on this PC (its card: my-card)") != std::string::npos &&
             all_text(list).find("an earlier try failed") == std::string::npos,
         "the selected one names its card (an earlier try's failure no longer matters): " + all_text(list));
  auto out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.empty() && app.status().find("Mine is already on this PC") != std::string::npos,
         "Enter downloads nothing that is already here: " + app.status());
  (void)app.key(plain(t::Key::Kind::down));
  out = app.key(character("d"));
  expect(out.requests.empty(), "nor what was downloaded");
  (void)app.key(plain(t::Key::Kind::down));
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/downloads/start" && out.requests[0].body == R"({"id":"c"})",
         "the others download as before");
  // another file of that name on this PC (another revision): said, with its card, and still downloadable
  (void)app.key(plain(t::Key::Kind::down));
  app.draw(list, 0);
  expect(row_of("Older").find("Other version") != std::string::npos &&
             all_text(list).find("Another version of this file is on this PC (its card: d-card)") != std::string::npos,
         "an other version is said, with its card: " + row_of("Older"));
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.size() == 1U && out.requests[0].body == R"({"id":"d"})", "and Enter downloads the catalog's version");
  // installed, but its card was never made (owner 2026-10-28): said, and Enter runs the step (nothing is downloaded again)
  (void)app.key(plain(t::Key::Kind::down));
  app.draw(list, 0);
  expect(row_of("Unset").find("installed") != std::string::npos &&
             all_text(list).find("Its files are on this PC, but its card was not made: Enter makes it") != std::string::npos,
         "an installed download without its card says so: " + row_of("Unset"));
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.size() == 1U && out.requests[0].body == R"({"id":"e"})", "and Enter makes its card");
}

// A message - a value refused as it is typed, an action's answer - is shown whole on the key line, before the keys, which
// take the room it leaves until it clears (the owner, 2026-10-31: "pressing w on the tui is not saving": the reason was
// the line's last few words, cut off).
void a_message_is_shown_whole_before_the_keys() {
  t::App app(t::Capabilities{}, std::optional<std::string>{"secret-key-for-test"});
  (void)app.key(character("4"));
  app.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200, kSettings);
  t::Screen screen(120, 30);
  const auto bottom = [&screen] { return row_text(screen, screen.height() - 1); };
  // a number out of range, refused as it is entered: nothing to save, and why, whole
  (void)app.key(plain(t::Key::Kind::enter));
  for (int i = 0; i < 8; ++i) (void)app.key(plain(t::Key::Kind::backspace));
  for (const char* c : {"7", "0", "0", "0", "0"}) (void)app.key(character(c));
  (void)app.key(plain(t::Key::Kind::enter));
  app.tick(100);
  app.draw(screen, 100);
  expect(bottom().find("listener.port takes a whole number from 1 to 65535") != std::string::npos, "the refusal, whole: " + bottom());
  expect(bottom().find("q  Quit") != std::string::npos, "the keys after it, in the room it leaves: " + bottom());
  expect(app.key(character("w")).requests.empty(), "nothing to save");
  // a save the server refuses: its reason whole
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  const auto out = app.key(character("w"));
  expect(out.requests.size() == 1U, "w saves the switch");
  const std::string reason = "listener.allow_remote needs a listener.host other machines can reach";
  app.answer(out.requests.at(0), 422,
             R"({"error":{"message":")" + reason + R"(","type":"invalid_request_error","param":null,"code":"settings_refused"}})");
  app.tick(200);
  app.draw(screen, 200);
  expect(bottom().find("save settings: " + reason) != std::string::npos, "the server's refusal, whole: " + bottom());
  // the keys come back once it clears
  app.tick(200 + t::App::kStatusSeconds);
  app.draw(screen, 200 + t::App::kStatusSeconds);
  expect(bottom().rfind("  q  Quit   Tab  View", 0) == 0U && bottom().find(reason) == std::string::npos, "then the keys alone: " + bottom());
}

void settings_are_edited_then_saved() {
  t::App app(t::Capabilities{}, std::optional<std::string>{"secret-key-for-test"});
  (void)app.key(character("4"));
  expect(app.view() == t::View::settings && app.polls().back().path == "/v1/strata/config", "4 is Settings, which reads them");
  app.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200, kSettings);
  expect(app.polls().size() == 2U, "read once, not at every tick (an edit would be lost)");
  // F4 is a view switch like 4: Settings read again
  (void)app.key(t::Key{t::Key::Kind::function, {}, 1});
  (void)app.key(t::Key{t::Key::Kind::function, {}, 4});
  expect(app.view() == t::View::settings && app.polls().back().path == "/v1/strata/config", "F4 reads the settings again");
  app.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200, kSettings);
  t::Screen screen(120, 30);
  app.draw(screen, 0);
  expect(all_text(screen).find("secret-key-for-test") == std::string::npos && all_text(screen).find("hidden") != std::string::npos,
         "the key is hidden");
  expect(all_text(screen).find("none (default)") != std::string::npos && all_text(screen).find("box") != std::string::npos,
         "a list is shown by its items, an empty one as none:\n" + all_text(screen));
  (void)app.key(character("k"));
  app.draw(screen, 0);
  expect(all_text(screen).find("secret-key-for-test") != std::string::npos, "k shows it");
  // the port: Enter opens the field, a new number, Enter keeps it
  (void)app.key(plain(t::Key::Kind::enter));
  for (int i = 0; i < 4; ++i) (void)app.key(plain(t::Key::Kind::backspace));
  for (const char* c : {"9", "0", "9", "0"}) (void)app.key(character(c));
  (void)app.key(plain(t::Key::Kind::enter));
  // the switch: Enter flips it; the list: its items
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  (void)app.key(character(","));
  for (const char* c : {" ", "n", "a", "s"}) (void)app.key(character(c));
  (void)app.key(plain(t::Key::Kind::enter));
  // the list's change opened again: its items in the field, not its JSON; Enter keeps them
  (void)app.key(plain(t::Key::Kind::enter));
  app.draw(screen, 0);
  expect(all_text(screen).find("box, nas_") != std::string::npos, "the pending list's items in the field:\n" + all_text(screen));
  (void)app.key(plain(t::Key::Kind::enter));
  // a whole number in the setting's range, or the status line says which setting takes what
  (void)app.key(plain(t::Key::Kind::up));
  (void)app.key(plain(t::Key::Kind::up));
  for (const char* typed : {"-", "8-0", "70000", "9O"}) {
    (void)app.key(plain(t::Key::Kind::enter));
    for (int i = 0; i < 8; ++i) (void)app.key(plain(t::Key::Kind::backspace));
    for (const char c : std::string_view{typed}) (void)app.key(character(std::string(1U, c)));
    (void)app.key(plain(t::Key::Kind::enter));
    expect(app.status() == "listener.port takes a whole number from 1 to 65535", std::string{typed} + " is refused: " + app.status());
  }
  // the changes not yet saved, shown as saved values are: a switch on/off, a list by its items
  app.draw(screen, 0);
  expect(all_text(screen).find("9090  * unsaved") != std::string::npos && all_text(screen).find("on  * unsaved") != std::string::npos &&
             all_text(screen).find("box, nas  * unsaved") != std::string::npos,
         "the unsaved changes:\n" + all_text(screen));
  const auto out = app.key(character("w"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/config/settings" &&
             out.requests[0].body ==
                 R"({"values":{"listener.allow_remote":true,"listener.host_aliases":["box","nas"],"listener.port":9090}})",
         "w saves the changes: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  // a change made while the save is under way is kept for the next one
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  app.answer(out.requests[0], 200, R"({"saved":true,"restart_needed":true})");
  expect(app.status().find("apply when the server starts again") != std::string::npos && app.polls().back().path == "/v1/strata/config",
         "saved: said, and read again");
  const auto later = app.key(character("w"));
  expect(later.requests.size() == 1U && later.requests[0].body == R"({"values":{"listener.allow_remote":false}})",
         "the later change is still to save: " + (later.requests.empty() ? std::string{"(none)"} : later.requests[0].body));
  // the key's rotation asks for ROTATE
  (void)app.key(character("R"));
  for (const char* c : {"R", "O", "T", "A", "T", "E"}) (void)app.key(character(c));
  const auto rotated = app.key(plain(t::Key::Kind::enter));
  expect(rotated.requests.size() == 1U && rotated.requests[0].path == "/v1/strata/key/rotate" &&
             rotated.requests[0].body == R"({"confirm":"ROTATE"})",
         "ROTATE rotates the key");
  // the field belongs to the setting it was opened on, even when the settings are read again while it is open
  t::App reread(t::Capabilities{}, std::nullopt);
  (void)reread.key(character("4"));
  reread.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200, kSettings);
  (void)reread.key(plain(t::Key::Kind::down));
  (void)reread.key(plain(t::Key::Kind::down));
  (void)reread.key(plain(t::Key::Kind::enter));
  reread.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200,
                R"({"editable":true,"settings":[{"key":"models.control_remote_networks","type":"string array","value":null,"default":[],)"
                R"("help":"Networks"}],"key":{"rotatable":true}})");
  for (const char* c : {",", " ", "n", "a", "s"}) (void)reread.key(character(c));
  (void)reread.key(plain(t::Key::Kind::enter));
  const auto reread_save = reread.key(character("w"));
  expect(reread_save.requests.size() == 1U && reread_save.requests[0].body == R"({"values":{"listener.host_aliases":["box","nas"]}})",
         "the edit kept its setting: " + (reread_save.requests.empty() ? std::string{"(no request)"} : reread_save.requests[0].body));
  // the key row tells a keyless server from a key that could not be read
  const auto key_row = [](const bool keyless) {
    t::App other(t::Capabilities{}, std::nullopt);
    other.set_key(std::nullopt, keyless);
    (void)other.key(character("4"));
    other.answer({"settings", "POST", "/v1/strata/config", "{}"}, 200, kSettings);
    t::Screen drawn(120, 30);
    other.draw(drawn, 0);
    return row_text(drawn, 3);
  };
  expect(key_row(true).find("none (a keyless server on this PC)") != std::string::npos, "keyless: " + key_row(true));
  expect(key_row(false).find("could not be read") != std::string::npos, "unread: " + key_row(false));
}

void quitting_asks_first() {
  t::App app(t::Capabilities{}, std::nullopt);
  expect(!app.key(character("q")).quit, "q asks");
  expect(!app.key(character("n")).quit, "n keeps it running");
  // the mouse wheel is no answer: the question stays
  (void)app.key(character("q"));
  (void)app.key(plain(t::Key::Kind::wheel_down));
  expect(app.key(character("y")).quit, "a wheel turn left the question open, and y quits");
  (void)app.key(t::Key{t::Key::Kind::ctrl, "c", 0});
  expect(app.key(character("y")).quit, "Ctrl-C asks too, and y quits");
  // while an option has the focus, q is not Quit
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  (void)app.key(character("2"));
  (void)app.key(character("o"));
  (void)app.key(character("q"));
  expect(!app.key(character("y")).quit, "q among the options does not quit");
  // F1 is a view switch: the option focus goes, and q quits again
  (void)app.key(t::Key{t::Key::Kind::function, {}, 1});
  (void)app.key(character("q"));
  expect(app.view() == t::View::monitor && app.key(character("y")).quit, "after F1, q quits again");
}

const std::string kPlugins =
    R"({"folder":"/opt/m-strata/plugins","editable":true,"plugins":[)"
    R"({"id":"debugging","kind":"diagnostics","name":"Debugging","version":"1.0.0","publisher":"M-Strata","license":"AGPL-3.0-only",)"
    R"("about":"Verbose logs, to find out why a model does not start or calibrate.","first_party":true,"state":"off","reason":null,)"
    R"("running":false,"manifest_sha256":"aaaa1111","files":[],"switches":["logging.requests","logging.engine_output","troubleshooting"]},)"
    R"({"id":"acme","kind":"tool","name":"Acme search","version":"0.2.0","publisher":"Example Labs","license":"MIT","about":"A search tool.",)"
    R"("first_party":false,"state":"waiting","reason":"never approved","running":false,"manifest_sha256":"bbbb2222",)"
    R"("files":[{"path":"acme.exe","sha256":"cccc3333"}],"switches":[]},)"
    R"({"id":"old","kind":"engine","name":"Old family","version":"1","publisher":"x","license":"x","about":"","first_party":false,)"
    R"("state":"refused","reason":"this server does not run engine plugins yet","running":false,"manifest_sha256":"dddd4444",)"
    R"("files":[],"switches":[]}]})";

// The owner (2026-10-30): "we also need to have a plugin management tab to manage these"; the TUI has the page's
// Plugins tab as its view 7 (F7, Tab): each plugin with its state, the selected one's facts, and Enter for its action -
// Approve (asked first, as the page asks: a code plugin's code runs with the server's rights), Turn on (a diagnostics
// plugin says what it logs first) or Turn off. A change applies when the server starts again; the list is read again.
void plugins_are_approved_and_switched() {
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  const t::Request list{"plugins", "POST", "/v1/strata/plugins", "{}"};
  const auto polls_list = [&app, &list] {
    const auto polls = app.polls();
    return std::find(polls.begin(), polls.end(), list) != polls.end();
  };
  (void)app.key(character("6"));
  (void)app.key(plain(t::Key::Kind::tab));
  expect(app.view() == t::View::plugins && polls_list(), "Tab after About is Plugins, which reads the list");
  (void)app.key(character("1"));
  (void)app.key(character("7"));
  expect(app.view() == t::View::plugins, "7 is Plugins");
  (void)app.key(t::Key{t::Key::Kind::function, {}, 1});
  (void)app.key(t::Key{t::Key::Kind::function, {}, 7});
  expect(app.view() == t::View::plugins, "and F7");
  // a refusal of the list is said in the view as on the status line
  app.answer(list, 403, R"({"error":{"message":"model control is not allowed from this client","type":"invalid_request_error","param":null,"code":"control_not_allowed"}})");
  const auto shown = [&app] {
    t::Screen screen(120, 32);
    app.draw(screen, 0);
    return all_text(screen);
  };
  {
    t::Screen screen(120, 32);
    app.draw(screen, 0);
    expect(row_with(screen, "plugins: model control is not allowed from this client (HTTP 403)") == 3,
           "the list's refusal, in the view (not only on the status line):\n" + all_text(screen));
  }
  expect(!polls_list(), "a refusal (403, 404) is not asked again every second");
  (void)app.key(character("r"));
  expect(polls_list(), "until r");
  app.answer(list, 200, kPlugins);
  expect(!polls_list(), "read once, not polled again");
  std::string text = shown();
  for (const char* words : {"Folder: /opt/m-strata/plugins", "Debugging", "Acme search", "Waiting for approval", "Refused",
                            "debugging \xC2\xB7 diagnostics \xC2\xB7 version 1.0.0 \xC2\xB7 from M-Strata \xC2\xB7 AGPL-3.0-only \xC2\xB7 built with this install",
                            "Verbose logs, to find out why", "Turns on: logging.requests, logging.engine_output, troubleshooting",
                            "Not running.", "plugin.json  aaaa1111", "A change applies when the server starts again."}) {
    expect(text.find(words) != std::string::npos, std::string{"the view shows: "} + words + "\n" + text);
  }
  const auto drawn = [&app] {
    auto screen = std::make_unique<t::Screen>(120, 32);
    app.draw(*screen, 0);
    return screen;
  };
  expect(row_text(*drawn(), 31).find("Turn on") != std::string::npos, "the keys name the selected plugin's action");
  // Turn on: a diagnostics plugin says what it logs first, the whole notice; n turns nothing on
  auto out = app.key(plain(t::Key::Kind::enter));
  std::string asked = box_words(*drawn(), "Turn the plugin on");
  expect(out.requests.empty() &&
             asked.find("Turn Debugging on? While it is on, the server logs one line per request (never its text) and the "
                        "engine's own output, which can hold what the engine prints of your prompts and the paths of your model "
                        "files. Keep the server's log private, look through a log before you share it, and turn it off when you "
                        "are done.") != std::string::npos &&
             asked.find("y / n") != std::string::npos,
         "asked first, the whole notice: " + asked);
  expect(app.key(character("n")).requests.empty() && shown().find("Turn Debugging on?") == std::string::npos, "n: nothing");
  (void)app.key(plain(t::Key::Kind::enter));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0] == t::Request{"turn on Debugging", "POST", "/v1/strata/plugins/switch", R"({"id":"debugging","on":true})"},
         "y turns it on: " + (out.requests.empty() ? std::string{} : out.requests[0].purpose + " " + out.requests[0].body));
  if (out.requests.empty()) return;
  app.answer(out.requests[0], 200, R"({"id":"debugging","state":"on","restart_needed":true})");
  expect(app.status() == "turn on Debugging: done; it applies when the server starts again", "said: " + app.status());
  expect(polls_list(), "and the list is read again");
  std::string on = kPlugins;
  on.replace(on.find(R"("state":"off")"), 13U, R"("state":"on")");
  app.answer(list, 200, on);
  // Turn off asks nothing (the page asks nothing either)
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.size() == 1U && out.requests[0] == t::Request{"turn off Debugging", "POST", "/v1/strata/plugins/switch", R"({"id":"debugging","on":false})"},
         "Enter on one that is on turns it off: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  if (out.requests.empty()) return;
  app.answer(out.requests[0], 200, "{}");
  app.answer(list, 200, kPlugins);
  // Approve: asked first, saying its code runs with the server's rights
  (void)app.key(plain(t::Key::Kind::down));
  text = shown();
  expect(text.find("acme \xC2\xB7 tool \xC2\xB7 version 0.2.0 \xC2\xB7 from Example Labs \xC2\xB7 MIT") != std::string::npos &&
             text.find("never approved") != std::string::npos && text.find("acme.exe  cccc3333") != std::string::npos,
         "the selected plugin's facts, reason and files:\n" + text);
  out = app.key(plain(t::Key::Kind::enter));
  asked = box_words(*drawn(), "Approve the plugin");
  expect(out.requests.empty() &&
             asked.find("Approve Acme search 0.2.0 from Example Labs? Its code then runs on this PC, with this server's rights. "
                        "Only the files there now are approved: their SHA-256s are kept, and a changed file waits for approval "
                        "again.") != std::string::npos,
         "approval is asked first: " + asked);
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0] == t::Request{"approve Acme search", "POST", "/v1/strata/plugins/approve", R"({"id":"acme"})"},
         "y approves it: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  if (out.requests.empty()) return;
  app.answer(out.requests[0], 422,
             R"({"error":{"message":"it is already approved","type":"invalid_request_error","param":null,"code":"plugin_refused"}})");
  expect(app.status() == "approve Acme search: it is already approved", "a refusal in the server's words: " + app.status());
  // a refused plugin has no action: why is said
  (void)app.key(plain(t::Key::Kind::down));
  out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.empty() && app.status() == "Old family is refused: this server does not run engine plugins yet", "refused: " + app.status());
  // r reads the list again
  app.answer(list, 200, kPlugins);
  (void)app.key(character("r"));
  expect(polls_list(), "r reads the list again");
}

// The Plugins view on a standard 80x24 terminal and smaller, a selection kept on a shorter list, a change waited for
// (as the page's buttons wait), and a server that keeps no choices - the page's words for each.
void the_plugins_view_fits_and_waits_for_its_changes() {
  const t::Request list{"plugins", "POST", "/v1/strata/plugins", "{}"};
  const auto with = [](std::string json, const std::string& from, const std::string& to) {
    json.replace(json.find(from), from.size(), to);
    return json;
  };
  const auto first_two = [](const std::string& json) { return json.substr(0, json.find(R"(,{"id":"old")")) + "]}"; };
  const std::string running = with(kPlugins, R"("state":"off","reason":null,"running":false)", R"("state":"on","reason":null,"running":true)");
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  (void)app.key(character("7"));
  app.answer(list, 200, running);
  // 80x24: every tab, and each state whole, with whether it runs
  t::Screen standard(80, 24);
  app.draw(standard, 0);
  const auto line_of = [&standard](const std::string& part) {
    const int y = row_with(standard, part);
    return y < 0 ? std::string{} : row_text(standard, y);
  };
  expect(row_text(standard, 1).find("7 Plugins") != std::string::npos, "the tab row has Plugins at 80 columns: " + row_text(standard, 1));
  expect(line_of("Acme search").find("Waiting for approval") != std::string::npos && line_of("Debugging").find("running") != std::string::npos,
         "each state whole, and running, at 80 columns:\n" + all_text(standard));
  // 10 rows: the selected plugin's row stays in view, above the note
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::down));
  t::Screen low(120, 10);
  app.draw(low, 0);
  expect(row_with(low, "Old family") >= 0 && row_with(low, "Old family") < row_with(low, "A change applies"),
         "the selected plugin is in view above the note:\n" + all_text(low));
  // the list read again shorter: the selection is its last one, and one Up moves it
  app.answer(list, 200, first_two(running));
  (void)app.key(plain(t::Key::Kind::up));
  auto out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.size() == 1U && out.requests[0].purpose == "turn off Debugging",
         "one Up from the last of two is the first: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].purpose));
  // a change waits for its answer and the list read again: Enter does nothing meanwhile (the page's buttons wait too)
  expect(app.key(plain(t::Key::Kind::enter)).requests.empty(), "no second change while the first is under way");
  app.answer({"turn off Debugging", "POST", "/v1/strata/plugins/switch", R"({"id":"debugging","on":false})"}, 200, "{}");
  expect(app.key(plain(t::Key::Kind::enter)).requests.empty(), "nor before the list is read again");
  t::Screen waiting(120, 32);
  app.draw(waiting, 0);
  expect(row_text(waiting, 31).find("Enter") == std::string::npos, "the keys offer no Enter meanwhile: " + row_text(waiting, 31));
  app.answer(list, 200, first_two(kPlugins));
  out = app.key(plain(t::Key::Kind::enter));
  t::Screen again(120, 32);
  app.draw(again, 0);
  expect(out.requests.empty() && box_words(again, "Turn the plugin on").find("Turn Debugging on?") != std::string::npos,
         "then Enter acts again (Debugging is off: it asks first): " + box_words(again, "Turn the plugin on"));
  (void)app.key(character("n"));
  // a question taller than the screen says so
  app.answer(list, 200, with(kPlugins, R"("state":"off","reason":null)", R"("state":"changed","reason":"changed since approved")"));
  out = app.key(plain(t::Key::Kind::enter));
  t::Screen short_screen(40, 12);
  app.draw(short_screen, 0);
  expect(out.requests.empty() && box_words(short_screen, "Approve the plugin").find("enlarge the terminal to read it all") != std::string::npos,
         "a cut question says so: " + box_words(short_screen, "Approve the plugin"));
  (void)app.key(character("n"));
  // a server that keeps no choices: said, and no action offered (the page's buttons are disabled)
  app.answer(list, 200, with(kPlugins, R"("editable":true)", R"("editable":false)"));
  t::Screen read_only(120, 32);
  app.draw(read_only, 0);
  expect(all_text(read_only).find("Folder: /opt/m-strata/plugins (read only: this server keeps no choices)") != std::string::npos &&
             row_text(read_only, 31).find("Enter") == std::string::npos,
         "read only, as the page says it, and no Enter:\n" + all_text(read_only));
  // no plugins: the page's words
  app.answer(list, 200, R"({"folder":"/opt/m-strata/plugins","editable":true,"plugins":[]})");
  t::Screen empty(120, 32);
  app.draw(empty, 0);
  expect(all_text(empty).find("No plugins yet: each plugin is a folder holding its plugin.json, in the folder above.") != std::string::npos,
         "no plugins:\n" + all_text(empty));
}

// A notifier with a setting of each kind (two secrets: one set, one not) and two presets, as the server lists it
// (plugin_routes settings_json): a secret only as set or not, never its value.
const std::string kMailer =
    R"({"id":"mailer","kind":"notifier","name":"Mailer","version":"1.0.0","publisher":"Example Labs","license":"MIT",)"
    R"("about":"Sends a mail when a download finishes.","first_party":false,"state":"on","reason":null,"running":true,)"
    R"("manifest_sha256":"eeee5555","files":[],"switches":[],"settings":[)"
    R"({"name":"host","kind":"text","label":"Mail server","help":"The name of the server that sends the mail","required":true,)"
    R"("default":null,"value":"smtp.example.com"},)"
    R"({"name":"port","kind":"integer","label":"Port","help":"","required":false,"default":587,"min":1,"max":65535,"value":null},)"
    R"({"name":"security","kind":"choice","label":"Security","help":"","required":false,"default":"starttls",)"
    R"("choices":["none","starttls","tls"],"value":null},)"
    R"({"name":"on_finish","kind":"boolean","label":"Mail on finish","help":"","required":false,"default":true,"value":false},)"
    R"({"name":"status_page","kind":"url","label":"Status page","help":"","required":false,"default":null,"value":null},)"
    R"({"name":"to","kind":"text_list","label":"Recipients","help":"","required":true,"default":null,"max_items":2,"value":["ops"]},)"
    R"({"name":"password","kind":"secret","label":"Password","help":"","required":true,"set":true},)"
    R"({"name":"token","kind":"secret","label":"API token","help":"","required":true,"set":false}],)"
    R"("presets":[{"label":"Hosted","values":{"host":"smtp.example.net","port":465,"security":"tls"}},)"
    R"({"label":"This PC","values":{"host":"localhost","port":25,"security":"none","on_finish":true,"to":["root"]}}],)"
    R"("network":"the mail server smtp.example.com","settings_problem":"its setting token is required: set it in the Plugins tab"})";

// Trust by key (plugin spec section 11): the selected plugin's details say who signed it, or why its signature is not
// taken, as the page's card does.
void the_plugins_view_says_who_signed_a_plugin() {
  const t::Request list{"plugins", "POST", "/v1/strata/plugins", "{}"};
  const auto with = [](std::string json, const std::string& from, const std::string& to) {
    json.replace(json.find(from), from.size(), to);
    return json;
  };
  for (const bool trusted : {true, false}) {
    const std::string signature = trusted ? R"("signature":{"key":")" + std::string(64U, 'a') + R"(","signer":"Example Labs","problem":null},)"
                                          : R"("signature":{"key":")" + std::string(64U, 'a') +
                                                R"(","signer":null,"problem":"it is signed with a key this server does not trust"},)";
    const std::string listed = with(kPlugins, R"("plugins":[)", R"("plugins":[)" + with(kMailer, R"("network":)", signature + R"("network":)") + ",");
    t::App app(t::Capabilities{}, std::nullopt);
    app.server_state("http://127.0.0.1:8080", true);
    (void)app.key(character("7"));
    app.answer(list, 200, listed);
    auto screen = std::make_unique<t::Screen>(120, 40);
    app.draw(*screen, 0);
    const std::string text = all_text(*screen);
    const std::string words = trusted ? "Signed by Example Labs." : "Its signature is not taken: it is signed with a key this server does not trust.";
    expect(text.find(words) != std::string::npos, "the view shows: " + words + "\n" + text);
  }
}

// Trust by key (plugin spec section 11) in the terminal, as on the page: t on a plugin signed with a key the server does
// not trust offers to trust its publisher's key; k lists the trusted keys (the install's own marked), where d removes a
// publisher key (never the install's own) and a adds one - its name, then its key - each asking first, each a plugin
// change that waits for its answer and the list read again; Esc goes back to the plugins.
void the_plugins_view_trusts_and_drops_publisher_keys() {
  const t::Request list{"plugins", "POST", "/v1/strata/plugins", "{}"};
  const std::string install(64U, 'e');
  const std::string acme(64U, 'a');
  const std::string signer(64U, 'c');
  const std::string signed_mailer = [&signer] {
    std::string json = kMailer;
    json.replace(json.find(R"("network":)"), 10, R"("signature":{"key":")" + signer +
                                                     R"(","signer":null,"problem":"it is signed with a key this server does not trust"},"network":)");
    return json;
  }();
  std::string listed = kPlugins;
  listed.replace(listed.find(R"("plugins":[)"), 11, R"("plugins":[)" + signed_mailer + ",");
  listed.replace(listed.rfind('}'), 1, R"(,"keys":[{"key":")" + install + R"(","name":"this install's key","install":true},)" +
                                         R"({"key":")" + acme + R"(","name":"Acme","install":false}]})");
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  (void)app.key(character("7"));
  app.answer(list, 200, listed);
  const auto screen_of = [&app] {
    auto screen = std::make_unique<t::Screen>(120, 32);
    app.draw(*screen, 0);
    return screen;
  };
  const auto type = [&app](const std::string& words) {
    for (const char c : words) (void)app.key(character(std::string(1U, c)));
  };
  const auto settle = [&app, &list, &listed](const t::Request& sent) {
    app.answer(sent, 200, "{}");
    app.answer(list, 200, listed);
  };
  expect(row_text(*screen_of(), 31).find("t  Trust key") != std::string::npos && row_text(*screen_of(), 31).find("k  Keys") != std::string::npos,
         "the keys offer t (its signer is not trusted) and k: " + row_text(*screen_of(), 31));
  // t: trust the signer's key, asked first
  auto out = app.key(character("t"));
  expect(out.requests.empty() && box_words(*screen_of(), "Trust a publisher's key").find("Example Labs's key (" + signer.substr(0, 16) + "...)") !=
                                     std::string::npos,
         "t asks first: " + box_words(*screen_of(), "Trust a publisher's key"));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/plugins/keys" &&
             out.requests[0].body == R"({"add":")" + signer + R"(","name":"Example Labs"})",
         "y trusts it: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].body));
  if (!out.requests.empty()) settle(out.requests[0]);
  // k: the keys
  (void)app.key(character("k"));
  auto keys = screen_of();
  const std::string text = all_text(*keys);
  const int install_row = row_with(*keys, install);
  expect(text.find("Publisher keys") != std::string::npos && install_row >= 0 && row_text(*keys, install_row).find("this install's key") != std::string::npos &&
             row_with(*keys, acme) >= 0 && row_with(*keys, "Acme") >= 0,
         "k lists the keys, the install's own marked:\n" + text);
  // the trust's answer first, whole, then the keys the room it leaves takes; once it clears, a and Esc
  expect(row_text(*keys, 31).find("trust Example Labs's key: done; it applies when the server starts again") != std::string::npos &&
             row_text(*keys, 31).find("a  Add") != std::string::npos,
         "the answer whole, then a: " + row_text(*keys, 31));
  app.tick(0);
  app.tick(t::App::kStatusSeconds);
  keys = screen_of();
  expect(row_text(*keys, 31).find("a  Add") != std::string::npos && row_text(*keys, 31).find("Esc  Back") != std::string::npos,
         "and offers a and Esc: " + row_text(*keys, 31));
  // the install's own cannot be removed
  expect(app.key(character("d")).requests.empty() && all_text(*screen_of()).find("The install's own key cannot be removed") != std::string::npos,
         "d on the install's key: said, nothing sent");
  // d on a publisher key: asked, then removed
  (void)app.key(plain(t::Key::Kind::down));
  out = app.key(character("d"));
  expect(out.requests.empty() && box_words(*screen_of(), "Stop trusting a key").find("Stop trusting Acme's key?") != std::string::npos,
         "d asks first: " + box_words(*screen_of(), "Stop trusting a key"));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0].body == R"({"remove":")" + acme + R"("})",
         "y removes it: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].body));
  expect(app.key(character("a")).requests.empty() && row_with(*screen_of(), "Trust a publisher's key") < 0,
         "nothing more while the change is under way");
  if (!out.requests.empty()) settle(out.requests[0]);
  // a: its name, then its key, then the question
  (void)app.key(character("a"));
  expect(box_words(*screen_of(), "Trust a publisher's key").find("name") != std::string::npos, "a asks for the name first");
  type("Beta Labs");
  (void)app.key(plain(t::Key::Kind::enter));
  expect(box_words(*screen_of(), "Trust a publisher's key").find("Beta Labs's public key") != std::string::npos, "then for the key");
  type(std::string(64U, 'B'));
  (void)app.key(plain(t::Key::Kind::enter));
  expect(box_words(*screen_of(), "Trust a publisher's key").find("Beta Labs's key (bbbbbbbbbbbbbbbb...)") != std::string::npos,
         "then asks, the key in lowercase: " + box_words(*screen_of(), "Trust a publisher's key"));
  out = app.key(character("y"));
  expect(out.requests.size() == 1U && out.requests[0].body == R"({"add":")" + std::string(64U, 'b') + R"(","name":"Beta Labs"})",
         "y adds it: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].body));
  if (!out.requests.empty()) settle(out.requests[0]);
  // Esc: back to the plugins
  (void)app.key(plain(t::Key::Kind::escape));
  expect(all_text(*screen_of()).find("Publisher keys") == std::string::npos && row_with(*screen_of(), "Mailer") >= 0, "Esc: the plugins again");
  // the list read again without keys (a server that does not say them) while they are in view: the plugins again
  (void)app.key(character("k"));
  app.answer(list, 200, kPlugins);
  expect(all_text(*screen_of()).find("Publisher keys") == std::string::npos && row_with(*screen_of(), "Debugging") >= 0,
         "a list without keys: the plugins again:\n" + all_text(*screen_of()));
}

// The owner (2026-10-30, decision 6): a tool's or notifier's settings are set in the Plugins view as on the page's card -
// o focuses them, Enter changes the one in focus (a switch flips, a choice moves on, the others in the text field, a
// secret typed unseen), c puts it back to its default (a secret: removed), p fills in a preset, w saves them as one plugin
// change (it waits as Enter's do) and u drops them. A secret is only ever said set or not.
void the_plugins_settings_are_edited_then_saved() {
  const t::Request list{"plugins", "POST", "/v1/strata/plugins", "{}"};
  const auto with = [](std::string json, const std::string& from, const std::string& to) {
    json.replace(json.find(from), from.size(), to);
    return json;
  };
  const std::string listed = with(kPlugins, R"("plugins":[)", R"("plugins":[)" + kMailer + ",");
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  (void)app.key(character("7"));
  app.answer(list, 200, listed);
  const auto drawn = [&app](const int height) {
    auto screen = std::make_unique<t::Screen>(120, height);
    app.draw(*screen, 0);
    return screen;
  };
  // a setting's row ("" when it is not drawn)
  const auto row_of = [](const t::Screen& screen, const std::string& label) {
    const int y = row_with(screen, label);
    return y < 0 ? std::string{} : row_text(screen, y);
  };
  // the selected plugin's settings below its details: what it connects to, what keeps it from running, then each
  // setting's label and value - its default when it has none, a secret as set or not
  auto screen = drawn(40);
  std::string text = all_text(*screen);
  for (const char* words : {"Connects to the mail server smtp.example.com.", "Not ready: its setting token is required: set it in the Plugins tab",
                            "Settings  (o to change)"}) {
    expect(text.find(words) != std::string::npos, std::string{"the view shows: "} + words + "\n" + text);
  }
  expect(row_of(*screen, "Mail server (needed)").find(" smtp.example.com ") != std::string::npos &&
             row_of(*screen, "Port").find(" 587 (default) ") != std::string::npos &&
             row_of(*screen, "Security").find(" starttls (default) ") != std::string::npos &&
             row_of(*screen, "Mail on finish").find(" off ") != std::string::npos &&
             row_of(*screen, "Mail on finish").find("(default)") == std::string::npos &&
             row_of(*screen, "Status page").find(" not set ") != std::string::npos &&
             row_of(*screen, "Recipients (needed)").find(" ops ") != std::string::npos,
         "each setting's value, else its default:\n" + text);
  expect(row_of(*screen, "Password (needed)").find(" set ") != std::string::npos && row_of(*screen, "Password").find("not set") == std::string::npos &&
             row_of(*screen, "API token (needed)").find(" not set ") != std::string::npos,
         "a secret as set or not:\n" + text);
  expect(row_text(*screen, 39).find("o  Settings") != std::string::npos, "the keys offer the settings: " + row_text(*screen, 39));
  // o focuses them: their own keys, and neither q nor the views' keys (1-7, Tab) do anything meanwhile
  (void)app.key(character("o"));
  const std::string focused_keys = row_text(*drawn(40), 39);
  expect(focused_keys.find("Up/Down  Setting") != std::string::npos && focused_keys.find("Enter  Change") != std::string::npos &&
             focused_keys.find("c  Clear") != std::string::npos && focused_keys.find("p  Preset") != std::string::npos &&
             focused_keys.find("Esc  Back") != std::string::npos && focused_keys.find("Quit") == std::string::npos &&
             focused_keys.find("Tab") == std::string::npos && focused_keys.find("Save") == std::string::npos,
         "the keys while the settings are focused: " + focused_keys);
  (void)app.key(character("q"));
  const auto after_q = app.key(character("y"));
  expect(!after_q.quit && after_q.requests.empty(), "q does not quit (the y after it answers no question)");
  for (const char* view : {"1", "5", "7"}) (void)app.key(character(view));
  (void)app.key(plain(t::Key::Kind::tab));
  (void)app.key(plain(t::Key::Kind::back_tab));
  expect(app.view() == t::View::plugins && row_text(*drawn(40), 39).find("Up/Down  Setting") != std::string::npos,
         "1-7 and Tab neither switch the view nor leave the settings");
  expect(all_text(*drawn(40)).find("The name of the server that sends the mail") != std::string::npos, "the focused setting's help");
  // a text: Enter opens the field on its value
  const auto typed_in = [&app](const std::string& typed) {
    for (int i = 0; i < 40; ++i) (void)app.key(plain(t::Key::Kind::backspace));
    for (const char c : typed) (void)app.key(character(std::string(1U, c)));
    (void)app.key(plain(t::Key::Kind::enter));
  };
  (void)app.key(plain(t::Key::Kind::enter));
  expect(all_text(*drawn(40)).find("smtp.example.com_") != std::string::npos, "the field starts with the value:\n" + all_text(*drawn(40)));
  typed_in("mail.example.org");
  // a whole number in its range, or the status line says what it takes and it is not kept
  (void)app.key(plain(t::Key::Kind::down));
  for (const char* refused : {"70000", "2.5", "x1", "0"}) {
    (void)app.key(plain(t::Key::Kind::enter));
    typed_in(refused);
    expect(app.status() == "Port takes a whole number from 1 to 65535", std::string{refused} + " is refused: " + app.status());
  }
  expect(row_of(*drawn(40), "Port").find("unsaved") == std::string::npos, "a refused number is not kept: " + row_of(*drawn(40), "Port"));
  (void)app.key(plain(t::Key::Kind::enter));
  typed_in("2525");
  // a choice: Enter moves to its next choice (from its default), Right and Left cycle it
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  expect(row_of(*drawn(40), "Security").find(" tls  * unsaved") != std::string::npos, "Enter: the next choice: " + row_of(*drawn(40), "Security"));
  (void)app.key(plain(t::Key::Kind::right));
  expect(row_of(*drawn(40), "Security").find(" none  * unsaved") != std::string::npos, "Right: round to the first: " + row_of(*drawn(40), "Security"));
  (void)app.key(plain(t::Key::Kind::left));
  // a switch flips; an address in the field (none yet: it starts empty)
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  typed_in("https://status.example.org");
  // a list as its items, separated by commas, no more than it takes
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  expect(all_text(*drawn(40)).find("ops_") != std::string::npos, "a list's field starts with its items");
  typed_in("ops, admin, extra");
  expect(app.status() == "Recipients takes at most 2 items" && row_of(*drawn(40), "Recipients").find("unsaved") == std::string::npos,
         "too many items are refused: " + app.status());
  (void)app.key(plain(t::Key::Kind::enter));
  typed_in("ops,, admin ");
  // c removes a secret that is set; another is typed unseen, and only ever said set
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(character("c"));
  (void)app.key(plain(t::Key::Kind::down));
  (void)app.key(plain(t::Key::Kind::enter));
  for (const char c : std::string_view{"s3cret-token"}) (void)app.key(character(std::string(1U, c)));
  text = all_text(*drawn(40));
  expect(text.find(std::string(12U, '*') + "_") != std::string::npos && text.find("s3cret-token") == std::string::npos,
         "a secret is typed unseen:\n" + text);
  (void)app.key(plain(t::Key::Kind::enter));
  screen = drawn(40);
  text = all_text(*screen);
  expect(row_of(*screen, "Mail server").find(" mail.example.org  * unsaved") != std::string::npos &&
             row_of(*screen, "Port").find(" 2525  * unsaved") != std::string::npos &&
             row_of(*screen, "Security").find(" tls  * unsaved") != std::string::npos &&
             row_of(*screen, "Mail on finish").find(" on  * unsaved") != std::string::npos &&
             row_of(*screen, "Status page").find(" https://status.example.org  * unsaved") != std::string::npos &&
             row_of(*screen, "Recipients").find(" ops, admin  * unsaved") != std::string::npos &&
             row_of(*screen, "Password").find(" not set  * unsaved") != std::string::npos &&
             row_of(*screen, "API token").find(" set  * unsaved") != std::string::npos && row_of(*screen, "API token").find("not set") == std::string::npos &&
             text.find("s3cret-token") == std::string::npos && text.find("Settings  (8 unsaved change(s): w saves, u undoes)") != std::string::npos,
         "the unsaved changes, as Settings marks them:\n" + text);
  // a short screen keeps the focused setting in view, above the note
  const auto low = drawn(20);
  expect(row_with(*low, "API token") >= 0 && row_with(*low, "API token") < row_with(*low, "A change applies"),
         "the focused setting in view on a short screen:\n" + all_text(*low));
  // w saves them, in the settings' order: a secret removed as null
  auto out = app.key(character("w"));
  const t::Request save{"save Mailer's settings", "POST", "/v1/strata/plugins/settings",
                        R"({"id":"mailer","values":{"host":"mail.example.org","port":2525,"security":"tls","on_finish":true,)"
                        R"("status_page":"https://status.example.org","to":["ops","admin"],"password":null,"token":"s3cret-token"}})"};
  expect(out.requests.size() == 1U && out.requests[0] == save,
         "w saves the changes: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].purpose + " " + out.requests[0].body));
  if (out.requests.empty()) return;
  // a plugin change: nothing more is sent until it is answered and the list read again, and the changes stay until then
  expect(app.key(character("w")).requests.empty(), "no second save while the first is under way");
  screen = drawn(40);
  expect(row_text(*screen, 39).find("Save") == std::string::npos && row_of(*screen, "Port").find(" 2525  * unsaved") != std::string::npos,
         "meanwhile no Save, and the changes stay: " + row_text(*screen, 39));
  app.answer(out.requests[0], 200, R"({"id":"mailer","saved":true,"restart_needed":true})");
  expect(app.status() == "save Mailer's settings: done; it applies when the server starts again", "saved: " + app.status());
  const auto polls = app.polls();
  expect(std::find(polls.begin(), polls.end(), list) != polls.end(), "and the list is read again");
  expect(app.key(character("w")).requests.empty() && row_of(*drawn(40), "Port").find(" 2525  * unsaved") != std::string::npos,
         "nor before the list is read again; the changes kept until then");
  app.answer(list, 200,
             with(with(listed, R"("value":"smtp.example.com")", R"("value":"mail.example.org")"), R"("max":65535,"value":null)",
                  R"("max":65535,"value":2525)"));
  screen = drawn(40);
  text = all_text(*screen);
  expect(text.find("unsaved") == std::string::npos && row_of(*screen, "Port").find(" 2525 ") != std::string::npos &&
             row_of(*screen, "Mail server").find(" mail.example.org ") != std::string::npos,
         "then the saved changes are dropped, the list's values shown:\n" + text);
  // p fills in the presets in turn (the page's "Fill in" buttons); a refused save keeps its changes
  (void)app.key(character("p"));
  screen = drawn(40);
  expect(row_of(*screen, "Mail server").find(" smtp.example.net  * unsaved") != std::string::npos &&
             row_of(*screen, "Port").find(" 465  * unsaved") != std::string::npos &&
             row_of(*screen, "Security").find(" tls  * unsaved") != std::string::npos && app.status() == "Filled in Hosted; w saves it",
         "p fills in the first preset: " + app.status() + "\n" + all_text(*screen));
  (void)app.key(character("p"));
  out = app.key(character("w"));
  const t::Request preset{"save Mailer's settings", "POST", "/v1/strata/plugins/settings",
                          R"({"id":"mailer","values":{"host":"localhost","port":25,"security":"none","on_finish":true,"to":["root"]}})"};
  expect(out.requests.size() == 1U && out.requests[0] == preset,
         "then the next: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].body));
  if (out.requests.empty()) return;
  app.answer(out.requests[0], 422,
             R"({"error":{"message":"this server was started without a config file to write","type":"invalid_request_error","param":null,"code":"plugin_refused"}})");
  expect(app.status() == "save Mailer's settings: this server was started without a config file to write", "refused: " + app.status());
  app.answer(list, 200, listed);
  out = app.key(character("w"));
  expect(out.requests == std::vector<t::Request>{preset}, "a refused save keeps its changes");
  if (out.requests.empty()) return;
  app.answer(out.requests[0], 422, R"({"error":{"message":"refused again"}})");
  app.answer(list, 200, listed);
  // u drops the changes; c puts a setting back to its default, sent as null
  (void)app.key(character("u"));
  expect(all_text(*drawn(40)).find("unsaved") == std::string::npos, "u drops them");
  for (int i = 0; i < 4; ++i) (void)app.key(plain(t::Key::Kind::up));
  (void)app.key(character("c"));
  expect(row_of(*drawn(40), "Mail on finish").find(" on (default)  * unsaved") != std::string::npos,
         "c: back to the default: " + row_of(*drawn(40), "Mail on finish"));
  out = app.key(character("w"));
  expect(out.requests.size() == 1U && out.requests[0].body == R"({"id":"mailer","values":{"on_finish":null}})",
         "c sends null: " + (out.requests.empty() ? std::string{"(none)"} : out.requests[0].body));
  // Esc leaves the settings, and o focuses and leaves them as well; q quits again
  (void)app.key(plain(t::Key::Kind::escape));
  expect(row_text(*drawn(40), 39).find("Quit") != std::string::npos, "Esc leaves the settings: " + row_text(*drawn(40), 39));
  (void)app.key(character("o"));
  expect(row_text(*drawn(40), 39).find("Esc  Back") != std::string::npos, "o focuses them again");
  (void)app.key(character("o"));
  expect(row_text(*drawn(40), 39).find("Quit") != std::string::npos, "and o leaves them");
  expect(!app.key(character("q")).quit && app.key(character("y")).quit, "q quits again");
}

}  // namespace

// The owner (2026-10-29, then 2026-10-30 as a plugin): while the Debugging plugin is on, the TUI shows what it logs and
// starts the server only on y; any other key quits without starting it (a wheel turn is no answer). The screen fits a
// small terminal.
void the_debugging_notice_waits_for_y() {
  using Kind = t::Key::Kind;
  for (const char* yes : {"y", "Y"}) {
    expect(t::debugging_gate(t::Key{Kind::character, yes}) == t::GateAnswer::accepted, std::string{"accepted: "} + yes);
  }
  for (const t::Key& no : {t::Key{Kind::character, "n"}, t::Key{Kind::character, "q"}, t::Key{Kind::escape},
                          t::Key{Kind::enter}, t::Key{Kind::ctrl, "c"}}) {
    expect(t::debugging_gate(no) == t::GateAnswer::declined, "any other key quits: " + no.text);
  }
  expect(t::debugging_gate(t::Key{Kind::wheel_down}) == t::GateAnswer::waiting, "a wheel turn is no answer");
  const std::string notice = "The Debugging plugin is on.\n\nIt logs a lot, so keep the log private and read it before you share it.";
  t::Screen screen(40, 16);
  t::draw_debugging_notice(screen, notice);
  const std::string shown = all_text(screen);
  expect(shown.find("Debugging is on") != std::string::npos && shown.find("The Debugging plugin is on.") != std::string::npos,
         "its title and its text: " + shown);
  expect(row_with(screen, "It logs a lot") >= 0 && row_with(screen, "read it before") > row_with(screen, "It logs a lot"),
         "a long line wraps at the screen's width: " + shown);
  expect(row_with(screen, "y: start") > row_with(screen, "read it before"), "the keys under the text: " + shown);
}

// The owner (2026-10-29): "starting an uncalibrated server recommend calibrating first and when answered yes, it will
// calibrate. However, during calibration, allow users to cancel calibration if they choose to."
// Enter on a card never calibrated asks: c calibrates, then starts (recommended); s starts without (calibrate false);
// any other key starts nothing. x cancels a calibration.
void an_uncalibrated_start_asks_to_calibrate_first() {
  t::App app(t::Capabilities{}, std::nullopt);
  app.server_state("http://127.0.0.1:8080", true);
  (void)app.key(character("2"));
  app.answer({"models", "GET", "/v1/strata/models", {}}, 200, kModels);
  const auto shown = [&app] {
    t::Screen screen(140, 30);
    app.draw(screen, 0);
    return all_text(screen);
  };
  (void)app.key(plain(t::Key::Kind::down));  // MiniMax, not calibrated (Qwen3.8 runs)
  auto out = app.key(plain(t::Key::Kind::enter));
  expect(out.requests.empty() && shown().find("Stop Qwen3.8 and start MiniMax? It is not calibrated") != std::string::npos &&
             shown().find("c: calibrate, then start (recommended)") != std::string::npos &&
             shown().find("s: start without calibrating") != std::string::npos,
         "Enter asks first: " + shown());
  out = app.key(character("c"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/calibrate" &&
             out.requests[0].body == R"({"name":"MiniMax","options":{},"then_start":true})",
         "c calibrates, then starts: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  (void)app.key(plain(t::Key::Kind::enter));
  out = app.key(character("s"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/start" &&
             out.requests[0].body == R"({"name":"MiniMax","options":{},"calibrate":false})",
         "s starts without calibrating: " + (out.requests.empty() ? std::string{} : out.requests[0].body));
  (void)app.key(plain(t::Key::Kind::enter));
  expect(app.key(character("n")).requests.empty() && shown().find("It is not calibrated") == std::string::npos,
         "any other key starts nothing");
  out = app.key(character("x"));
  expect(out.requests.size() == 1U && out.requests[0].path == "/v1/strata/models/calibrate/cancel", "x cancels a calibration");
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_screen_draws_text_boxes_and_gauges", the_screen_draws_text_boxes_and_gauges},
      {"the_debugging_notice_waits_for_y", the_debugging_notice_waits_for_y},
      {"plugins_are_approved_and_switched", plugins_are_approved_and_switched},
      {"the_plugins_view_fits_and_waits_for_its_changes", the_plugins_view_fits_and_waits_for_its_changes},
      {"the_plugins_settings_are_edited_then_saved", the_plugins_settings_are_edited_then_saved},
      {"the_plugins_view_says_who_signed_a_plugin", the_plugins_view_says_who_signed_a_plugin},
      {"the_plugins_view_trusts_and_drops_publisher_keys", the_plugins_view_trusts_and_drops_publisher_keys},
      {"an_uncalibrated_start_asks_to_calibrate_first", an_uncalibrated_start_asks_to_calibrate_first},
      {"charts_are_braille_or_blocks", charts_are_braille_or_blocks},
      {"only_the_changes_are_written", only_the_changes_are_written},
      {"keys_decode_from_vt_sequences", keys_decode_from_vt_sequences},
      {"the_monitor_draws_the_servers_readings", the_monitor_draws_the_servers_readings},
      {"the_views_switch_and_keys_become_requests", the_views_switch_and_keys_become_requests},
      {"settings_are_edited_then_saved", settings_are_edited_then_saved},
      {"a_message_is_shown_whole_before_the_keys", a_message_is_shown_whole_before_the_keys},
      {"downloads_say_what_is_on_this_pc", downloads_say_what_is_on_this_pc},
      {"quitting_asks_first", quitting_asks_first},
  }, failures);
}
