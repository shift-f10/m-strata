// strata-unified-engine: GPU kernel self-checks (see selftest.hpp).
#include "kernels/selftest.hpp"

#include "kernels/common.cuh"
#include "u_cpu_mmvq.hpp"  // the CPU expert lane's MMVQ mirror, checked against the GPU here
#if defined(STRATA_U_GGML)
#include "ggml.h"
#include "ggml-cpu.h"
#endif

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace u::k {

namespace {

struct DevBuf {
  void* p = nullptr;
  explicit DevBuf(std::size_t bytes) {
    if (cudaMalloc(&p, bytes) != cudaSuccess) p = nullptr;
  }
  ~DevBuf() {
    if (p) cudaFree(p);
  }
  DevBuf(const DevBuf&) = delete;
  DevBuf& operator=(const DevBuf&) = delete;
};

void line(std::string& report, const char* fmt, long long a, long long b, int nc, const char* verdict, double err) {
  char buf[256];
  std::snprintf(buf, sizeof buf, fmt, a, b, nc, verdict, err);
  report += buf;
}

// bf16_gemv_f32x: NC columns vs NC single-column calls (bitwise), vs bf16_gemv on f32_to_bf16(x) (tolerance).
bool check_bf16_f32x(long long n_out, long long n_in, int nc, std::mt19937& rng, std::string& report) {
  std::normal_distribution<float> nd(0.0f, 1.0f);
  std::vector<std::uint16_t> hw((std::size_t)(n_out * n_in));
  for (auto& v : hw) v = bf16_from_f32(nd(rng) * 0.05f);
  std::vector<float> hx((std::size_t)(nc * n_in));
  for (auto& v : hx) v = nd(rng);
  DevBuf w(hw.size() * 2), x(hx.size() * 4), y_multi((std::size_t)(nc * n_out) * 4), y_one((std::size_t)n_out * 4),
      x16((std::size_t)n_in * 2), y_ref((std::size_t)n_out * 4);
  if (!w.p || !x.p || !y_multi.p || !y_one.p || !x16.p || !y_ref.p) {
    report += "selftest: device alloc failed\n";
    return false;
  }
  cudaMemcpy(w.p, hw.data(), hw.size() * 2, cudaMemcpyHostToDevice);
  cudaMemcpy(x.p, hx.data(), hx.size() * 4, cudaMemcpyHostToDevice);
  const auto* wd = static_cast<const std::uint16_t*>(w.p);
  const auto* xd = static_cast<const float*>(x.p);
  if (!bf16_gemv_f32x(xd, wd, static_cast<float*>(y_multi.p), n_in, n_out, nc, nullptr)) {
    report += "selftest: bf16_gemv_f32x refused a supported shape\n";
    return false;
  }
  std::vector<float> multi((std::size_t)(nc * n_out)), one((std::size_t)n_out), ref((std::size_t)n_out);
  cudaMemcpy(multi.data(), y_multi.p, multi.size() * 4, cudaMemcpyDeviceToHost);
  bool bitwise = true;
  double max_rel = 0.0;
  for (int c = 0; c < nc; ++c) {
    const float* xc = xd + (long long)c * n_in;
    if (!bf16_gemv_f32x(xc, wd, static_cast<float*>(y_one.p), n_in, n_out, 1, nullptr)) return false;
    f32_to_bf16(xc, static_cast<std::uint16_t*>(x16.p), n_in, nullptr);
    bf16_gemv(static_cast<const std::uint16_t*>(x16.p), wd, static_cast<float*>(y_ref.p), n_in, n_out, nullptr);
    cudaMemcpy(one.data(), y_one.p, one.size() * 4, cudaMemcpyDeviceToHost);
    cudaMemcpy(ref.data(), y_ref.p, ref.size() * 4, cudaMemcpyDeviceToHost);
    for (long long r = 0; r < n_out; ++r) {
      const float m = multi[(std::size_t)(c * n_out + r)], s = one[(std::size_t)r];
      if (std::memcmp(&m, &s, 4) != 0) bitwise = false;
      const double scale = std::max(1.0, std::fabs((double)ref[(std::size_t)r]));
      max_rel = std::max(max_rel, std::fabs((double)s - (double)ref[(std::size_t)r]) / scale);
    }
  }
  const bool ok = bitwise && max_rel < 1e-4 && cudaGetLastError() == cudaSuccess;
  line(report, "selftest bf16_gemv_f32x %lldx%lld nc=%d: %s (max rel diff vs bf16_gemv %.2e)\n", n_out, n_in, nc,
       ok ? "ok" : (bitwise ? "TOLERANCE FAIL" : "BITWISE FAIL"), max_rel);
  return ok;
}

// A random weight matrix in format `q`: random code bytes, every block's fp16 scale(s) set to a sane value so
// no block decodes to NaN/inf. Returns the byte image (blocks laid out row-major, as the GGUF stores them).
std::vector<std::uint8_t> random_blocks(Quant q, int n_in, int n_out, std::mt19937& rng) {
  const std::size_t bytes = weight_bytes(q, n_in, n_out);
  const int be = block_elems(q);
  const std::size_t nblk = (std::size_t)n_out * (std::size_t)(n_in / be);
  const std::size_t bb = bytes / nblk;  // bytes a block
  std::vector<std::uint8_t> img(bytes);
  std::uniform_int_distribution<int> byte(0, 255);
  for (auto& b : img) b = (std::uint8_t)byte(rng);
  std::uniform_real_distribution<float> scale(0.002f, 0.02f);
  for (std::size_t i = 0; i < nblk; ++i) {
    std::uint8_t* blk = img.data() + i * bb;
    auto put_half = [&](std::size_t off) {
      const __half h = __float2half(scale(rng));
      std::memcpy(blk + off, &h, 2);
    };
    if (q == Quant::q6_k) {
      put_half(208);  // {ql[128], qh[64], scales[16], d}
    } else {
      put_half(0);  // d first in every other block
      if (q == Quant::q4_k || q == Quant::q5_k) put_half(2);  // dmin
    }
  }
  return img;
}

// mmvq: an NC-column call (one fused launch) vs NC single-column calls on the same Q8_1 columns (bitwise).
bool check_mmvq(Quant q, const char* name, int n_out, int n_in, int nc, std::mt19937& rng, std::string& report) {
  const std::vector<std::uint8_t> img = random_blocks(q, n_in, n_out, rng);
  std::normal_distribution<float> nd(0.0f, 1.0f);
  std::vector<float> hx((std::size_t)nc * (std::size_t)n_in);
  for (auto& v : hx) v = nd(rng);
  const std::size_t xq_bytes = q8_1_scratch_bytes(n_in, nc);
  DevBuf w(img.size()), x(hx.size() * 4), xq(xq_bytes), y_multi((std::size_t)nc * n_out * 4), y_one((std::size_t)n_out * 4);
  if (!w.p || !x.p || !xq.p || !y_multi.p || !y_one.p) {
    report += "selftest: device alloc failed\n";
    return false;
  }
  cudaMemcpy(w.p, img.data(), img.size(), cudaMemcpyHostToDevice);
  cudaMemcpy(x.p, hx.data(), hx.size() * 4, cudaMemcpyHostToDevice);
  quantize_q8_1(static_cast<const float*>(x.p), xq.p, n_in, nc, nullptr);
  mmvq(q, w.p, xq.p, static_cast<float*>(y_multi.p), n_in, n_out, nc, nullptr);
  std::vector<float> multi((std::size_t)nc * n_out), one((std::size_t)n_out);
  cudaMemcpy(multi.data(), y_multi.p, multi.size() * 4, cudaMemcpyDeviceToHost);
  const std::size_t col_bytes = xq_bytes / (std::size_t)nc;
  bool bitwise = true, finite = true;
  for (int c = 0; c < nc; ++c) {
    mmvq(q, w.p, static_cast<const std::uint8_t*>(xq.p) + (std::size_t)c * col_bytes, static_cast<float*>(y_one.p),
         n_in, n_out, 1, nullptr);
    cudaMemcpy(one.data(), y_one.p, one.size() * 4, cudaMemcpyDeviceToHost);
    for (int r = 0; r < n_out; ++r) {
      const float m = multi[(std::size_t)c * n_out + r], s = one[(std::size_t)r];
      if (std::memcmp(&m, &s, 4) != 0) bitwise = false;
      if (!std::isfinite(s)) finite = false;
    }
  }
  const bool ok = bitwise && finite && cudaGetLastError() == cudaSuccess;
  char buf[256];
  std::snprintf(buf, sizeof buf, "selftest mmvq %-7s %dx%d nc=%d: %s\n", name, n_out, n_in, nc,
                ok ? "ok" : (!bitwise ? "BITWISE FAIL" : "NON-FINITE / CUDA ERROR"));
  report += buf;
  return ok;
}

// The CPU expert lane's MMVQ mirror (u::cpu::mmvq_rows) against the GPU's own mmvq on the same weights and the
// same activation: a missed expert the CPU computes must equal the one the GPU would, so the only difference is
// the order the per-block float terms are summed (the GPU warp-reduces, the CPU sums a row left to right).
bool check_mmvq_cpu(Quant q, const char* name, int n_out, int n_in, std::mt19937& rng, std::string& report) {
  if (!u::cpu::mmvq_type_supported((int)q)) return true;  // only the CPU-lane quant set
  if (n_in % block_elems(q) != 0) return true;            // a 256-elem i-quant needs n_in % 256 == 0
  const std::vector<std::uint8_t> img = random_blocks(q, n_in, n_out, rng);
  std::normal_distribution<float> nd(0.0f, 1.0f);
  std::vector<float> hx((std::size_t)n_in);
  for (auto& v : hx) v = nd(rng);
  DevBuf w(img.size()), x(hx.size() * 4), xq(q8_1_scratch_bytes(n_in, 1)), y((std::size_t)n_out * 4);
  if (!w.p || !x.p || !xq.p || !y.p) {
    report += "selftest cpu-mmvq: device alloc failed\n";
    return false;
  }
  cudaMemcpy(w.p, img.data(), img.size(), cudaMemcpyHostToDevice);
  cudaMemcpy(x.p, hx.data(), hx.size() * 4, cudaMemcpyHostToDevice);
  quantize_q8_1(static_cast<const float*>(x.p), xq.p, n_in, 1, nullptr);
  mmvq(q, w.p, xq.p, static_cast<float*>(y.p), n_in, n_out, 1, nullptr);
  std::vector<float> ygpu((std::size_t)n_out);
  cudaMemcpy(ygpu.data(), y.p, ygpu.size() * 4, cudaMemcpyDeviceToHost);
  std::vector<u::cpu::BlockQ81> xb((std::size_t)(n_in / 32));
  u::cpu::quantize_q81(hx.data(), xb.data(), n_in);
  std::vector<float> ycpu((std::size_t)n_out);
  u::cpu::mmvq_rows((int)q, img.data(), (std::int64_t)(img.size() / (std::size_t)n_out), xb.data(), ycpu.data(),
                    n_in, n_out);
  double worst = 0.0;
  for (int r = 0; r < n_out; ++r) {
    const double den = std::max(1e-3, (double)std::fabs(ygpu[(std::size_t)r]));
    worst = std::max(worst, std::fabs((double)ycpu[(std::size_t)r] - (double)ygpu[(std::size_t)r]) / den);
  }
  const bool ok = worst < 2e-3 && cudaGetLastError() == cudaSuccess;
  char buf[256];
  std::snprintf(buf, sizeof buf, "selftest cpu-mmvq %-7s %dx%d: %s (worst rel %.2e)\n", name, n_out, n_in,
                ok ? "ok" : "FAIL", worst);
  report += buf;
  return ok;
}

// Timing (not a check): one NC-column call against one single-column call on the same matrix, the window's
// premise that reading the weights once serves NC tokens.
void bench_mmvq(Quant q, const char* name, int n_out, int n_in, std::mt19937& rng, std::string& report) {
  const std::vector<std::uint8_t> img = random_blocks(q, n_in, n_out, rng);
  DevBuf w(img.size()), x((std::size_t)8 * n_in * 4), xq(q8_1_scratch_bytes(n_in, 8)), y((std::size_t)8 * n_out * 4);
  if (!w.p || !x.p || !xq.p || !y.p) return;
  cudaMemcpy(w.p, img.data(), img.size(), cudaMemcpyHostToDevice);
  cudaMemset(x.p, 0, (std::size_t)8 * n_in * 4);
  quantize_q8_1(static_cast<const float*>(x.p), xq.p, n_in, 8, nullptr);
  cudaEvent_t a = nullptr, b = nullptr;
  cudaEventCreate(&a);
  cudaEventCreate(&b);
  char buf[256];
  std::string line_s = "selftest bench mmvq " + std::string(name) + " " + std::to_string(n_out) + "x" +
                       std::to_string(n_in) + ":";
  for (int nc : {1, 2, 4, 8}) {
    for (int i = 0; i < 5; ++i) mmvq(q, w.p, xq.p, static_cast<float*>(y.p), n_in, n_out, nc, nullptr);
    cudaEventRecord(a);
    for (int i = 0; i < 50; ++i) mmvq(q, w.p, xq.p, static_cast<float*>(y.p), n_in, n_out, nc, nullptr);
    cudaEventRecord(b);
    cudaEventSynchronize(b);
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, a, b);
    std::snprintf(buf, sizeof buf, " nc=%d %.1f us", nc, 1000.0f * ms / 50.0f);
    line_s += buf;
  }
  cudaEventDestroy(a);
  cudaEventDestroy(b);
  report += line_s + "\n";
}

// Timing: the PCIe ceiling the expert misses see -- a wide uint4 kernel reading 2 MB experts at scattered
// offsets of a mapped, write-combined pinned arena (zero-copy), against cudaMemcpy of the same bytes (DMA).
__global__ void zc_read_kernel(const uint4* __restrict__ src, long long n16, const long long* __restrict__ offs,
                               int n_chunks, uint4* __restrict__ sink) {
  const int c = (int)blockIdx.y;
  const uint4* p = src + offs[c];
  uint4 acc = make_uint4(0, 0, 0, 0);
  for (long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x; i < n16; i += (long long)gridDim.x * blockDim.x) {
    const uint4 v = p[i];
    acc.x ^= v.x;
    acc.y ^= v.y;
    acc.z ^= v.z;
    acc.w ^= v.w;
  }
  if (acc.x == 0x12345678u) sink[0] = acc;  // keeps the loads alive
}
void bench_pcie(std::string& report) {
  const char* ab = std::getenv("STRATA_SELFTEST_PCIE_MIB");  // the arena size (default 512 MiB)
  const std::size_t arena = (std::size_t)(ab != nullptr ? std::atoll(ab) : 512) << 20, chunk = (std::size_t)2 << 20;
  void* h = nullptr;
  if (cudaHostAlloc(&h, arena, cudaHostAllocMapped | cudaHostAllocWriteCombined) != cudaSuccess) {
    report += "selftest bench pcie: pinned alloc failed\n";
    return;
  }
  std::memset(h, 1, arena);
  void* hd = nullptr;
  cudaHostGetDevicePointer(&hd, h, 0);
  const int n_chunks = 32;
  std::vector<long long> offs(n_chunks);
  const std::size_t n_slots = arena / chunk - 1;
  for (int c = 0; c < n_chunks; ++c)
    offs[(std::size_t)c] = (long long)(((std::size_t)c * 7919 % n_slots) * chunk / 16);
  DevBuf d_offs(offs.size() * sizeof(long long)), sink(64), dst(chunk * n_chunks);
  cudaMemcpy(d_offs.p, offs.data(), offs.size() * sizeof(long long), cudaMemcpyHostToDevice);
  cudaEvent_t a = nullptr, b = nullptr;
  cudaEventCreate(&a);
  cudaEventCreate(&b);
  std::string line_s = "selftest bench pcie (" + std::to_string(n_chunks) + " x 2 MB scattered):";
  char buf[160];
  for (int nc1 : {1, 2}) {  // one or two experts at a time: what a single-token layer's misses see
    for (int blocks : {40, 160, 640}) {
      zc_read_kernel<<<dim3((unsigned)blocks, (unsigned)nc1), 256>>>(static_cast<const uint4*>(hd),
                                                                    (long long)(chunk / 16),
                                                                    static_cast<const long long*>(d_offs.p), nc1,
                                                                    static_cast<uint4*>(sink.p));
      cudaEventRecord(a);
      for (int r = 0; r < 20; ++r)
        zc_read_kernel<<<dim3((unsigned)blocks, (unsigned)nc1), 256>>>(static_cast<const uint4*>(hd),
                                                                      (long long)(chunk / 16),
                                                                      static_cast<const long long*>(d_offs.p), nc1,
                                                                      static_cast<uint4*>(sink.p));
      cudaEventRecord(b);
      cudaEventSynchronize(b);
      float ms1 = 0.0f;
      cudaEventElapsedTime(&ms1, a, b);
      std::snprintf(buf, sizeof buf, " [%d expert(s), %d blocks each: %.1f GB/s]", nc1, blocks,
                    (double)(20.0 * nc1 * chunk) / (ms1 * 1e6));
      line_s += buf;
    }
  }
  for (int blocks : {40, 160, 640}) {
    zc_read_kernel<<<dim3((unsigned)blocks, n_chunks), 256>>>(static_cast<const uint4*>(hd), (long long)(chunk / 16),
                                                            static_cast<const long long*>(d_offs.p), n_chunks,
                                                            static_cast<uint4*>(sink.p));
    cudaEventRecord(a);
    for (int r = 0; r < 5; ++r)
      zc_read_kernel<<<dim3((unsigned)blocks, n_chunks), 256>>>(static_cast<const uint4*>(hd), (long long)(chunk / 16),
                                                              static_cast<const long long*>(d_offs.p), n_chunks,
                                                              static_cast<uint4*>(sink.p));
    cudaEventRecord(b);
    cudaEventSynchronize(b);
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, a, b);
    std::snprintf(buf, sizeof buf, " zero-copy %d blocks/expert %.1f GB/s;", blocks,
                  (double)(5.0 * n_chunks * chunk) / (ms * 1e6));
    line_s += buf;
  }
  cudaEventRecord(a);
  for (int r = 0; r < 5; ++r)
    for (int c = 0; c < n_chunks; ++c)
      cudaMemcpyAsync(static_cast<std::uint8_t*>(dst.p) + (std::size_t)c * chunk,
                      static_cast<const std::uint8_t*>(h) + (std::size_t)offs[(std::size_t)c] * 16, chunk,
                      cudaMemcpyHostToDevice);
  cudaEventRecord(b);
  cudaEventSynchronize(b);
  float ms = 0.0f;
  cudaEventElapsedTime(&ms, a, b);
  std::snprintf(buf, sizeof buf, " DMA %.1f GB/s", (double)(5.0 * n_chunks * chunk) / (ms * 1e6));
  line_s += buf;
  cudaEventDestroy(a);
  cudaEventDestroy(b);
  cudaFreeHost(h);
  report += line_s + "\n";
}

