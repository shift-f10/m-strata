// engines/unified/src/family/qwen4exp_mtp.cu - the qwen4exp MTP draft layer (speculative decode S3; spec
// docs/superpowers/specs/2026-10-24-qwen4exp-speculative-decode-design.md).
//
// Transcribed from upstream src/core/mtp.cpp (MtpDrafter::record_front / record_rest / draft) and the runtime
// files tools/mtp_rt.py writes (mtp/rt: dense.bin + dense.txt, experts.bin, draft_vocab.bin):
//
//   e = fc_embedding(rms(embed(tok)) * w)        h = fc_hidden per stream(rms_hc_dim(R_main) * w)
//   R = h + e (broadcast over the hc streams) -> attention HC read -> K/V into the layer's own cache, dense
//   attention over every cell -> R += attn write -> MLP HC read -> MoE (512 Q2_0 experts, top-10, shared expert)
//   -> R += MoE write -> final mixer -> head (the main output.weight's rows of the draft vocabulary).
//
// The F32 norm weights carry GemmaRMSNorm's +1 (mtp_rt.py applies it), so they multiply directly. Cell i pairs
// the main model's final hc residual at position i with the token at i+1, at rope position i; a draft step's
// own residual R feeds the next step. Drafts only: the verify window decides every emitted token, so nothing
// here can change the output -- only how many drafts the windows accept.
#include "family/qwen4exp_graph.hpp"

#include "kernels/common.cuh"
#include "kernels/qwen4exp_hc.cuh"
#include "kernels/qwen4exp_qsa.cuh"
#include "kernels/u_iq.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace u {

namespace {

constexpr float kMtpEps = 1e-6f;  // mtp.cpp EPS

// R[t][c][i] = h[t][c][i] + e[t][i]  (verify_kernels add_streams_broadcast)
__global__ void add_streams_broadcast_kernel(const float* __restrict__ h, const float* __restrict__ e,
                                             float* __restrict__ R, long long n, int hc, int T) {
  const long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n * hc * T) return;
  const long long t = i / (n * hc);
  R[i] = h[i] + e[t * n + i % n];
}

// dst row r = src row ids[r] (row_bytes each), byte-wise over the grid
__global__ void gather_rows_kernel(const std::uint8_t* __restrict__ src, long long row_bytes,
                                   const int* __restrict__ ids, long long n_rows, std::uint8_t* __restrict__ dst) {
  const long long total = row_bytes * n_rows;
  for (long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x; i < total;
       i += (long long)gridDim.x * blockDim.x) {
    const long long r = i / row_bytes, b = i - r * row_bytes;
    dst[i] = src[(long long)ids[r] * row_bytes + b];
  }
}

// the draft: token = dvocab[pick], probability = softmax(logits)[pick] (one block)
__global__ void draft_pick_kernel(const float* __restrict__ logits, int n, const int* __restrict__ pick,
                                  const int* __restrict__ dvocab, int* __restrict__ tok, float* __restrict__ prob) {
  __shared__ float red[32];
  const int p = *pick;
  const float mx = logits[p];
  float s = 0.0f;
  for (int i = threadIdx.x; i < n; i += blockDim.x) s += __expf(logits[i] - mx);
  for (int o = 16; o > 0; o >>= 1) s += __shfl_down_sync(0xffffffffu, s, o);
  if ((threadIdx.x & 31) == 0) red[threadIdx.x >> 5] = s;
  __syncthreads();
  if (threadIdx.x == 0) {
    float t = 0.0f;
    for (int w = 0; w < (int)(blockDim.x >> 5); ++w) t += red[w];
    *prob = 1.0f / t;
    *tok = dvocab[p];
  }
}

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  out.resize((std::size_t)n);
  return (bool)f.read(reinterpret_cast<char*>(out.data()), n);
}

}  // namespace

#define MTP_CU(call, err)                                                                    \
  do {                                                                                       \
    const cudaError_t e_ = (call);                                                           \
    if (e_ != cudaSuccess) {                                                                 \
      (err) = std::string("qwen4exp mtp: ") + cudaGetErrorString(e_) + " @ line " + std::to_string(__LINE__); \
      return false;                                                                          \
    }                                                                                        \
  } while (0)

