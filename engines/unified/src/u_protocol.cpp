#include "u_protocol.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace u {
namespace {

bool printable(std::string_view line) {
  for (const unsigned char ch : line) {
    if (ch < 0x20 || ch > 0x7E) return false;
  }
  return true;
}

// A strict u32: only digits, no sign, no leading zero (except "0"), within the u32 range.
bool parse_u32(std::string_view s, std::uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  if (s.size() > 1 && s.front() == '0') return false;
  for (const char ch : s) {
    if (ch < '0' || ch > '9') return false;
  }
  unsigned long long v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size() || v > 0xFFFFFFFFull) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}

// A nonnegative decimal: digits and at most one '.', at least one digit; finite and >= 0.
bool parse_decimal(std::string_view s, double& out) {
  if (s.empty() || s.size() > 32) return false;
  int digits = 0;
  int dots = 0;
  for (const char ch : s) {
    if (ch == '.') {
      if (++dots > 1) return false;
    } else if (ch >= '0' && ch <= '9') {
      ++digits;
    } else {
      return false;
    }
  }
  if (digits == 0) return false;
  double v = 0.0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(v) || v < 0.0) return false;
  out = v;
  return true;
}

// Splits on single spaces; a leading, trailing or doubled space (an empty field) is refused.
bool split_fields(std::string_view line, std::vector<std::string_view>& out) {
  std::size_t at = 0;
  for (;;) {
    const std::size_t sp = line.find(' ', at);
    const std::size_t end = sp == std::string_view::npos ? line.size() : sp;
    if (end == at) return false;  // empty field
    out.push_back(line.substr(at, end - at));
    if (sp == std::string_view::npos) break;
    at = sp + 1;
  }
  return true;
}

bool parse_kv(std::string_view field, KeyVal& out) {
  const std::size_t eq = field.find('=');
  if (eq == std::string_view::npos || eq == 0) return false;
  out.key = std::string(field.substr(0, eq));
  out.value = std::string(field.substr(eq + 1));
  return true;
}

bool parse_kv_list(const std::vector<std::string_view>& fields, std::size_t from, std::size_t to,
                   std::vector<KeyVal>& out) {
  for (std::size_t i = from; i < to; ++i) {
    KeyVal kv;
    if (!parse_kv(fields[i], kv)) return false;
    out.push_back(std::move(kv));
  }
  return true;
}

bool parse_ids(std::string_view field, std::vector<std::uint32_t>& out) {
  std::size_t at = 0;
  for (;;) {
    const std::size_t comma = field.find(',', at);
    const std::size_t end = comma == std::string_view::npos ? field.size() : comma;
    std::uint32_t id = 0;
    if (!parse_u32(field.substr(at, end - at), id)) return false;
    out.push_back(id);
    if (comma == std::string_view::npos) break;
    at = comma + 1;
  }
  return !out.empty();
}

bool parse_finish(std::string_view s, Finish& out) {
  if (s == "stop") { out = Finish::stop; return true; }
  if (s == "length") { out = Finish::length; return true; }
  if (s == "cancel") { out = Finish::cancel; return true; }
  if (s == "nonfinite") { out = Finish::nonfinite; return true; }
  return false;
}

using R = std::variant<Record, ParseError>;
R err(const char* what) { return ParseError{what}; }

std::string decimal(double v) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.3f", v);
  return std::string(buffer);
}

std::string kv_suffix(const std::vector<KeyVal>& fields) {
  std::string s;
  for (const auto& kv : fields) {
    s += ' ';
    s += kv.key;
    s += '=';
    s += kv.value;
  }
  return s;
}

struct EmitVisitor final {
  std::string operator()(const Info& x) const { return "INFO" + kv_suffix(x.fields); }
  std::string operator()(const Ready& x) const {
    return "READY " + std::to_string(x.ctx) + (x.stop_honored ? " stop" : "");
  }
  std::string operator()(const Gen& x) const {
    std::string s = "GEN " + std::to_string(x.max_new) + kv_suffix(x.params) + ' ';
    for (std::size_t i = 0; i < x.ids.size(); ++i) {
      if (i != 0) s += ',';
      s += std::to_string(x.ids[i]);
    }
    return s;
  }
  std::string operator()(const Stop&) const { return "STOP"; }
  std::string operator()(const Quit&) const { return "QUIT"; }
  std::string operator()(const Token& x) const { return "T " + std::to_string(x.id); }
  std::string operator()(const PromptProgress& x) const {
    return "PP " + std::to_string(x.done) + ' ' + std::to_string(x.total) + ' ' + decimal(x.ms) + ' ' + decimal(x.tok_s);
  }
  std::string operator()(const Stat& x) const { return "STAT" + kv_suffix(x.fields); }
  std::string operator()(const Done& x) const {
    return "DONE " + std::to_string(x.produced) + ' ' + std::to_string(x.prompt) + ' ' + decimal(x.prompt_ms) + ' ' +
           decimal(x.decode_ms) + ' ' + std::string(finish_name(x.finish)) + ' ' + std::to_string(x.spec_hits) + ' ' +
           std::to_string(x.spec_steps) + ' ' + std::to_string(x.reused);
  }
  std::string operator()(const Err& x) const { return x.text.empty() ? std::string("ERR") : "ERR " + x.text; }
};

}  // namespace

