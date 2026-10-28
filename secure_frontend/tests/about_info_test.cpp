// The page's About (owner 2026-10-27: "in the about, it should list the build of our engines and also list the hardware
// of the system"): the engine folder's build record (make.py's engine/build.json) read and checked, this PC's
// hardware, and the JSON GET /v1/strata/about answers.
#include "secure_frontend/about_info.hpp"
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

const char* const kRecord = R"({
  "schema_version": 1,
  "built": "2026-10-27T23:00:00Z",
  "commit": "0123456789ab",
  "ggml": "3cf03257f219",
  "cuda_arch": "native",
  "features": ["email", "search"],
  "engines": [
    {"library": "multiengine_strata.dll", "models": "Qwen3.8-Flash family, Qwen3.6", "source": "upstream Strata, engine 0.1.51", "bytes": 51380224},
    {"library": "multiengine_raon1.dll", "models": "Raon-1-mini", "source": "our port of Project Maya v1.1.27's GLM engine, engine 0.1.49.2", "bytes": 53477376}
  ]
}
)";

void the_build_record_is_read_and_checked() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  expect(!secure_frontend::read_engine_build(folder).has_value(), "no build.json: nothing");
  secure_frontend_test::write_file(folder / "build.json", kRecord);
  const auto build = secure_frontend::read_engine_build(folder);
  expect(build.has_value(), "the record is read");
  if (!build) return;
  expect(build->built == "2026-10-27T23:00:00Z" && build->commit == "0123456789ab" && build->ggml == "3cf03257f219" &&
             build->cuda_arch == "native" && build->features == std::vector<std::string>{"email", "search"},
         "when, from which commit, the ggml pin, the GPU targets and the features");
  expect(build->engines.size() == 2U && build->engines[0].library == "multiengine_strata.dll" &&
             build->engines[1].models == "Raon-1-mini" && build->engines[1].bytes == 53477376U &&
             build->engines[0].source == "upstream Strata, engine 0.1.51",
         "each library with its models, its source and its size");
  for (const std::string bad : {std::string{"{\"schema_version\": 2, \"engines\": []}"}, std::string{"[1, 2]"}, std::string{"not json"},
                                std::string{"{\"schema_version\": 1, \"engines\": [{\"library\": 7}]}"},
                                std::string{"{\"schema_version\": 1, \"commit\": \"a\\u0007b\", \"engines\": []}"},
                                std::string(70000U, ' ') + "{}"}) {
    secure_frontend_test::write_file(folder / "build.json", bad);
    expect(!secure_frontend::read_engine_build(folder).has_value(), "refused: " + bad.substr(0, 60));
  }
}

void this_pcs_hardware_is_named() {
  const auto hardware = secure_frontend::hardware_facts();
  expect(!hardware.cpu.empty() && hardware.cpu.find("threads") != std::string::npos, "the CPU and its threads: " + hardware.cpu);
  expect(hardware.ram_bytes.has_value() && *hardware.ram_bytes > (512ULL << 20U), "the RAM");
#if defined(_WIN32)
  expect(hardware.os.rfind("Windows", 0) == 0U && hardware.os.find("build ") != std::string::npos, "Windows, its edition and build: " + hardware.os);
#else
  expect(!hardware.os.empty(), "the system and its kernel: " + hardware.os);
#endif
  for (const std::string* text : {&hardware.cpu, &hardware.os}) {
    for (const char c : *text) expect(c >= 0x20 && c < 0x7f, "printable ASCII only");
  }
}

void the_about_json_has_both() {
  const auto folder = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(folder / "build.json", kRecord);
  const secure_frontend::HardwareFacts hardware{.cpu = "Test CPU, 32 threads", .ram_bytes = 103079215104ULL,
                                                .gpu = "NVIDIA GeForce RTX 4080 SUPER, 16376 MiB", .os = "Windows 11 Pro 25H2 (build 26200.1)"};
  const std::string text = secure_frontend::render_about_json(secure_frontend::read_engine_build(folder), hardware);
  const auto parsed = secure_frontend::parse_json(text, secure_frontend::JsonLimits{.max_bytes = 1U << 16U, .max_depth = 6U,
                                                                                    .max_container_entries = 256U, .max_string_bytes = 4096U});
  expect(std::holds_alternative<secure_frontend::JsonValue>(parsed), "it is JSON: " + text);
  for (const char* part : {"\"commit\":\"0123456789ab\"", "\"library\":\"multiengine_raon1.dll\"", "\"cpu\":\"Test CPU, 32 threads\"",
                           "\"ram_bytes\":103079215104", "\"gpu\":\"NVIDIA GeForce RTX 4080 SUPER, 16376 MiB\"",
                           "\"os\":\"Windows 11 Pro 25H2 (build 26200.1)\""}) {
    expect(text.find(part) != std::string::npos, std::string{"it holds "} + part + ": " + text);
  }
  const std::string none = secure_frontend::render_about_json(std::nullopt, secure_frontend::HardwareFacts{});
  expect(none.find("\"build\":null") != std::string::npos && none.find("\"gpu\":null") != std::string::npos &&
             none.find("\"ram_bytes\":null") != std::string::npos,
         "without a record or a reading: null, not invented: " + none);
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_build_record_is_read_and_checked", the_build_record_is_read_and_checked},
      {"this_pcs_hardware_is_named", this_pcs_hardware_is_named},
      {"the_about_json_has_both", the_about_json_has_both},
  }, failures);
}
