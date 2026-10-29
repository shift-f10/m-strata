#include "mtp_layer.hpp"

#include "iq_pack.hpp"
#include "pack_common.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

namespace strata_prepare {
namespace fs = std::filesystem;

namespace {

// tools/mtp_fetch.py at PINNED_REVISION: mtp-inventory.json's shard and byte range of each tensor, SHA256's hash
constexpr std::array<MtpTensor, 31> kTensors{{
    {"mtp.fc_embedding.weight", "model-00120-of-00131.safetensors", 1935412640ULL, 13107200ULL, {2560, 2560, 0}, 2, "241d6e87ded317a61b1aeb3ca4e28c53f1caba09ad5be66092e91fe5e3e14751"},
    {"mtp.fc_hidden.weight", "model-00124-of-00131.safetensors", 1716599928ULL, 13107200ULL, {2560, 2560, 0}, 2, "97c8e427ad0177e2c713ed24a397d815b523eda15ef453f1ae6463f6c70a8f96"},
    {"mtp.hyper_connection_mixer.hc_norm.weight", "model-00037-of-00131.safetensors", 1742326680ULL, 20480ULL, {10240, 0, 0}, 1, "ce948a398e15aeaa403c424cda34e6eed296ab2570d591bb5ff3da642b255d64"},
    {"mtp.hyper_connection_mixer.input_mix_weight_down.weight", "model-00041-of-00131.safetensors", 1935412688ULL, 6553600ULL, {320, 10240, 0}, 2, "c633d2841fda463e82045e0b93fb8cb8d511ec0c9eda616020f01aca18d71947"},
    {"mtp.hyper_connection_mixer.input_mix_weight_up.weight", "model-00043-of-00131.safetensors", 1832518416ULL, 6553600ULL, {10240, 320, 0}, 2, "6565285f0ba2923703252763cfc11d8a9bda40ab42e7548e12464b04dbdbec13"},
    {"mtp.layers.0.attn_hyper_connection.block_inject_weight.weight", "model-00047-of-00131.safetensors", 1716599984ULL, 81920ULL, {4, 10240, 0}, 2, "c8afff64e7f54412d02120097c4255033c55245e1c1eed84cc630d428ccb864c"},
    {"mtp.layers.0.attn_hyper_connection.hc_norm.weight", "model-00049-of-00131.safetensors", 1935412672ULL, 20480ULL, {10240, 0, 0}, 1, "1fb619aeaaf637ccd70f46f1f8f8116e6a46448cb43026b246de8ee8286ed809"},
    {"mtp.layers.0.attn_hyper_connection.input_mix_weight_down.weight", "model-00053-of-00131.safetensors", 1832518424ULL, 6553600ULL, {320, 10240, 0}, 2, "e80ac8bf3e270e9d5a949ec7c913329dc8de017d69851334820fae7b8c86a7f5"},
    {"mtp.layers.0.attn_hyper_connection.input_mix_weight_up.weight", "model-00057-of-00131.safetensors", 1935412680ULL, 6553600ULL, {10240, 320, 0}, 2, "f80f1fd44e1183e7764ea496b11ff89ead9dd758f861d4e79a770bf54956b1a5"},
    {"mtp.layers.0.mlp.experts.down_proj", "model-00062-of-00131.safetensors", 1832518400ULL, 1677721600ULL, {512, 2560, 640}, 3, "539c46234f73bac70450f731f40e715f77ac32faf77def0f3f800e58aaabbbc0"},
    {"mtp.layers.0.mlp.experts.gate_up_proj", "model-00060-of-00131.safetensors", 115918328ULL, 3355443200ULL, {512, 1280, 2560}, 3, "839e98f9642ebc884b178abd2090b6dc8b0bcd81ed92d7f7a08314c6a68f96d5"},
    {"mtp.layers.0.mlp.gate.weight", "model-00066-of-00131.safetensors", 1716599952ULL, 2621440ULL, {512, 2560, 0}, 2, "f0d35fa41b360a08bc0c5b1e5766aa42df960291716ee189208b5d2599462c20"},
    {"mtp.layers.0.mlp.shared_expert.down_proj.weight", "model-00076-of-00131.safetensors", 1935412680ULL, 3276800ULL, {2560, 640, 0}, 2, "d2ced8c81367cc494599d1a8cdc279c3dd6579b628ded05acd7495e99cee01d8"},
    {"mtp.layers.0.mlp.shared_expert.gate_proj.weight", "model-00072-of-00131.safetensors", 1832518528ULL, 3276800ULL, {640, 2560, 0}, 2, "f4a81109165853b3b64f36d809f98094b599763364070f5fa079200e1c386d4c"},
    {"mtp.layers.0.mlp.shared_expert.up_proj.weight", "model-00072-of-00131.safetensors", 1835795328ULL, 3276800ULL, {640, 2560, 0}, 2, "172586861eea53efbff10afc43ded5b6e49c83879e9efc01e089c652bbb0a156"},
    {"mtp.layers.0.mlp.shared_expert_gate.weight", "model-00070-of-00131.safetensors", 1832518400ULL, 5120ULL, {1, 2560, 0}, 2, "acdd306d9996d356e4667a16ff63af6fef086fc7f570f1bc00bbe882a2a93f6e"},
    {"mtp.layers.0.mlp_hyper_connection.block_inject_weight.weight", "model-00078-of-00131.safetensors", 1832518424ULL, 81920ULL, {4, 10240, 0}, 2, "c5782dd92b479a1f8ef43d683fe9c917fd9e8fba81f23c1aae8817bfe4b476a6"},
    {"mtp.layers.0.mlp_hyper_connection.hc_norm.weight", "model-00082-of-00131.safetensors", 1935412656ULL, 20480ULL, {10240, 0, 0}, 1, "21a504876b5e7b788301557c68dfb757999430eba36c66ec8ab96a63604dbfd1"},
    {"mtp.layers.0.mlp_hyper_connection.input_mix_weight_down.weight", "model-00086-of-00131.safetensors", 1935412696ULL, 6553600ULL, {320, 10240, 0}, 2, "b71240036dea954e034d5ae04e097bcd7ac51cb28a44e4c93d198af3447278a1"},
    {"mtp.layers.0.mlp_hyper_connection.input_mix_weight_up.weight", "model-00088-of-00131.safetensors", 1832518424ULL, 6553600ULL, {10240, 320, 0}, 2, "40756866bf2a5e82c28e407f717b6fa07614d593f78e79c7ceb2cd4ffe6dda94"},
    {"mtp.layers.0.self_attn.indexer.index_qk_proj.weight", "model-00090-of-00131.safetensors", 1832518416ULL, 3276800ULL, {640, 2560, 0}, 2, "2befafae6e944c1fc651fd73d4d527eb6f138a6c1679065b003128ee3150e859"},
    {"mtp.layers.0.self_attn.indexer.k_layernorm.weight", "model-00094-of-00131.safetensors", 1935412672ULL, 256ULL, {128, 0, 0}, 1, "2db621f6a08b4085756fc69d60a4a1bd1d7fbf5d406456d2477075842efaae54"},
    {"mtp.layers.0.self_attn.indexer.q_layernorm.weight", "model-00100-of-00131.safetensors", 1716599968ULL, 256ULL, {128, 0, 0}, 1, "afaf59f68e251939970ffdeacad0667dffc657a522bea62cdc44a52816fc49d6"},
    {"mtp.layers.0.self_attn.k_norm.weight", "model-00102-of-00131.safetensors", 1935412648ULL, 512ULL, {256, 0, 0}, 1, "8f924b0357cf936e14fc34518205d000e1423aa60626827df81b0804fae8cdfd"},
    {"mtp.layers.0.self_attn.k_proj.weight", "model-00108-of-00131.safetensors", 1832518624ULL, 2621440ULL, {512, 2560, 0}, 2, "1a295dc2d6e633e808aeb19e00c97856a1817bbfcc83f5b610e2bfcacfcb8c38"},
    {"mtp.layers.0.self_attn.o_proj.weight", "model-00106-of-00131.safetensors", 1832518400ULL, 31457280ULL, {2560, 6144, 0}, 2, "9d2fcf0ac6ac19de87f638f710cfd1af6ad14141e75b7d3d5d29fbd06f374219"},
    {"mtp.layers.0.self_attn.q_norm.weight", "model-00112-of-00131.safetensors", 1935412664ULL, 512ULL, {256, 0, 0}, 1, "57957aa1e7c5a3ac628bff0ec5522ad55a39b8c0b8bd2d324ae640e3771617ef"},
    {"mtp.layers.0.self_attn.q_proj.weight", "model-00108-of-00131.safetensors", 1835140064ULL, 62914560ULL, {12288, 2560, 0}, 2, "5220aef3d288f40759f7356af9ef453e5ac0256a38749a9bb803a4e9db58150d"},
    {"mtp.layers.0.self_attn.v_proj.weight", "model-00108-of-00131.safetensors", 1898054624ULL, 2621440ULL, {512, 2560, 0}, 2, "39cb12f5424e965fa71fb81db0ee230c701b7b1e0b72c36641fde7e0e41bff41"},
    {"mtp.pre_fc_norm_embedding.weight", "model-00114-of-00131.safetensors", 1832518392ULL, 5120ULL, {2560, 0, 0}, 1, "04c4a570850e06f2d8913da8220d54d4c7f87db6eb6d45480b938e8ba41d6a86"},
    {"mtp.pre_fc_norm_hidden.weight", "model-00118-of-00131.safetensors", 1716599944ULL, 20480ULL, {10240, 0, 0}, 1, "b933b44559879449b1baf2af8385c5c237347307e3fd452394870cb53e6eab6d"},
}};

constexpr std::string_view kGateUp = "mtp.layers.0.mlp.experts.gate_up_proj";
constexpr std::string_view kDown = "mtp.layers.0.mlp.experts.down_proj";
// mtp_rt.py's Q8: the large projections, Q8_0 so the engine's multi-column MMVQ runs them (by their short names)
constexpr std::array<std::string_view, 10> kQ8{"fc_embedding.weight", "fc_hidden.weight", "self_attn.q_proj.weight",
                                               "self_attn.k_proj.weight", "self_attn.v_proj.weight", "self_attn.o_proj.weight",
                                               "self_attn.indexer.index_qk_proj.weight", "mlp.shared_expert.gate_proj.weight",
                                               "mlp.shared_expert.up_proj.weight", "mlp.shared_expert.down_proj.weight"};

struct Failure {
  std::string reason;
};

// numpy's float32 add.reduce over 64 contiguous values (its pairwise sum: eight accumulators for n <= 128, combined as
// ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7))), so a near tie between two scales goes the way mtp_pack.py's does
float pairwise_sum64(const float* a) noexcept {
  float r[8];
  for (int j = 0; j < 8; ++j) r[j] = a[j];
  for (int i = 8; i < 64; i += 8) {
    for (int j = 0; j < 8; ++j) r[j] += a[i + j];
  }
  return ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
}

// np.rint for |v| < 2^22 (round half to even in the default rounding mode): the codes here are at most 2
float rint_small(const float v) noexcept {
  constexpr float kMagic = 12582912.0F;   // 1.5 x 2^23
  return (v + kMagic) - kMagic;
}

std::uint64_t element_count(const MtpTensor& t) noexcept {
  std::uint64_t n = 1;
  for (int d = 0; d < t.dims; ++d) n *= static_cast<std::uint64_t>(t.shape[static_cast<std::size_t>(d)]);
  return n;
}

fs::path tensor_file(const fs::path& src, const std::string_view name) { return src / "tensors" / path_from_utf8(std::string{name} + ".bin"); }

std::string sha256_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  Sha256 h;
  std::vector<char> buffer(1U << 24U);
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = in.gcount();
    if (got > 0) h.update(buffer.data(), static_cast<std::size_t>(got));
  }
  return h.hex();
}