bool Qwen4ExpGraph::mtp_load_(const std::string& dir, std::string& err) {
  const std::int64_t N = g_.n_embd, HC = g_.hc, HCD = HC * N, LR = g_.hc_lr;
  const std::int64_t NH = g_.n_head, HD = g_.head_dim, NKV = g_.n_head_kv, QD = NH * HD, KV = NKV * HD;
  // ---- dense.bin / dense.txt: name kind rows cols offset bytes
  std::vector<std::uint8_t> dense;
  std::ifstream txt(dir + "/dense.txt");
  if (!txt || !read_file(dir + "/dense.bin", dense)) {
    err = "qwen4exp mtp: cannot read " + dir + "/dense.txt or dense.bin";
    return false;
  }
  MTP_CU(cudaMalloc(&mtp_dense_, dense.size()), err);
  MTP_CU(cudaMemcpy(mtp_dense_, dense.data(), dense.size(), cudaMemcpyHostToDevice), err);
  std::string line;
  while (std::getline(txt, line)) {
    std::istringstream ls(line);
    std::string name, kind;
    long long rows = 0, cols = 0, off = 0, bytes = 0;
    if (!(ls >> name >> kind >> rows >> cols >> off >> bytes)) continue;
    if (off < 0 || off + bytes > (long long)dense.size()) {
      err = "qwen4exp mtp: dense.txt entry past dense.bin: " + name;
      return false;
    }
    WSlot w;
    const void* p = static_cast<const std::uint8_t*>(mtp_dense_) + off;
    w.n_out = rows;
    w.n_in = cols;
    if (kind == "f32") {
      w.type = 0;
      w.f32 = static_cast<const float*>(p);
    } else if (kind == "bf16") {
      w.type = (int)k::Quant::bf16;
      w.q = p;
    } else if (kind == "q8_0") {
      w.type = (int)k::Quant::q8_0;
      w.q = p;
    } else {
      err = "qwen4exp mtp: unknown tensor kind " + kind + " (" + name + ")";
      return false;
    }
    mtpw_[name] = w;
  }
  for (const char* need : {"fc_embedding.weight", "fc_hidden.weight", "pre_fc_norm_embedding.weight",
                           "pre_fc_norm_hidden.weight", "self_attn.q_proj.weight", "self_attn.k_proj.weight",
                           "self_attn.v_proj.weight", "self_attn.o_proj.weight", "self_attn.q_norm.weight",
                           "self_attn.k_norm.weight", "mlp.gate.weight", "mlp.shared_expert.gate_proj.weight",
                           "mlp.shared_expert.up_proj.weight", "mlp.shared_expert.down_proj.weight",
                           "mlp.shared_expert_gate.weight", "hyper_connection_mixer.hc_norm.weight"})
    if (mtpw_.count(need) == 0) {
      err = std::string("qwen4exp mtp: dense.txt lacks ") + need;
      return false;
    }
  mtp_ff_ = mtpw_.at("mlp.shared_expert.gate_proj.weight").n_out;  // 640: the experts' n_ff too
  mtp_nexp_ = mtpw_.at("mlp.gate.weight").n_out;                     // 512
  // ---- experts.bin: per expert upstream's CPU blob (gate/up rows interleaved, a codes plane then an fp16 scales
  // plane, mtp_rt.py blob_of) -> GGUF Q2_0 blocks (2-byte scale + 16 code bytes per 64 values), gate|up|down
  const std::int64_t gu_blocks = N / 64, dn_blocks = mtp_ff_ / 64;
  const std::int64_t row_gu = gu_blocks * 18, row_dn = dn_blocks * 18;
  const std::int64_t mat_gu = mtp_ff_ * row_gu, mat_dn = N * row_dn;
  mtp_blob_ = 2 * mat_gu + mat_dn;
  std::ifstream ex(dir + "/experts.bin", std::ios::binary);
  if (!ex) {
    err = "qwen4exp mtp: cannot read " + dir + "/experts.bin";
    return false;
  }
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_exp_), (std::size_t)(mtp_nexp_ * mtp_blob_)), err);
  {
    std::vector<std::uint8_t> blob((std::size_t)mtp_blob_), out((std::size_t)mtp_blob_);
    const std::int64_t gu_rows = 2 * mtp_ff_;
    const std::uint8_t* c_gu = blob.data();
    const std::uint8_t* c_dn = c_gu + gu_rows * gu_blocks * 16;
    const std::uint8_t* s_gu = c_dn + N * dn_blocks * 16;
    const std::uint8_t* s_dn = s_gu + gu_rows * gu_blocks * 2;
    for (std::int64_t e = 0; e < mtp_nexp_; ++e) {
      if (!ex.read(reinterpret_cast<char*>(blob.data()), mtp_blob_)) {
        err = "qwen4exp mtp: experts.bin is short";
        return false;
      }
      for (std::int64_t r = 0; r < mtp_ff_; ++r)       // gate row r = blob row 2r, up row r = blob row 2r+1
        for (int which = 0; which < 2; ++which) {
          const std::int64_t br = 2 * r + which;
          std::uint8_t* dst = out.data() + which * mat_gu + r * row_gu;
          for (std::int64_t b = 0; b < gu_blocks; ++b) {
            std::memcpy(dst + b * 18, s_gu + (br * gu_blocks + b) * 2, 2);
            std::memcpy(dst + b * 18 + 2, c_gu + (br * gu_blocks + b) * 16, 16);
          }
        }
      for (std::int64_t r = 0; r < N; ++r) {
        std::uint8_t* dst = out.data() + 2 * mat_gu + r * row_dn;
        for (std::int64_t b = 0; b < dn_blocks; ++b) {
          std::memcpy(dst + b * 18, s_dn + (r * dn_blocks + b) * 2, 2);
          std::memcpy(dst + b * 18 + 2, c_dn + (r * dn_blocks + b) * 16, 16);
        }
      }
      MTP_CU(cudaMemcpy(mtp_exp_ + e * mtp_blob_, out.data(), (std::size_t)mtp_blob_, cudaMemcpyHostToDevice), err);
    }
  }
  {
    std::vector<int> ident((std::size_t)mtp_nexp_);
    for (std::int64_t e = 0; e < mtp_nexp_; ++e) ident[(std::size_t)e] = (int)e;
    MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_res_), ident.size() * sizeof(int)), err);
    MTP_CU(cudaMemcpy(mtp_res_, ident.data(), ident.size() * sizeof(int), cudaMemcpyHostToDevice), err);
    MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_tail0_), sizeof(int)), err);
    MTP_CU(cudaMemset(mtp_tail0_, 0, sizeof(int)), err);
  }
  // ---- the draft head: output.weight's rows of the draft vocabulary (draft_vocab.bin: int32 ids)
  {
    std::vector<std::uint8_t> raw;
    if (!read_file(dir + "/draft_vocab.bin", raw) || raw.size() < 4 || raw.size() % 4 != 0) {
      err = "qwen4exp mtp: cannot read " + dir + "/draft_vocab.bin";
      return false;
    }
    mtp_ndv_ = (std::int64_t)(raw.size() / 4);
    const WSlot& head = W("output.weight");
    if (head.type == 0 || head.type == (int)k::Quant::bf16 || head.q == nullptr) {
      err = "qwen4exp mtp: the draft head needs a quantized output.weight";
      return false;
    }
    const std::int64_t rb = (std::int64_t)k::weight_bytes((k::Quant)head.type, (int)N, 1);
    MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_dvocab_), raw.size()), err);
    MTP_CU(cudaMemcpy(mtp_dvocab_, raw.data(), raw.size(), cudaMemcpyHostToDevice), err);
    MTP_CU(cudaMalloc(&mtp_head_, (std::size_t)(mtp_ndv_ * rb)), err);
    gather_rows_kernel<<<1024, 256>>>(static_cast<const std::uint8_t*>(head.q), rb, mtp_dvocab_, mtp_ndv_,
                                      static_cast<std::uint8_t*>(mtp_head_));
    MTP_CU(cudaDeviceSynchronize(), err);
    mtp_head_slot_.type = head.type;
    mtp_head_slot_.q = mtp_head_;
    mtp_head_slot_.n_in = N;
    mtp_head_slot_.n_out = mtp_ndv_;
  }
  // ---- the layer's own K/V, the routed-expert scratch, the I/O buffers, the activation scratch
  const std::size_t kv_bytes = (std::size_t)(KV * max_ctx_) * sizeof(float);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_kc_), kv_bytes), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_vc_), kv_bytes), err);
  MTP_CU(cudaMemset(mtp_kc_, 0, kv_bytes), err);
  MTP_CU(cudaMemset(mtp_vc_, 0, kv_bytes), err);
  const int K = g_.n_exp_used;
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_ids_), (std::size_t)K * sizeof(int)), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_eff_), (std::size_t)K * sizeof(int)), err);
  MTP_CU(cudaMalloc(&mtp_xq8_, k::q8_1_scratch_bytes((int)N, 1)), err);
  MTP_CU(cudaMalloc(&mtp_hq8_, (std::size_t)K * k::q8_1_scratch_bytes((int)mtp_ff_, 1)), err);
  const std::int64_t W8 = kWinMax;
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mtok_), (std::size_t)W8 * sizeof(int)), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mcell_), (std::size_t)W8 * sizeof(int)), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mrows_), (std::size_t)(W8 * pos_rows_) * sizeof(int)), err);
  MTP_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_mio_), (std::size_t)(W8 * (2 + pos_rows_)) * sizeof(int),
                       cudaHostAllocDefault),
         err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mpick_), sizeof(int)), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mtokout_), sizeof(int)), err);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&d_mprob_), sizeof(float)), err);
  MTP_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_mout_), 2 * sizeof(int), cudaHostAllocDefault), err);
  std::int64_t s = 0;
  auto take = [&](std::int64_t n) { const std::int64_t at = s; s += (n + 3) / 4 * 4; return at; };
  mo_emb_ = take(W8 * N);
  mo_en_ = take(W8 * N);
  mo_e2_ = take(W8 * N);
  mo_hn_ = take(W8 * HCD);
  mo_h2_ = take(W8 * HCD);
  mo_r_ = take(W8 * HCD);
  mo_xn_ = take(W8 * HCD);
  mo_lo_ = take(W8 * LR);
  mo_hg_ = take(W8 * HCD);
  mo_mixed_ = take(W8 * N);
  mo_inj_ = take(W8 * HC);
  mo_k_ = take(W8 * KV);
  mo_v_ = take(W8 * KV);
  mo_qf_ = take(2 * QD);
  mo_q_ = take(QD);
  mo_qg_ = take(QD);
  mo_at_ = take(QD);
  mo_at2_ = take(QD);
  mo_bo_ = take(N);
  mo_inj2_ = take(HC);
  mo_rlog_ = take(mtp_nexp_);
  mo_rw_ = take(K);
  mo_parts_ = take((std::int64_t)K * N);
  mo_g_ = take((std::int64_t)K * mtp_ff_);
  mo_u_ = take((std::int64_t)K * mtp_ff_);
  mo_h_ = take((std::int64_t)K * mtp_ff_);
  mo_sg_ = take(mtp_ff_);
  mo_su_ = take(mtp_ff_);
  mo_sh_ = take(mtp_ff_);
  mo_shd_ = take(N);
  mo_sgl_ = take(1);
  mo_moe_ = take(N);
  mo_ffn_ = take(N);
  mo_sample_ = take(N);
  mo_hlog_ = take(mtp_ndv_);
  MTP_CU(cudaMalloc(reinterpret_cast<void**>(&mtp_sc_), (std::size_t)s * sizeof(float)), err);
  const double mib = (double)(dense.size() + (std::size_t)(mtp_nexp_ * mtp_blob_) +
                              (std::size_t)(mtp_ndv_ * k::weight_bytes((k::Quant)W("output.weight").type, (int)N, 1)) +
                              2 * kv_bytes + (std::size_t)s * 4) /
                     (1024.0 * 1024.0);
  std::fprintf(stderr,
               "qwen4exp: MTP draft layer from %s: %zu dense tensors, %lld Q2_0 experts (%.1f MiB), draft head over "
               "%lld tokens; %.0f MiB of VRAM\n",
               dir.c_str(), mtpw_.size(), (long long)mtp_nexp_,
               (double)(mtp_nexp_ * mtp_blob_) / (1024.0 * 1024.0), (long long)mtp_ndv_, mib);
  return true;
}

