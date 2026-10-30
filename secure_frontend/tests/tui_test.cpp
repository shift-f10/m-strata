// strata-tui (spec docs/superpowers/specs/2026-10-28-server-tui-design.md; the owner: "a TUI with the same settings and
// options ... monitoring the default view in a nice graph dashboard"): the screen and its VT output, the keys, the views
// drawn from the server's answers, and the keys' requests.
#include "secure_frontend/tui_app.hpp"
#include "secure_frontend/tui_input.hpp"
#include "secure_frontend/tui_screen.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <limits>
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
    R"("calibration":{"state":"missing","first_start":false,"running":null,"args":[],"auto":false,"error":null,)"
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

}  // namespace

// The owner (2026-10-29): a debugging build's TUI shows what that build logs and starts the server only on y; any other
// key quits without starting it (a wheel turn is no answer). The screen fits a small terminal.
void a_debugging_build_waits_for_y() {
  using Kind = t::Key::Kind;
  for (const char* yes : {"y", "Y"}) {
    expect(t::debugging_gate(t::Key{Kind::character, yes}) == t::GateAnswer::accepted, std::string{"accepted: "} + yes);
  }
  for (const t::Key& no : {t::Key{Kind::character, "n"}, t::Key{Kind::character, "q"}, t::Key{Kind::escape},
                          t::Key{Kind::enter}, t::Key{Kind::ctrl, "c"}}) {
    expect(t::debugging_gate(no) == t::GateAnswer::declined, "any other key quits: " + no.text);
  }
  expect(t::debugging_gate(t::Key{Kind::wheel_down}) == t::GateAnswer::waiting, "a wheel turn is no answer");
  const std::string notice = "This is a debugging build.\n\nIt logs a lot, so keep the log private and read it before you share it.";
  t::Screen screen(40, 16);
  t::draw_debugging_notice(screen, notice);
  const std::string shown = all_text(screen);
  expect(shown.find("Debugging build") != std::string::npos && shown.find("This is a debugging build.") != std::string::npos,
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
      {"a_debugging_build_waits_for_y", a_debugging_build_waits_for_y},
      {"an_uncalibrated_start_asks_to_calibrate_first", an_uncalibrated_start_asks_to_calibrate_first},
      {"charts_are_braille_or_blocks", charts_are_braille_or_blocks},
      {"only_the_changes_are_written", only_the_changes_are_written},
      {"keys_decode_from_vt_sequences", keys_decode_from_vt_sequences},
      {"the_monitor_draws_the_servers_readings", the_monitor_draws_the_servers_readings},
      {"the_views_switch_and_keys_become_requests", the_views_switch_and_keys_become_requests},
      {"settings_are_edited_then_saved", settings_are_edited_then_saved},
      {"downloads_say_what_is_on_this_pc", downloads_say_what_is_on_this_pc},
      {"quitting_asks_first", quitting_asks_first},
  }, failures);
}