// `count` BF16 values of `file` from value `first`, as float32
std::vector<float> read_bf16(std::ifstream& in, const std::uint64_t first, const std::size_t count) {
  std::vector<std::uint16_t> raw(count);
  in.seekg(static_cast<std::streamoff>(first * 2U));
  in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(count * 2U));
  if (!in) throw Failure{"wrong_size"};
  std::vector<float> out(count);
  for (std::size_t i = 0; i < count; ++i) out[i] = f32_from_bf16(raw[i]);
  return out;
}

std::vector<unsigned char> q2_0_rows(const std::vector<float>& x) {
  std::vector<unsigned char> out(x.size() / 64U * 18U);
  for (std::size_t b = 0; b < x.size() / 64U; ++b) q2_0_block(x.data() + b * 64U, out.data() + b * 18U);
  return out;
}

void write_all(const fs::path& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) throw Failure{"write_failed"};
}

void publish(const fs::path& tmp, const fs::path& to) {
  std::error_code ec;
  fs::rename(tmp, to, ec);
  if (ec) throw Failure{"write_failed"};
}

}  // namespace

std::span<const MtpTensor> mtp_tensors() noexcept { return kTensors; }

void q2_0_block(const float* x, unsigned char* out) noexcept {
  float amax = 0.0F;
  for (int i = 0; i < 64; ++i) amax = std::max(amax, std::fabs(x[i]));
  float best_err = std::numeric_limits<float>::infinity();
  float best_d = 0.0F;
  float e[64];
  // candidate scales amax/2 (the +2 level reaches the max) ... amax (ggml's reference): np.linspace(0.5, 1.0, 17)
  for (int s = 0; s < 17; ++s) {
    const float f = static_cast<float>(0.5 + 0.03125 * s);
    const float d = amax * f;
    const float inv = d > 0.0F ? 1.0F / d : 0.0F;
    for (int i = 0; i < 64; ++i) {
      const float q = std::clamp(rint_small(x[i] * inv), -1.0F, 2.0F);
      const float diff = q * d - x[i];
      e[i] = diff * diff;
    }
    const float err = pairwise_sum64(e);
    if (err < best_err) {
      best_err = err;
      best_d = d;
    }
  }
  bool overflow = false;
  const unsigned short d16 = f16_bits(best_d, overflow);
  const float d = f32_from_f16(d16);   // quantized against the STORED scale
  const float inv = d > 0.0F ? 1.0F / d : 0.0F;
  unsigned char codes[64];
  for (int i = 0; i < 64; ++i) codes[i] = static_cast<unsigned char>(std::clamp(rint_small(x[i] * inv), -1.0F, 2.0F) + 1.0F);
  out[0] = static_cast<unsigned char>(d16 & 0xffU);
  out[1] = static_cast<unsigned char>(d16 >> 8U);
  for (int j = 0; j < 16; ++j) {
    out[2 + j] = static_cast<unsigned char>(codes[4 * j] | (codes[4 * j + 1] << 2U) | (codes[4 * j + 2] << 4U) | (codes[4 * j + 3] << 6U));
  }
}