// The HC read of the MTP's hyper-connection `prefix` over n rows of R: mixed = the block input, inj = the inject
// logits (nullptr: the final mixer, which has none). The main model's read() with the MTP's weights.
void Qwen4ExpGraph::mtp_read_(const std::string& prefix, float* R, int n, float* mixed, float* inj) {
  const std::int64_t N = g_.n_embd, HCD = (std::int64_t)g_.hc * N, LR = g_.hc_lr;
  qwen4exp::HcShapes hcs{g_.n_embd, g_.hc, g_.hc_lr};
  float* const A = mtp_sc_;
  qwen4exp::hc_rms_gamma(R, mtpw_.at(prefix + ".hc_norm.weight").f32, hcs, kMtpEps, A + mo_xn_, cs_, n);
  gemv_multi_(mtpw_.at(prefix + ".input_mix_weight_down.weight"), A + mo_xn_, A + mo_lo_, LR, HCD, n, cs_);
  k::scale_inplace(A + mo_lo_, (std::int64_t)n * LR, 1.0f / g_.hc, cs_);
  k::silu_inplace(A + mo_lo_, (std::int64_t)n * LR, cs_);
  gemv_multi_(mtpw_.at(prefix + ".input_mix_weight_up.weight"), A + mo_lo_, A + mo_hg_, HCD, LR, n, cs_);
  qwen4exp::hc_gated_mean(A + mo_xn_, A + mo_hg_, hcs, mixed, cs_, n);
  if (inj != nullptr)
    gemv_multi_(mtpw_.at(prefix + ".block_inject_weight.weight"), A + mo_xn_, inj, g_.hc, HCD, n, cs_);
}