std::string_view finish_name(Finish f) {
  switch (f) {
    case Finish::stop: return "stop";
    case Finish::length: return "length";
    case Finish::cancel: return "cancel";
    case Finish::nonfinite: return "nonfinite";
  }
  return "length";
}

std::variant<Record, ParseError> parse_line(std::string_view line) {
  if (line.empty()) return err("empty line");
  if (line.size() > kMaxLineBytes) return err("line exceeds 8192 bytes");
  if (!printable(line)) return err("line has a non-printable byte");

  // ERR carries free-form text that may contain spaces; it is not field-split.
  if (line == "ERR") return Record{Err{std::string{}}};
  if (line.starts_with("ERR ")) return Record{Err{std::string(line.substr(4))}};

  std::vector<std::string_view> f;
  if (!split_fields(line, f)) return err("malformed spacing");
  const std::string_view head = f.front();

  if (head == "INFO") {
    Info info;
    if (!parse_kv_list(f, 1, f.size(), info.fields)) return err("INFO field is not key=value");
    return Record{std::move(info)};
  }
  if (head == "STAT") {
    Stat stat;
    if (!parse_kv_list(f, 1, f.size(), stat.fields)) return err("STAT field is not key=value");
    return Record{std::move(stat)};
  }
  if (head == "READY") {
    if (f.size() < 2 || f.size() > 3) return err("READY takes <ctx> [stop]");
    Ready ready;
    if (!parse_u32(f[1], ready.ctx)) return err("READY ctx is not a u32");
    if (f.size() == 3) {
      if (f[2] != "stop") return err("READY second field must be 'stop'");
      ready.stop_honored = true;
    }
    return Record{ready};
  }
  if (head == "GEN") {
    if (f.size() < 3) return err("GEN takes <max_new> [key=value ...] <ids>");
    Gen gen;
    if (!parse_u32(f[1], gen.max_new) || gen.max_new == 0) return err("GEN max_new is not a positive u32");
    if (!parse_kv_list(f, 2, f.size() - 1, gen.params)) return err("GEN param is not key=value");
    if (!parse_ids(f.back(), gen.ids)) return err("GEN id list is malformed");
    return Record{std::move(gen)};
  }
  if (head == "STOP") {
    if (f.size() != 1) return err("STOP takes no fields");
    return Record{Stop{}};
  }
  if (head == "QUIT") {
    if (f.size() != 1) return err("QUIT takes no fields");
    return Record{Quit{}};
  }
  if (head == "T") {
    if (f.size() != 2) return err("T takes one id");
    Token token;
    if (!parse_u32(f[1], token.id)) return err("T id is not a u32");
    return Record{token};
  }
  if (head == "PP") {
    if (f.size() != 5) return err("PP takes <done> <total> <ms> <tok_s>");
    PromptProgress pp;
    if (!parse_u32(f[1], pp.done) || !parse_u32(f[2], pp.total)) return err("PP counts are not u32");
    if (!parse_decimal(f[3], pp.ms) || !parse_decimal(f[4], pp.tok_s)) return err("PP timings are not decimals");
    return Record{pp};
  }
  if (head == "DONE") {
    if (f.size() != 9) return err("DONE takes eight fields");
    Done done;
    if (!parse_u32(f[1], done.produced) || !parse_u32(f[2], done.prompt)) return err("DONE counts are not u32");
    if (!parse_decimal(f[3], done.prompt_ms) || !parse_decimal(f[4], done.decode_ms)) return err("DONE timings are not decimals");
    if (!parse_finish(f[5], done.finish)) return err("DONE finish is not stop|length|cancel|nonfinite");
    if (!parse_u32(f[6], done.spec_hits) || !parse_u32(f[7], done.spec_steps) || !parse_u32(f[8], done.reused)) {
      return err("DONE spec/reuse counts are not u32");
    }
    return Record{done};
  }
  return err("unknown record");
}

std::string emit(const Record& r) { return std::visit(EmitVisitor{}, r); }

bool is_terminal(const Record& r) {
  return std::holds_alternative<Done>(r) || std::holds_alternative<Err>(r);
}

bool allowed_before_ready(const Record& r) {
  return std::holds_alternative<Info>(r) || std::holds_alternative<Ready>(r);
}

}  // namespace u