// The same reads, the arena pinned as many slices (STRATA_SELFTEST_PCIE_SLICE_MIB each, the expert arena's way)
// and every chunk in its own slice: does the slicing, or the total pinned footprint, decide the cost?
__global__ void zc_read_ptrs_kernel(const unsigned long long* __restrict__ ptrs, long long n16, uint4* __restrict__ sink) {
  const uint4* p = reinterpret_cast<const uint4*>(ptrs[blockIdx.y]);
  uint4 acc = make_uint4(0, 0, 0, 0);
  for (long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x; i < n16; i += (long long)gridDim.x * blockDim.x) {
    const uint4 v = p[i];
    acc.x ^= v.x;
    acc.y ^= v.y;
    acc.z ^= v.z;
    acc.w ^= v.w;
  }
  if (acc.x == 0x12345678u) sink[0] = acc;
}
void bench_pcie_slices(std::string& report) {
  const char* sz = std::getenv("STRATA_SELFTEST_PCIE_SLICE_MIB");
  const char* tot = std::getenv("STRATA_SELFTEST_PCIE_MIB");
  if (sz == nullptr || tot == nullptr) return;
  const std::size_t slice = (std::size_t)std::atoll(sz) << 20, total = (std::size_t)std::atoll(tot) << 20;
  const std::size_t chunk = (std::size_t)2 << 20;
  const std::size_t n_slices = std::max<std::size_t>(1, total / slice);
  std::vector<void*> hs, reg;
  std::vector<unsigned long long> dev;
  for (std::size_t i = 0; i < n_slices; ++i) {
    void* h = nullptr;
    if (std::getenv("STRATA_SELFTEST_PCIE_REGISTER") != nullptr) {  // ordinary memory, registered (pinned + mapped)
      h = std::malloc(slice);
      if (h == nullptr) break;
      std::memset(h, 1, slice);
      if (cudaHostRegister(h, slice, cudaHostRegisterMapped) != cudaSuccess) {
        std::free(h);
        break;
      }
      reg.push_back(h);
    } else {
      if (cudaHostAlloc(&h, slice, cudaHostAllocMapped | cudaHostAllocWriteCombined) != cudaSuccess) break;
      std::memset(h, 1, slice);
    }
    void* hd = nullptr;
    cudaHostGetDevicePointer(&hd, h, 0);
    hs.push_back(h);
    dev.push_back((unsigned long long)(std::uintptr_t)hd);
  }
  const int n_chunks = 32;
  std::vector<unsigned long long> ptrs((std::size_t)n_chunks);
  const std::size_t per = slice / chunk - 1;
  for (int c = 0; c < n_chunks; ++c)
    ptrs[(std::size_t)c] = dev[(std::size_t)c * 7 % dev.size()] + (unsigned long long)(((std::size_t)c * 13 % per) * chunk);
  DevBuf d_ptrs(ptrs.size() * sizeof(unsigned long long)), sink(64);
  cudaMemcpy(d_ptrs.p, ptrs.data(), ptrs.size() * sizeof(unsigned long long), cudaMemcpyHostToDevice);
  cudaEvent_t a = nullptr, b = nullptr;
  cudaEventCreate(&a);
  cudaEventCreate(&b);
  std::string line_s = "selftest bench pcie slices (" + std::to_string(hs.size()) + " x " + std::to_string(slice >> 20) +
                       " MiB):";
  char buf[128];
  for (int nc : {1, 32}) {
    zc_read_ptrs_kernel<<<dim3(160u, (unsigned)nc), 256>>>(static_cast<const unsigned long long*>(d_ptrs.p),
                                                        (long long)(chunk / 16), static_cast<uint4*>(sink.p));
    cudaEventRecord(a);
    for (int r = 0; r < 10; ++r)
      zc_read_ptrs_kernel<<<dim3(160u, (unsigned)nc), 256>>>(static_cast<const unsigned long long*>(d_ptrs.p),
                                                          (long long)(chunk / 16), static_cast<uint4*>(sink.p));
    cudaEventRecord(b);
    cudaEventSynchronize(b);
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, a, b);
    std::snprintf(buf, sizeof buf, " %d expert(s) at a time %.1f GB/s;", nc, (double)(10.0 * nc * chunk) / (ms * 1e6));
    line_s += buf;
  }
  cudaEventDestroy(a);
  cudaEventDestroy(b);
  for (void* h : hs) {
    if (std::find(reg.begin(), reg.end(), h) != reg.end()) {
      cudaHostUnregister(h);
      std::free(h);
    } else {
      cudaFreeHost(h);
    }
  }
  report += line_s + (reg.empty() ? "" : " (malloc + cudaHostRegister)") + "\n";
}

}  // namespace