// n rows (cells d_mcell_[0..n), tokens d_mtok_, rope rows d_mrows_): the front (embeddings, the input branches,
// the attention HC read, K/V appended at the rows' cells). full (n == 1): the rest of the layer and the head --
// the draft (d_mtokout_ / d_mprob_) and the layer's residual in mo_r_ row 0 for the next step.
bool Qwen4ExpGraph::mtp_forward_(int n, bool full, const float* rin, std::string& err) {
  const std::int64_t N = g_.n_embd, HC = g_.hc, HCD = HC * N;
  const std::int64_t NH = g_.n_head, HD = g_.head_dim, NKV = g_.n_head_kv, QD = NH * HD, KV = NKV * HD;
  const int K = g_.n_exp_used;
  qwen4exp::HcShapes hcs{g_.n_embd, g_.hc, g_.hc_lr};
  qwen4exp::QsaShapes qss{g_.head_dim, g_.n_head, g_.n_head_kv, g_.n_rot, g_.idx_q_heads, g_.idx_key_dim,
                         g_.idx_block, g_.idx_top_k};
  const auto cs = (cudaStream_t)cs_;
  float* const A = mtp_sc_;
  float* const R = A + mo_r_;
  auto MW = [&](const char* name) -> const WSlot& { return mtpw_.at(name); };
  // ---- the two input branches
  const WSlot& emb = W("token_embd.weight");
  const std::size_t rb = k::weight_bytes(k::Quant::iq4_xs, (int)N, 1);
  for (int t = 0; t < n; ++t) k::dequant_iq4xs_row(emb.q, rb, d_mtok_ + t, (int)N, A + mo_emb_ + t * N, cs_);
  MTP_CU(cudaMemcpyAsync(A + mo_en_, A + mo_emb_, (std::size_t)(n * N) * sizeof(float), cudaMemcpyDeviceToDevice, cs),
         err);
  k::rms_norm(A + mo_en_, MW("pre_fc_norm_embedding.weight").f32, n, N, kMtpEps, cs_);
  gemv_multi_(MW("fc_embedding.weight"), A + mo_en_, A + mo_e2_, N, N, n, cs_);
  MTP_CU(cudaMemcpyAsync(A + mo_hn_, rin, (std::size_t)(n * HCD) * sizeof(float), cudaMemcpyDeviceToDevice, cs), err);
  k::rms_norm(A + mo_hn_, MW("pre_fc_norm_hidden.weight").f32, n, HCD, kMtpEps, cs_);  // one RMS over hc*n_embd
  for (std::int64_t c0 = 0; c0 < n * HC; c0 += 8) {  // fc_hidden per stream, 8 stream rows a call
    const int nc = (int)std::min<std::int64_t>(8, n * HC - c0);
    gemv_multi_(MW("fc_hidden.weight"), A + mo_hn_ + c0 * N, A + mo_h2_ + c0 * N, N, N, nc, cs_);
  }
  {
    const long long tot = (long long)n * HCD;
    add_streams_broadcast_kernel<<<(unsigned)((tot + 255) / 256), 256, 0, cs>>>(A + mo_h2_, A + mo_e2_, R, N,
                                                                                (int)HC, n);
  }
  // ---- the attention hyper-connection read, then K/V into the layer's cache at the rows' cells
  mtp_read_("attn_hyper_connection", R, n, A + mo_mixed_, A + mo_inj_);
  gemv_multi_(MW("self_attn.k_proj.weight"), A + mo_mixed_, A + mo_k_, KV, N, n, cs_);
  gemv_multi_(MW("self_attn.v_proj.weight"), A + mo_mixed_, A + mo_v_, KV, N, n, cs_);
  k::rms_norm(A + mo_k_, MW("self_attn.k_norm.weight").f32, (std::int64_t)n * NKV, HD, kMtpEps, cs_);
  for (int t = 0; t < n; ++t)
    k::rope_neox_base(A + mo_k_ + t * KV, A + mo_k_ + t * KV, NKV, (int)HD, g_.n_rot, (float)g_.rope_freq_base,
                      d_mrows_ + (std::int64_t)t * pos_rows_, cs_);
  qwen4exp::qsa_kv_append(A + mo_k_, A + mo_v_, d_mcell_, qss, n, mtp_kc_, mtp_vc_, cs_);
  if (!full) return true;
  if (n != 1) {
    err = "qwen4exp mtp: the full layer runs one row";
    return false;
  }
  // ---- dense attention over every cell up to this one
  gemv_(MW("self_attn.q_proj.weight"), A + mo_mixed_, A + mo_qf_, 2 * QD, N, cs_);
  MTP_CU(cudaMemcpy2DAsync(A + mo_q_, (std::size_t)HD * sizeof(float), A + mo_qf_, (std::size_t)2 * HD * sizeof(float),
                           (std::size_t)HD * sizeof(float), (std::size_t)NH, cudaMemcpyDeviceToDevice, cs),
         err);
  MTP_CU(cudaMemcpy2DAsync(A + mo_qg_, (std::size_t)HD * sizeof(float), A + mo_qf_ + HD,
                           (std::size_t)2 * HD * sizeof(float), (std::size_t)HD * sizeof(float), (std::size_t)NH,
                           cudaMemcpyDeviceToDevice, cs),
         err);
  k::rms_norm(A + mo_q_, MW("self_attn.q_norm.weight").f32, NH, HD, kMtpEps, cs_);
  k::rope_neox_base(A + mo_q_, A + mo_q_, NH, (int)HD, g_.n_rot, (float)g_.rope_freq_base, d_mrows_, cs_);
  k::attn_decode_devlen(A + mo_q_, mtp_kc_, mtp_vc_, A + mo_at_, (int)NH, (int)NKV, (int)HD, d_mcell_,
                        1.0f / std::sqrt((float)HD), cs_);
  qwen4exp::qsa_out_gate(A + mo_at_, A + mo_qg_, qss, 1, A + mo_at2_, cs_);
  gemv_(MW("self_attn.o_proj.weight"), A + mo_at2_, A + mo_bo_, N, QD, cs_);
  qwen4exp::hc_inject(R, A + mo_bo_, A + mo_inj_, hcs, cs_);  // the attention write
  // ---- the MLP hyper-connection read, the MoE (512 resident Q2_0 experts + the shared expert), its write
  mtp_read_("mlp_hyper_connection", R, 1, A + mo_mixed_, A + mo_inj2_);
  gemv_(MW("mlp.gate.weight"), A + mo_mixed_, A + mo_rlog_, mtp_nexp_, N, cs_);
  k::router_topk(A + mo_rlog_, 1, (int)mtp_nexp_, K, mtp_ids_, A + mo_rw_, cs_);
  const std::int64_t mat_gu = mtp_ff_ * (N / 64) * 18;
  k::expert_route_plan(mtp_ids_, mtp_res_, K, mtp_tail0_, mtp_exp_, mtp_blob_, mat_gu, mtp_exp_ + mat_gu, mtp_blob_,
                       mat_gu, mtp_exp_ + 2 * mat_gu, mtp_blob_, N * (mtp_ff_ / 64) * 18, nullptr, 0, mtp_eff_, cs_);
  k::quantize_q8_1(A + mo_mixed_, mtp_xq8_, (int)N, 1, cs_);
  const int q2 = (int)k::Quant::q2_0;
  k::iq_mmvq_grouped(q2, mtp_ids_, mtp_eff_, mtp_exp_, mtp_blob_, 0, nullptr, 0, mtp_exp_, mtp_blob_, mtp_xq8_, 0,
                     A + mo_g_, mtp_ff_, (int)N, (int)mtp_ff_, K, cs_, K);
  k::iq_mmvq_grouped(q2, mtp_ids_, mtp_eff_, mtp_exp_, mtp_blob_, mat_gu, nullptr, 0, mtp_exp_ + mat_gu, mtp_blob_,
                     mtp_xq8_, 0, A + mo_u_, mtp_ff_, (int)N, (int)mtp_ff_, K, cs_, K);
  k::swiglu(A + mo_g_, A + mo_u_, A + mo_h_, (std::int64_t)K * mtp_ff_, cs_);
  k::quantize_q8_1(A + mo_h_, mtp_hq8_, (int)mtp_ff_, K, cs_);
  k::iq_mmvq_grouped(q2, mtp_ids_, mtp_eff_, mtp_exp_, mtp_blob_, 2 * mat_gu, nullptr, 0, mtp_exp_ + 2 * mat_gu,
                     mtp_blob_, mtp_hq8_, mtp_ff_ / 32, A + mo_parts_, N, (int)mtp_ff_, (int)N, K, cs_, 1);
  k::moe_combine(A + mo_parts_, A + mo_rw_, nullptr, A + mo_moe_, N, K, cs_);
  gemv_(MW("mlp.shared_expert.gate_proj.weight"), A + mo_mixed_, A + mo_sg_, mtp_ff_, N, cs_);
  gemv_(MW("mlp.shared_expert.up_proj.weight"), A + mo_mixed_, A + mo_su_, mtp_ff_, N, cs_);
  k::swiglu(A + mo_sg_, A + mo_su_, A + mo_sh_, mtp_ff_, cs_);
  gemv_(MW("mlp.shared_expert.down_proj.weight"), A + mo_sh_, A + mo_shd_, N, mtp_ff_, cs_);
  gemv_(MW("mlp.shared_expert_gate.weight"), A + mo_mixed_, A + mo_sgl_, 1, N, cs_);
  qwen4exp::shexp_combine(A + mo_moe_, A + mo_shd_, A + mo_sgl_, (int)N, A + mo_ffn_, cs_);
  qwen4exp::hc_inject(R, A + mo_ffn_, A + mo_inj2_, hcs, cs_);  // the MoE write: R is the next step's residual
  // ---- the final mixer and the draft head
  mtp_read_("hyper_connection_mixer", R, 1, A + mo_sample_, nullptr);
  gemv_(mtp_head_slot_, A + mo_sample_, A + mo_hlog_, mtp_ndv_, N, cs_);
  k::SamplerParams sp;
  sp.greedy = true;
  k::sample_tokens(A + mo_hlog_, 1, (int)mtp_ndv_, nullptr, 0, sp, d_mpick_, cs_);
  draft_pick_kernel<<<1, 1024, 0, cs>>>(A + mo_hlog_, (int)mtp_ndv_, d_mpick_, mtp_dvocab_, d_mtokout_, d_mprob_);
  return true;
}

