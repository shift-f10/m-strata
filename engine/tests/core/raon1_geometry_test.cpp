// tests/core/raon1_geometry_test.cpp - Raon1Geometry::from_gguf on a synthetic raon1 GGUF: no model, no GPU.
//
// The key set + values are the ones the shipped Raon-1-mini-preview Q8_0 GGUF carries (read from the file, 731
// tensors): the hybrid head_count_kv pattern, the Gated-MLA latent dims with decoupled RoPE, the RDA/SSM dims,
// the 128/6+1 sigmoid MoE, the `situ` activation, and the 4-stream hyper-connections.  Asserts the reader maps
// each raon1.* key to the right field and the guards accept the real geometry.  Self-contained: a tiny GGUF
// writer (0 tensors) lives here so the test links against strata_glm's gguf_reader only.
#include "strata/artifact/gguf_reader.hpp"
#include "strata/core/raon1_geometry.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using strata::core::Raon1Geometry;

namespace {

// ---- a minimal GGUF v3 writer (metadata only; mirrors the on-disk layout strata::GgufFile reads) ------------
struct Kv {
    std::string key;
    uint32_t type = 8;                 // 4 u32, 6 f32, 8 string, 9 array
    uint64_t u = 0;
    float f = 0;
    std::string s;
    std::vector<uint64_t> arr;         // array of u32
};
Kv str(const std::string& k, const std::string& v) { Kv r; r.key = k; r.type = 8; r.s = v; return r; }
Kv u32(const std::string& k, uint64_t v) { Kv r; r.key = k; r.type = 4; r.u = v; return r; }
Kv f32(const std::string& k, float v) { Kv r; r.key = k; r.type = 6; r.f = v; return r; }
Kv u32s(const std::string& k, std::vector<uint64_t> v) { Kv r; r.key = k; r.type = 9; r.arr = std::move(v); return r; }

void write_gguf(const fs::path& path, const std::vector<Kv>& kv) {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*) p, (const uint8_t*) p + n); };
    auto put32 = [&](uint32_t v) { put(&v, 4); };
    auto put64 = [&](uint64_t v) { put(&v, 8); };
    auto puts = [&](const std::string& s) { put64(s.size()); put(s.data(), s.size()); };
    put32(0x46554747u);  // "GGUF"
    put32(3);            // version
    put64(0);            // tensor_count
    put64(kv.size());    // kv_count
    for (const auto& k : kv) {
        puts(k.key);
        put32(k.type);
        if (k.type == 8) puts(k.s);
        else if (k.type == 4) { const uint32_t v = (uint32_t) k.u; put(&v, 4); }
        else if (k.type == 6) put(&k.f, 4);
        else if (k.type == 9) { put32(4); put64(k.arr.size()); for (uint64_t x : k.arr) put32((uint32_t) x); }
    }
    b.resize((b.size() + 31) / 32 * 32, 0);  // align the (empty) tensor-data start to 32
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*) b.data(), (std::streamsize) b.size());
}

int g_fail = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %-78s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