#if defined(STRATA_U_GGML)
// The CPU expert lane links the forked ggml-cpu for its dots (upstream native_expert.cpp). Prove it: every
// qwen4exp expert quant has a ggml-cpu vec_dot here (IQ2_S 22, IQ3_XXS 18, IQ3_S 21, IQ4_XS 23, IQ4_NL 20,
// the custom Q2_0 42) and the activation quant its vec_dot wants.
bool check_ggml_cpu(std::string& report) {
  ggml_cpu_init();
  bool ok = true;
  for (const int t : {22, 18, 21, 23, 20, 42}) {
    const ggml_type_traits_cpu* tr = ggml_get_type_traits_cpu((ggml_type)t);
    const bool has = tr != nullptr && tr->vec_dot != nullptr && tr->from_float != nullptr;
    const int vdt = has ? (int)tr->vec_dot_type : -1;
    char buf[160];
    std::snprintf(buf, sizeof buf, "selftest ggml-cpu type %-2d: %s (vec_dot_type %d)\n", t,
                  has ? "vec_dot + from_float" : "MISSING", vdt);
    report += buf;
    ok = ok && has;
  }
  return ok;
}
#endif

bool selftest_kernels(std::string& report) {
  std::mt19937 rng(20261024u);
  bool ok = true;
#if defined(STRATA_U_GGML)
  ok = check_ggml_cpu(report) && ok;
#endif
  // every MMVQ format, a row count that is not a multiple of 4 (the SmallK tail) and both SmallK branches
  const struct { Quant q; const char* name; } fmts[] = {
      {Quant::q4_k, "q4_k"},     {Quant::q5_k, "q5_k"},       {Quant::q6_k, "q6_k"},   {Quant::q8_0, "q8_0"},
      {Quant::iq2_xxs, "iq2_xxs"}, {Quant::iq2_s, "iq2_s"},   {Quant::iq3_xxs, "iq3_xxs"}, {Quant::iq3_s, "iq3_s"},
      {Quant::iq4_xs, "iq4_xs"}, {Quant::iq4_nl, "iq4_nl"},   {Quant::q2_0, "q2_0"}};
  const int mshapes[][2] = {{777, 2560}, {301, 6144}, {2560, 512}};
  for (const auto& f : fmts)
    for (const auto& sh : mshapes)
      for (int nc : {2, 3, 8}) ok = check_mmvq(f.q, f.name, sh[0], sh[1], nc, rng, report) && ok;
  // the CPU expert lane's dots against the GPU's, on the qwen4exp expert shapes (gate/up 2560-in, down 640-in)
  for (const auto& f : fmts)
    for (const int ni : {2560, 640}) ok = check_mmvq_cpu(f.q, f.name, 128, ni, rng, report) && ok;
  // the qwen4exp BF16 shapes (n_out x n_in): HC up, HC down, HC inject, ssm alpha/beta, shexp gate, router
  const long long shapes[][2] = {{10240, 320}, {320, 10240}, {4, 10240}, {48, 2560}, {1, 2560}, {512, 2560}};
  for (const auto& sh : shapes)
    for (int nc : {1, 2, 3, 4, 8}) ok = check_bf16_f32x(sh[0], sh[1], nc, rng, report) && ok;
  bench_mmvq(Quant::q6_k, "q6_k", 10240, 2560, rng, report);
  bench_mmvq(Quant::q5_k, "q5_k", 2560, 6144, rng, report);
  bench_mmvq(Quant::iq4_xs, "iq4_xs", 1536, 2560, rng, report);
  bench_pcie(report);
  bench_pcie_slices(report);
  report += ok ? "selftest: all kernel checks passed\n" : "selftest: FAILED\n";
  return ok;
}

}  // namespace u::k