bool Qwen4ExpGraph::mtp_kv_rows_(const float* rin, int n, const std::int32_t* toks, std::int64_t cell0,
                                 std::string& err) {
  if (n <= 0) return true;
  if (n > kWinMax) {
    err = "qwen4exp mtp: too many K/V rows";
    return false;
  }
  const auto cs = (cudaStream_t)cs_;
  MTP_CU(cudaStreamSynchronize(cs), err);  // the pinned inputs are free again
  int* tok = h_mio_;
  int* cell = h_mio_ + kWinMax;
  int* rows = h_mio_ + 2 * kWinMax;
  for (int t = 0; t < n; ++t) {
    tok[t] = toks[t];
    cell[t] = (int)(cell0 + t);
    for (int r = 0; r < pos_rows_; ++r) rows[t * pos_rows_ + r] = (int)(cell0 + t);
  }
  MTP_CU(cudaMemcpyAsync(d_mtok_, tok, (std::size_t)n * sizeof(int), cudaMemcpyHostToDevice, cs), err);
  MTP_CU(cudaMemcpyAsync(d_mcell_, cell, (std::size_t)n * sizeof(int), cudaMemcpyHostToDevice, cs), err);
  MTP_CU(cudaMemcpyAsync(d_mrows_, rows, (std::size_t)(n * pos_rows_) * sizeof(int), cudaMemcpyHostToDevice, cs), err);
  return mtp_forward_(n, false, rin, err);
}

