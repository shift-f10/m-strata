// engines/unified/src/kernels/glm_kda.cu - the glm5-next KDA pieces. See glm_kda.cuh for the math.
//
// PORTED FROM engines/maya-ref/src/kernels/cuda/glm_kda.cu (reviewed, verbatim kernel bodies;
// namespace strata::kernels -> u::glm, launcher bounds-checks preserved). The parity harness on the
// GPU box holds these kernels to engines/maya-ref/ref/glm.py's f64 reference at f32 tolerance.
//
// Six kernels:
//   conv        one thread per (channel, token): the d_conv taps read x_proj where the window is
//               inside the sequence and the state columns where it is not, then SiLU. READS history
//               only - the slide is a separate kernel (an in-place slide while earlier tokens still
//               read the old columns is a race).
//   conv_slide  one thread per channel: the history becomes the last (d_conv-1) columns of
//               [state | x_proj], oldest first; staged in registers before the write.
//   gate        one thread per output element: the lower-bound squash, reshaping (d_inner, T) into
//               (head_dim, n_head, T) on the way through.
//   recurrence  ONE BLOCK PER HEAD, tokens looped INSIDE the block (the state is carried by the
//               block, so the sequential dependency never leaves the SM). __syncthreads between
//               phases: key-axis row scale -> u = S^T k -> delta -> outer update -> o = S^T q. The
//               1/sqrt(dv) scale lands on o here, like ggml's kernel.
//   out_gate    one thread per (head, token): two passes over head_dim - the sum of squares for the
//               RMS, then the normed-and-gated write.
//   l2_heads    one thread per (head, token): GDN L2 norm over the head_dim column.
//
// All arithmetic f32.
#include "glm_kda.cuh"

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

namespace u::glm {
namespace {

constexpr int KD_THREADS = 256;
constexpr int KD_MAX_D_CONV = 16;   // the slide's register staging

__device__ __forceinline__ float dsigmoid(float x) { return 1.0f / (1.0f + __expf(-x)); }

__global__ void glm_kda_conv_kernel(const float* __restrict__ x_proj, const float* __restrict__ conv_w,
                                    const float* __restrict__ conv_state, int d_inner, int d_conv, int T,
                                    float* __restrict__ out) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = d_inner * T;
    if (idx >= total) return;
    const int c = idx % d_inner;      // ggml order: features fastest
    const int t = idx / d_inner;
    const int hist = d_conv - 1;

    float acc = 0.0f;
    for (int tap = 0; tap < d_conv; ++tap) {
        const int src = t - hist + tap;
        const float x = src >= 0 ? x_proj[(size_t) src * d_inner + c]
                                 : conv_state[(size_t)(src + hist) + (size_t) hist * c];
        acc += conv_w[(size_t) tap + (size_t) d_conv * c] * x;
    }
    out[(size_t) t * d_inner + c] = acc / (1.0f + __expf(-acc));   // silu
}

__global__ void glm_kda_conv_slide_kernel(const float* __restrict__ x_proj, float* __restrict__ conv_state,
                                          int d_inner, int d_conv, int T) {
    const int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= d_inner) return;
    const int hist = d_conv - 1;
    float nv[KD_MAX_D_CONV];
    for (int j = 0; j < hist; ++j) {
        const int src = T - hist + j;   // may still reach into the old state when T < hist
        nv[j] = src >= 0 ? x_proj[(size_t) src * d_inner + c]
                         : conv_state[(size_t)(src + hist) + (size_t) hist * c];
    }
    for (int j = 0; j < hist; ++j) conv_state[(size_t) j + (size_t) hist * c] = nv[j];
}

__global__ void glm_kda_gate_kernel(const float* __restrict__ g, const float* __restrict__ ssm_a,
                                    float lower_bound, int head_dim, int n_head, int T,
                                    float* __restrict__ g1) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    const int total = head_dim * n_head * T;
    if (idx >= total) return;
    const int e = idx % head_dim;
    const int h = (idx / head_dim) % n_head;
    const int t = idx / (head_dim * n_head);
    // g arrives (d_inner, T) in ggml head order: row e + head_dim*h
    const float v = g[(size_t)(e + head_dim * h) + (size_t) t * head_dim * n_head];
    g1[idx] = lower_bound * dsigmoid(-v * ssm_a[h]);
}