std::vector<Kv> raon1_keys() {
    const std::string p = "raon1.";
    std::vector<uint64_t> hkv;  // [0,0,0,1] x6 then 1 -> 18 RDA + 7 MLA {3,7,11,15,19,23,24}
    for (int il = 0; il < 25; ++il) hkv.push_back((il % 4 == 3 || il == 24) ? 1 : 0);
    return {str("general.architecture", "raon1"),
            u32(p + "block_count", 25),
            u32(p + "context_length", 262144),
            u32(p + "embedding_length", 2560),
            u32(p + "attention.head_count", 40),
            u32(p + "attention.q_lora_rank", 576),
            u32(p + "attention.kv_lora_rank", 512),
            u32(p + "attention.key_length_mla", 192),
            u32(p + "rope.dimension_count", 64),
            u32(p + "attention.value_length_mla", 128),
            f32(p + "attention.latent_norm_rms_epsilon", 1e-6f),
            u32(p + "kda.head_dim", 128),
            u32(p + "ssm.conv_kernel", 4),
            u32(p + "ssm.time_step_rank", 128),
            f32(p + "kda.gate_lower_bound", -5.0f),
            u32(p + "hyper_connection.count", 4),
            f32(p + "hyper_connection.epsilon", 1e-5f),
            f32(p + "hyper_connection.magnitude", 2.0f),
            f32(p + "attention.layer_norm_rms_epsilon", 1e-5f),
            u32(p + "expert_count", 128),
            u32(p + "expert_used_count", 6),
            u32(p + "expert_feed_forward_length", 1088),
            u32(p + "expert_shared_count", 1),
            u32(p + "feed_forward_length", 12032),
            u32(p + "leading_dense_block_count", 1),
            f32(p + "expert_weights_scale", 1.0f),
            u32(p + "expert_weights_norm", 1),
            u32(p + "expert_gating_func", 2),
            f32(p + "activation.situ_beta", 4.0f),
            f32(p + "activation.situ_linear_beta", 25.0f),
            u32(p + "vocab_size", 259840),
            u32s(p + "attention.head_count_kv", hkv)};
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("raon1-geom-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path gguf = dir / "raon1.gguf";
    write_gguf(gguf, raon1_keys());

    Raon1Geometry g;
    std::string err;
    const bool ok = Raon1Geometry::from_gguf(strata::GgufFile(gguf.string()), g, err);
    check(ok, std::string("from_gguf accepts the real raon1 geometry") + (ok ? "" : " [" + err + "]"));

    check(g.n_embd == 2560, "n_embd = 2560");
    check(g.n_layers == 25, "n_layers = 25");
    check(g.n_head == 40, "n_head = 40");
    check(g.n_vocab == 259840, "n_vocab = 259840");
    check(g.n_ctx_train == 262144, "n_ctx_train = 262144");
    check(g.q_lora == 576, "q_lora = 576");
    check(g.kv_lora == 512, "kv_lora = 512");
    check(g.qk == 192 && g.qk_rope == 64 && g.qk_nope() == 128, "qk 192 = nope 128 + rope 64");
    check(g.v_head == 128, "v_head = 128 (== qk_nope)");
    check(g.kda_head_dim == 128 && g.d_conv == 4 && g.dt_rank == 128, "RDA: head_dim 128, d_conv 4, dt_rank 128");
    check(g.kda_lb == -5.0f, "kda_lb = -5");
    check(g.n_expert == 128 && g.n_exp_used == 6 && g.n_shared == 1, "MoE: 128 experts, 6 used, 1 shared");
    check(g.n_ff_exp == 1088 && g.n_ff_dense == 12032 && g.dense_lead == 1, "ff_exp 1088, ff_dense 12032, dense_lead 1");
    check(g.gating_func == 2 && g.norm_w == 1 && g.w_scale == 1.0f, "sigmoid gate, renorm, scale 1");
    check(g.situ_beta == 4.0f && g.situ_linear_beta == 25.0f, "situ beta 4 / linear_beta 25");
    check(g.hc == 4 && g.hc_magnitude == 2.0f, "hc 4, magnitude 2");
    check(g.mla_gate, "mla_gate on (Gated-MLA)");

    int rda = 0, mla = 0;
    bool split_ok = ok && (int) g.n_head_kv.size() == 25;
    for (int il = 0; il < 25 && split_ok; ++il) (g.is_recr(il) ? rda : mla)++;
    for (int il : {3, 7, 11, 15, 19, 23, 24}) split_ok = split_ok && !g.is_recr(il);
    check(split_ok && rda == 18 && mla == 7, "hybrid split: 18 RDA + 7 MLA at {3,7,11,15,19,23,24}");

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s: %d failure(s)\n", g_fail ? "RAON1-GEOM FAIL" : "raon1-geom OK", g_fail);
    return g_fail ? 1 : 0;
}