void q8_0_block(const float* x, unsigned char* out) noexcept {
  float amax = 0.0F;
  for (int i = 0; i < 32; ++i) amax = std::max(amax, std::fabs(x[i]));
  const float d = amax / 127.0F;
  const float inv = d > 0.0F ? 1.0F / d : 0.0F;
  bool overflow = false;
  const unsigned short d16 = f16_bits(d, overflow);
  out[0] = static_cast<unsigned char>(d16 & 0xffU);
  out[1] = static_cast<unsigned char>(d16 >> 8U);
  for (int i = 0; i < 32; ++i) {
    const float v = x[i] * inv;
    const float magnitude = std::floor(std::fabs(v) + 0.5F);   // np.floor(np.abs(v) + 0.5) in float32
    const float q = v > 0.0F ? magnitude : v < 0.0F ? -magnitude : 0.0F;
    out[2 + i] = static_cast<unsigned char>(static_cast<signed char>(q));
  }
}

std::string expert_blob(const unsigned char* gate_up, const unsigned char* down, const std::int64_t h, const std::int64_t ff) {
  const auto gu_blocks = static_cast<std::size_t>(h / 64);   // per gate_up row
  const auto dn_blocks = static_cast<std::size_t>(ff / 64);  // per down row
  const auto rows = static_cast<std::size_t>(2 * ff);
  const auto down_rows = static_cast<std::size_t>(h);
  // inter row r: gate row r / 2 (even r), up row ff + r / 2 (odd r)
  const auto inter = [&](const std::size_t r) {
    const std::size_t source = r % 2U == 0U ? r / 2U : static_cast<std::size_t>(ff) + r / 2U;
    return gate_up + source * gu_blocks * 18U;
  };
  std::string out;
  out.reserve((rows * gu_blocks + down_rows * dn_blocks) * 18U);
  for (const bool scales : {false, true}) {
    const std::size_t from = scales ? 0U : 2U;
    const std::size_t size = scales ? 2U : 16U;
    for (std::size_t r = 0; r < rows; ++r) {
      for (std::size_t b = 0; b < gu_blocks; ++b) out.append(reinterpret_cast<const char*>(inter(r) + b * 18U + from), size);
    }
    for (std::size_t r = 0; r < down_rows; ++r) {
      for (std::size_t b = 0; b < dn_blocks; ++b) out.append(reinterpret_cast<const char*>(down + (r * dn_blocks + b) * 18U + from), size);
    }
  }
  return out;
}