__global__ void glm_kda_recurrence_kernel(const float* __restrict__ q, const float* __restrict__ k,
                                          const float* __restrict__ v, const float* __restrict__ g1,
                                          const float* __restrict__ beta, float* __restrict__ state,
                                          int n_head, int head_dim, int T, float* __restrict__ out) {
    const int h = blockIdx.x;
    if (h >= n_head) return;
    const int dk = head_dim;
    const int tid = threadIdx.x;
    const int nt = blockDim.x;
    const float scale = rsqrtf((float) dk);

    float* S = state + (size_t) h * dk * dk;   // S[i*dk + j], row i = KEY axis
    extern __shared__ float smem[];            // u (dk) then delta (dk)
    float* s_u = smem;
    float* s_delta = smem + dk;

    for (int t = 0; t < T; ++t) {
        const float* qt = q + ((size_t) t * n_head + h) * dk;
        const float* kt = k + ((size_t) t * n_head + h) * dk;
        const float* vt = v + ((size_t) t * n_head + h) * dk;
        const float* gt = g1 + ((size_t) t * n_head + h) * dk;
        const float b = beta[(size_t) t * n_head + h];

        // the gate scales the KEY axis: S[i][:] *= exp(g1[i])
        for (int i = tid; i < dk; i += nt) {
            const float decay = __expf(gt[i]);
            for (int j = 0; j < dk; ++j) S[(size_t) i * dk + j] *= decay;
        }
        __syncthreads();

        // u[j] = sum_i S[i][j] * k[i]   (= (S^T k)[j])
        for (int j = tid; j < dk; j += nt) {
            float acc = 0.0f;
            for (int i = 0; i < dk; ++i) acc += S[(size_t) i * dk + j] * kt[i];
            s_u[j] = acc;
        }
        __syncthreads();

        // delta[j] = beta * (v[j] - u[j])
        for (int j = tid; j < dk; j += nt) s_delta[j] = b * (vt[j] - s_u[j]);
        __syncthreads();

        // S[i][j] += k[i] * delta[j]
        for (int idx = tid; idx < dk * dk; idx += nt) {
            const int i = idx / dk, j = idx % dk;
            S[(size_t) i * dk + j] += kt[i] * s_delta[j];
        }
        __syncthreads();

        // o[j] = (1/sqrt(dk)) * sum_i S[i][j] * q[i]   (= (1/sqrt(dv)) * (S^T q)[j])
        for (int j = tid; j < dk; j += nt) {
            float acc = 0.0f;
            for (int i = 0; i < dk; ++i) acc += S[(size_t) i * dk + j] * qt[i];
            out[((size_t) t * n_head + h) * dk + j] = acc * scale;
        }
        __syncthreads();
    }
}

__global__ void glm_kda_out_gate_kernel(const float* __restrict__ o, const float* __restrict__ norm_w,
                                        const float* __restrict__ g2, int head_dim, int n_head, int T,
                                        float eps, float* __restrict__ gated) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n_head * T) return;
    const int h = idx % n_head;
    const int t = idx / n_head;
    const float* ot = o + ((size_t) t * n_head + h) * head_dim;
    const float* gt = g2 + ((size_t) t * n_head + h) * head_dim;

    float ss = 0.0f;
    for (int e = 0; e < head_dim; ++e) ss += ot[e] * ot[e];
    const float inv = rsqrtf(ss / (float) head_dim + eps);
    for (int e = 0; e < head_dim; ++e)
        gated[((size_t) t * n_head + h) * head_dim + e] = ot[e] * inv * norm_w[e] * dsigmoid(gt[e]);
}

