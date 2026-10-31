// Upstream's low-RAM mode (low_ram.hpp; owner 2026-10-31, "Yes, build it"): upstream setup.py's rule for when a PC's
// RAM cannot hold the model's experts with 10 GB beside them, its two variants, and the pack's expert size it reads.
#include "secure_frontend/low_ram.hpp"
#include "support/test_support.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::LowRamChoice;
using secure_frontend::LowRamFacts;
using secure_frontend::LowRamMode;

constexpr std::uint64_t kGiB = 1ULL << 30U;
constexpr std::uint64_t kIq2XsExperts = 35454976000ULL;   // the VM pack's native_experts.txt total

bool close_to(const double a, const double b) { return std::fabs(a - b) < 1e-3; }
std::string mode_text(const LowRamMode mode) { return std::string{secure_frontend::low_ram_value(mode)}; }
std::string shown(const LowRamChoice& c) {
  return mode_text(c.mode) + " share " + std::to_string(c.gpu_share) + " rest " + std::to_string(c.rest_gb) + " arena " +
         std::to_string(c.arena_gb) + " ram " + std::to_string(c.ram_gib);
}

// The owner's Azure VM (2026-10-31 calibration report): 27.4 GiB of RAM, a Tesla T4 of 16 GiB, Qwen3.8 IQ2_XS's 35.45 GB
// of experts, 16K context, q4_0, the draft layer on. Upstream: 27.4 < 35.45 + 10, so the low-RAM mode; the T4 holds
// 16 - 5 - 1 = 10 GB of them, the other 25.45 GB do not fit 27.4 - 10: mapped, not resident.
LowRamFacts the_vm() {
  LowRamFacts f;
  f.ram_bytes = static_cast<std::uint64_t>(27.4 * static_cast<double>(kGiB));
  f.vram_bytes = {16ULL * kGiB};
  f.expert_bytes = kIq2XsExperts;
  f.context = 16384;
  f.kv = "q4_0";
  f.mtp = true;
  f.expert_profile = true;
  return f;
}

void a_pc_short_of_ram_maps_its_experts() {
  const auto choice = secure_frontend::choose_low_ram(the_vm(), "auto");
  expect(choice.mode == LowRamMode::mmap, "the VM: the low-RAM mode, mapped: " + shown(choice));
  expect(close_to(choice.arena_gb, 35.454976) && close_to(choice.ram_gib, 27.4), "the arena in GB, the RAM in GiB (upstream's units): " + shown(choice));
  expect(close_to(choice.rest_gb, 25.454976) && close_to(choice.gpu_share, 10.0 / 35.454976), "the T4 holds 10 GB of them: " + shown(choice));
}

// 32 GiB with an RTX 4090 (24 GiB): the GPU holds 18 GB, the other 17.45 GB fit 32 - 10 - resident; without a static
// expert profile the engine cannot keep that copy (generate.cpp: --resident-cpu-experts needs one), so mapped.
void the_rest_in_ram_is_the_resident_variant() {
  LowRamFacts f = the_vm();
  f.ram_bytes = 32ULL * kGiB;
  f.vram_bytes = {24ULL * kGiB};
  const auto resident = secure_frontend::choose_low_ram(f, "auto");
  expect(resident.mode == LowRamMode::resident && close_to(resident.rest_gb, 17.454976), "32 GiB and 24 GiB: resident: " + shown(resident));
  f.expert_profile = false;
  expect(secure_frontend::choose_low_ram(f, "auto").mode == LowRamMode::mmap, "with no expert profile: mapped");
  f.expert_profile = true;
  f.ram_bytes = 64ULL * kGiB;
  expect(secure_frontend::choose_low_ram(f, "auto").mode == LowRamMode::off, "64 GiB holds them with 10 beside: no low-RAM mode");
}

// What takes the GPU's room: the draft layer's 1 GB, the picture encoder's 700 MiB on the main GPU, and the KV cache of
// a context past 32K (13 layers x the cell bytes: q4_0 576, k8v4 816, any other 1056). Never below none.
void the_gpus_share_follows_upstream() {
  LowRamFacts f = the_vm();
  f.mtp = false;
  expect(close_to(secure_frontend::choose_low_ram(f, "auto").rest_gb, 35.454976 - 11.0), "no draft layer: 11 GB on the T4");
  f.mtp = true;
  f.vision_gpu = true;
  expect(close_to(secure_frontend::choose_low_ram(f, "auto").rest_gb, 35.454976 - (16.0 - 700.0 / 1024.0 - 6.0)),
         "the picture encoder's 700 MiB");
  f.vision_gpu = false;
  f.context = 131072;
  expect(close_to(secure_frontend::choose_low_ram(f, "auto").rest_gb, 35.454976 - (10.0 - 98304.0 * 13.0 * 576.0 / 1e9)),
         "128K at q4_0: its KV past 32K");
  f.kv = "int8";
  f.ram_bytes = 32ULL * kGiB;
  f.vram_bytes = {24ULL * kGiB};
  f.context = 1048576;
  const auto long_context = secure_frontend::choose_low_ram(f, "auto");
  expect(close_to(long_context.rest_gb, 35.454976 - (18.0 - 1015808.0 * 13.0 * 1056.0 / 1e9)) && long_context.mode == LowRamMode::mmap,
         "1M at int8 leaves the 4090 4 GB: the rest no longer fits the RAM, mapped: " + shown(long_context));
  f.vram_bytes = {4ULL * kGiB};
  f.context = 16384;
  const auto four_gib = secure_frontend::choose_low_ram(f, "auto");
  expect(four_gib.gpu_share == 0.0 && close_to(four_gib.rest_gb, 35.454976), "a 4 GiB GPU holds none, never less: " + shown(four_gib));
  f.vram_bytes = {};
  expect(secure_frontend::choose_low_ram(f, "auto").gpu_share == 0.0, "no GPU known: none");
}