bool Qwen4ExpGraph::mtp_draft_row_(const float* rin, std::int32_t tok, std::int64_t cell, std::int32_t& out,
                                   float& prob, std::string& err) {
  const auto cs = (cudaStream_t)cs_;
  MTP_CU(cudaStreamSynchronize(cs), err);
  h_mio_[0] = tok;
  h_mio_[kWinMax] = (int)cell;
  for (int r = 0; r < pos_rows_; ++r) h_mio_[2 * kWinMax + r] = (int)cell;
  MTP_CU(cudaMemcpyAsync(d_mtok_, h_mio_, sizeof(int), cudaMemcpyHostToDevice, cs), err);
  MTP_CU(cudaMemcpyAsync(d_mcell_, h_mio_ + kWinMax, sizeof(int), cudaMemcpyHostToDevice, cs), err);
  MTP_CU(cudaMemcpyAsync(d_mrows_, h_mio_ + 2 * kWinMax, (std::size_t)pos_rows_ * sizeof(int), cudaMemcpyHostToDevice,
                         cs),
         err);
  if (!mtp_forward_(1, true, rin != nullptr ? rin : mtp_sc_ + mo_r_, err)) return false;
  MTP_CU(cudaMemcpyAsync(h_mout_, d_mtokout_, sizeof(int), cudaMemcpyDeviceToHost, cs), err);
  MTP_CU(cudaMemcpyAsync(h_mout_ + 1, d_mprob_, sizeof(float), cudaMemcpyDeviceToHost, cs), err);
  MTP_CU(cudaStreamSynchronize(cs), err);
  out = h_mout_[0];
  std::memcpy(&prob, h_mout_ + 1, sizeof(float));
  mtp_draft_cell_ = cell;
  ++mtp_steps_;
  return true;
}

int Qwen4ExpGraph::mtp_chain_(const float* rin, std::int32_t tok, std::int64_t cell, int max_drafts,
                              std::int32_t* drafts, std::string& err) {
  if (max_drafts <= 0) return 0;
  ++mtp_rounds_;
  float prob = 0.0f;
  if (!mtp_draft_row_(rin, tok, cell, drafts[0], prob, err)) return -1;
  int n = 1;
  while (n < max_drafts && prob >= spec_min_p_ && cell + n < max_ctx_) {  // the next step reads its own residual
    if (!mtp_draft_row_(nullptr, drafts[n - 1], cell + n, drafts[n], prob, err)) return -1;
    ++n;
  }
  return n;
}

}  // namespace u