// gdn_l2_norm per head (glm_model.cu l2_heads_kernel, ref/glm.py::gdn_l2_norm): one thread per
// (head, token), x / sqrt(sum_e x_e^2 + eps) over the head_dim column.
__global__ void glm_l2_norm_heads_kernel(const float* __restrict__ x, float* __restrict__ out, int head_dim,
                                         int n_head, int T, float eps) {
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n_head * T) return;
    const float* xt = x + (size_t) idx * head_dim;
    float* ot = out + (size_t) idx * head_dim;
    float ss = 0.0f;
    for (int e = 0; e < head_dim; ++e) ss += xt[e] * xt[e];
    const float inv = rsqrtf(ss + eps);
    for (int e = 0; e < head_dim; ++e) ot[e] = xt[e] * inv;
}

}  // namespace

void glm_kda_conv(const float* x_proj, const float* conv_w, float* conv_state, int d_inner, int d_conv,
                  int T, float* out, void* stream) {
    if (d_inner <= 0 || d_conv <= 1 || T <= 0) return;
    if (d_conv > KD_MAX_D_CONV + 1) {
        std::fprintf(stderr, "glm_kda_conv: d_conv %d exceeds the kernel's %d\n", d_conv, KD_MAX_D_CONV + 1);
        std::exit(1);
    }
    cudaStream_t st = (cudaStream_t) stream;
    const int total = d_inner * T;
    glm_kda_conv_kernel<<<(total + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, st>>>(x_proj, conv_w,
                                                                                      conv_state, d_inner, d_conv,
                                                                                      T, out);
    glm_kda_conv_slide_kernel<<<(d_inner + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, st>>>(x_proj,
                                                                                              conv_state, d_inner,
                                                                                              d_conv, T);
}

void glm_kda_gate(const float* g, const float* ssm_a, float lower_bound, int head_dim, int n_head, int T,
                  float* g1, void* stream) {
    if (head_dim <= 0 || n_head <= 0 || T <= 0) return;
    const int total = head_dim * n_head * T;
    glm_kda_gate_kernel<<<(total + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, (cudaStream_t) stream>>>(
        g, ssm_a, lower_bound, head_dim, n_head, T, g1);
}

void glm_kda_recurrence(const float* q, const float* k, const float* v, const float* g1, const float* beta,
                        float* state, int n_head, int head_dim, int T, float* out, void* stream) {
    if (n_head <= 0 || head_dim <= 0 || T <= 0) return;
    if (head_dim > KD_THREADS) {
        std::fprintf(stderr, "glm_kda_recurrence: head_dim %d exceeds the kernel's %d threads\n", head_dim,
                     KD_THREADS);
        std::exit(1);
    }
    const size_t smem = (size_t) 2 * head_dim * sizeof(float);
    glm_kda_recurrence_kernel<<<n_head, KD_THREADS, smem, (cudaStream_t) stream>>>(q, k, v, g1, beta, state,
                                                                                   n_head, head_dim, T, out);
}

void glm_kda_out_gate(const float* o, const float* norm_w, const float* g2, int head_dim, int n_head, int T,
                      float eps, float* gated, void* stream) {
    if (head_dim <= 0 || n_head <= 0 || T <= 0) return;
    const int total = n_head * T;
    glm_kda_out_gate_kernel<<<(total + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, (cudaStream_t) stream>>>(
        o, norm_w, g2, head_dim, n_head, T, eps, gated);
}

void glm_l2_norm_heads(const float* x, float* out, int head_dim, int n_head, int T, float eps, void* stream) {
    if (head_dim <= 0 || n_head <= 0 || T <= 0) return;
    const int total = n_head * T;
    glm_l2_norm_heads_kernel<<<(total + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, (cudaStream_t) stream>>>(
        x, out, head_dim, n_head, T, eps);
}

namespace {
__global__ void glm_sigmoid_inplace_kernel(float* __restrict__ x, int n) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    x[i] = 1.0f / (1.0f + __expf(-x[i]));
}
}  // namespace

void glm_sigmoid_inplace(float* x, int n, void* stream) {
    if (n <= 0) return;
    glm_sigmoid_inplace_kernel<<<(n + KD_THREADS - 1) / KD_THREADS, KD_THREADS, 0, (cudaStream_t) stream>>>(x, n);
}

}  // namespace u::glm