std::string mtp_runtime(const MtpOptions& options, const std::span<const MtpTensor> tensors, std::ostream& steps, std::ostream& log) {
  try {
    // verify: every tensor fetched, of its size, and the checkpoint's own bytes (#327: a mirror that ignored a Range
    // request sent the shard's header instead)
    steps << "STEP verify\n" << std::flush;
    const MtpTensor* gate_up = nullptr;
    const MtpTensor* down = nullptr;
    for (const auto& t : tensors) {
      const fs::path file = tensor_file(options.src, t.name);
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) throw Failure{"missing_tensor"};
      const auto size = fs::file_size(file, ec);
      if (ec || size != t.bytes) throw Failure{"wrong_size"};
      if (element_count(t) * 2U != t.bytes) throw Failure{"bad_shape"};
      if (!t.sha256.empty() && sha256_file(file) != t.sha256) throw Failure{"wrong_sha256"};
      if (t.name == kGateUp) gate_up = &t;
      if (t.name == kDown) down = &t;
    }
    // the experts: gate_up [ne, 2 x ff, h], down [ne, h, ff], rows of whole Q2_0 blocks
    if (gate_up == nullptr || down == nullptr || gate_up->dims != 3 || down->dims != 3) throw Failure{"bad_shape"};
    const std::int64_t ne = gate_up->shape[0];
    const std::int64_t h = gate_up->shape[2];
    const std::int64_t ff = down->shape[2];
    if (down->shape[0] != ne || gate_up->shape[1] != 2 * ff || down->shape[1] != h || h % 64 != 0 || ff % 64 != 0 || ne <= 0) {
      throw Failure{"bad_shape"};
    }
    std::error_code ec;
    if (options.draft_vocab && !fs::is_regular_file(*options.draft_vocab, ec)) throw Failure{"missing_vocab"};

    fs::create_directories(options.out, ec);
    if (ec) throw Failure{"write_failed"};
    // the vocabulary goes first and comes back last: until then the folder is not a whole draft layer
    fs::remove(options.out / "draft_vocab.bin", ec);

    steps << "STEP experts\n" << std::flush;
    const unsigned threads = std::max(1U, options.threads != 0U ? options.threads : std::thread::hardware_concurrency());
    const fs::path experts_tmp = options.out / "experts.bin.tmp";
    {
      std::ofstream out(experts_tmp, std::ios::binary | std::ios::trunc);
      if (!out) throw Failure{"write_failed"};
      const auto gu_values = static_cast<std::uint64_t>(2 * ff * h);
      const auto dn_values = static_cast<std::uint64_t>(h * ff);
      for (std::int64_t base = 0; base < ne; base += threads) {
        const auto batch = static_cast<std::size_t>(std::min<std::int64_t>(threads, ne - base));
        std::vector<std::string> blobs(batch);
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        for (std::size_t k = 0; k < batch; ++k) {
          workers.emplace_back([&, k] {
            try {
              const auto e = static_cast<std::uint64_t>(base) + k;
              std::ifstream gu_in(tensor_file(options.src, kGateUp), std::ios::binary);
              std::ifstream dn_in(tensor_file(options.src, kDown), std::ios::binary);
              const auto gu = q2_0_rows(read_bf16(gu_in, e * gu_values, static_cast<std::size_t>(gu_values)));
              const auto dn = q2_0_rows(read_bf16(dn_in, e * dn_values, static_cast<std::size_t>(dn_values)));
              blobs[k] = expert_blob(gu.data(), dn.data(), h, ff);
            } catch (...) {
              failed = true;
            }
          });
        }
        for (auto& w : workers) w.join();
        if (failed) throw Failure{"wrong_size"};
        for (const auto& blob : blobs) out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        if (!out) throw Failure{"write_failed"};
      }
    }
    publish(experts_tmp, options.out / "experts.bin");

    // dense.bin: every other tensor in name order (the packed GGUF's order), each at a 256-byte boundary - the norms
    // (1-D) as F32 + 1 (vLLM's GemmaRMSNorm scales by 1 + w), the large projections Q8_0, the rest BF16 as fetched
    steps << "STEP dense\n" << std::flush;
    std::vector<const MtpTensor*> dense;
    for (const auto& t : tensors) {
      if (t.name.find("experts.") == std::string_view::npos) dense.push_back(&t);
    }
    std::sort(dense.begin(), dense.end(), [](const MtpTensor* a, const MtpTensor* b) { return a->name < b->name; });
    std::string bin;
    std::string txt;
    for (const MtpTensor* t : dense) {
      std::string_view short_name = t->name.substr(t->name.rfind("mtp.", 0) == 0 ? 4U : 0U);
      if (short_name.rfind("layers.0.", 0) == 0) short_name.remove_prefix(9U);
      std::ifstream in(tensor_file(options.src, t->name), std::ios::binary);
      const auto values = read_bf16(in, 0, static_cast<std::size_t>(element_count(*t)));
      std::string raw;
      std::string kind;
      std::uint64_t rows = 0;
      std::uint64_t cols = 0;
      if (t->dims == 1) {
        raw.resize(values.size() * 4U);
        for (std::size_t i = 0; i < values.size(); ++i) {
          const float v = values[i] + 1.0F;
          std::memcpy(raw.data() + i * 4U, &v, 4U);
        }
        kind = "f32";
        rows = 1;
        cols = values.size();
      } else {
        rows = static_cast<std::uint64_t>(t->shape[0]);
        cols = values.size() / rows;
        if (std::find(kQ8.begin(), kQ8.end(), short_name) != kQ8.end()) {
          if (values.size() % 32U != 0U) throw Failure{"bad_shape"};
          raw.resize(values.size() / 32U * 34U);
          for (std::size_t b = 0; b < values.size() / 32U; ++b) {
            q8_0_block(values.data() + b * 32U, reinterpret_cast<unsigned char*>(raw.data()) + b * 34U);
          }
          kind = "q8_0";
        } else {
          raw = read_file_text(tensor_file(options.src, t->name));   // the fetched BF16 bytes as they are
          kind = "bf16";
        }
      }
      bin.append((256U - bin.size() % 256U) % 256U, '\0');
      txt += std::string{short_name} + " " + kind + " " + std::to_string(rows) + " " + std::to_string(cols) + " " +
             std::to_string(bin.size()) + " " + std::to_string(raw.size()) + "\n";
      bin += raw;
    }
    write_all(options.out / "dense.bin.tmp", bin);
    publish(options.out / "dense.bin.tmp", options.out / "dense.bin");
    write_all(options.out / "dense.txt.tmp", text_bytes(txt));   // Path.write_text: the platform's line ends
    publish(options.out / "dense.txt.tmp", options.out / "dense.txt");

    if (options.draft_vocab) {
      steps << "STEP vocab\n" << std::flush;
      write_all(options.out / "draft_vocab.bin.tmp", read_file_text(*options.draft_vocab));
      publish(options.out / "draft_vocab.bin.tmp", options.out / "draft_vocab.bin");
    }
    log << "experts.bin " << ne * static_cast<std::int64_t>(3 * h * ff / 64 * 18) << " B, dense.bin " << bin.size() << " B, "
        << dense.size() << " tensors -> " << utf8(options.out) << "\n";
    return {};
  } catch (const Failure& failure) {
    return failure.reason;
  } catch (const std::exception&) {
    return "write_failed";
  }
}

}  // namespace strata_prepare