// Several GPUs (upstream since the engine's resident split): each holds its share, the rest by the same rule.
void several_gpus_hold_more() {
  LowRamFacts f = the_vm();
  f.ram_bytes = 32ULL * kGiB;
  expect(secure_frontend::choose_low_ram(f, "auto").mode == LowRamMode::mmap, "one T4 on 32 GiB: 25.45 GB left, mapped");
  f.vram_bytes = {16ULL * kGiB, 16ULL * kGiB};
  const auto two = secure_frontend::choose_low_ram(f, "auto");
  expect(two.mode == LowRamMode::resident && close_to(two.rest_gb, 35.454976 - 20.0), "two T4s: 20 GB on the GPUs, resident: " + shown(two));
  f.vision_gpu = true;
  expect(close_to(secure_frontend::choose_low_ram(f, "auto").rest_gb, 35.454976 - 20.0 + 700.0 / 1024.0),
         "the picture encoder only on the main one");
}

// The option's values: on is the mode with its variant by the rule; resident and mmap force the variant (resident still
// needs the expert profile); off never; anything else is off; an unknown expert size is off.
void the_choice_overrides_the_rule() {
  LowRamFacts big = the_vm();
  big.ram_bytes = 64ULL * kGiB;
  big.vram_bytes = {24ULL * kGiB};
  expect(secure_frontend::choose_low_ram(big, "on").mode == LowRamMode::resident, "on: the mode where it is not needed, resident here");
  expect(secure_frontend::choose_low_ram(big, "mmap").mode == LowRamMode::mmap, "mmap");
  expect(secure_frontend::choose_low_ram(the_vm(), "resident").mode == LowRamMode::resident, "resident on the VM, as asked");
  LowRamFacts no_profile = the_vm();
  no_profile.expert_profile = false;
  expect(secure_frontend::choose_low_ram(no_profile, "resident").mode == LowRamMode::mmap, "resident without a profile: mapped");
  expect(secure_frontend::choose_low_ram(the_vm(), "off").mode == LowRamMode::off, "off: never");
  expect(secure_frontend::choose_low_ram(the_vm(), "sometimes").mode == LowRamMode::off, "a value that is no choice: off");
  LowRamFacts unknown = the_vm();
  unknown.expert_bytes = 0;
  expect(secure_frontend::choose_low_ram(unknown, "auto").mode == LowRamMode::off, "the experts' size not known: off");
  expect(secure_frontend::low_ram_flag(LowRamMode::mmap) == "--mmap-experts" &&
             secure_frontend::low_ram_flag(LowRamMode::resident) == "--resident-experts" && secure_frontend::low_ram_flag(LowRamMode::off).empty(),
         "the engine's flags");
}

// The pack's experts: native_experts.txt's header total (CRLF too), else experts.bin's size; none of them, none.
void the_pack_says_how_large_its_experts_are() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto native = root / "native";
  secure_frontend_test::make_private_directory(native);
  secure_frontend_test::write_file(native / "native_experts.txt",
                                   "# strata native experts v3: layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard] "
                                   "(n_expert 512, total 35454976000; absolute offsets in Q-00001-of-00002.gguf, or in the named shard beside it)\r\n"
                                   "0 22 42 0 1510400 957139232 1229167392 720288032\r\n");
  const auto got = secure_frontend::pack_expert_bytes(native);
  expect(got == std::optional<std::uint64_t>{kIq2XsExperts}, "native_experts.txt's total: " + (got ? std::to_string(*got) : std::string{"none"}));
  const auto canonical = root / "canonical";
  secure_frontend_test::make_private_directory(canonical);
  secure_frontend_test::write_file(canonical / "experts.bin", std::string(4096, 'x'));
  expect(secure_frontend::pack_expert_bytes(canonical) == std::optional<std::uint64_t>{4096U}, "experts.bin's size");
  secure_frontend_test::write_file(native / "experts.bin", std::string(10, 'x'));
  expect(secure_frontend::pack_expert_bytes(native) == std::optional<std::uint64_t>{kIq2XsExperts}, "the header's total before experts.bin");
  const auto broken = root / "broken";
  secure_frontend_test::make_private_directory(broken);
  secure_frontend_test::write_file(broken / "native_experts.txt", "# strata native experts v3: (n_expert 512, total lots; ...)\n");
  expect(!secure_frontend::pack_expert_bytes(broken).has_value(), "a total that is no number: none");
  expect(!secure_frontend::pack_expert_bytes(root / "missing").has_value(), "no pack: none");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_pc_short_of_ram_maps_its_experts", a_pc_short_of_ram_maps_its_experts},
      {"the_rest_in_ram_is_the_resident_variant", the_rest_in_ram_is_the_resident_variant},
      {"the_gpus_share_follows_upstream", the_gpus_share_follows_upstream},
      {"several_gpus_hold_more", several_gpus_hold_more},
      {"the_choice_overrides_the_rule", the_choice_overrides_the_rule},
      {"the_pack_says_how_large_its_experts_are", the_pack_says_how_large_its_experts_are},
  }, failures);
}
