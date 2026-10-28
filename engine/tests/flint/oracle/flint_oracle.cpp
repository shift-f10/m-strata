// TEST-ONLY. Official-graph oracle: evaluate a token-id sequence with llama.cpp master (e86f5fd1, flint) and dump
// the full logits for the last N positions (N x n_vocab F32), the layout multiengine-flint's --dump-full-logits writes.
// Usage: flint_oracle <model.gguf> <tokens.txt (comma/space ids)> <n_last> <out.bin> [n_gpu_layers=0] [threads=16]
//        [cache type f16|f32|q8_0|q4_0] [flash attention auto|on|off]
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s model.gguf tokens.txt n_last out.bin [n_gpu_layers] [threads]\n", argv[0]);
        return 2;
    }
    const char* model_path = argv[1];
    const int n_last = std::atoi(argv[3]);
    const int ngl = argc > 5 ? std::atoi(argv[5]) : 0;
    const int threads = argc > 6 ? std::atoi(argv[6]) : 16;
    // [cache type]: the K/V cache's ggml type - f16 (llama.cpp's default), f32, q8_0, q4_0
    const std::string ct = argc > 7 ? argv[7] : "f16";
    const ggml_type ctype = ct == "f32" ? GGML_TYPE_F32 : ct == "q8_0" ? GGML_TYPE_Q8_0 : ct == "q4_0" ? GGML_TYPE_Q4_0
                                                                                                        : GGML_TYPE_F16;

    std::ifstream in(argv[2]);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    for (char& c : text)
        if (c == ',') c = ' ';
    std::vector<llama_token> toks;
    {
        std::istringstream is(text);
        long long v;
        while (is >> v) toks.push_back((llama_token) v);
    }
    const int n = (int) toks.size();
    if (n_last <= 0 || n_last > n) {
        std::fprintf(stderr, "n_last %d out of range (have %d tokens)\n", n_last, n);
        return 2;
    }

    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = ngl;
    llama_model* model = llama_model_load_from_file(model_path, mp);
    if (model == nullptr) {
        std::fprintf(stderr, "model load failed\n");
        return 1;
    }
    const llama_vocab* vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t) (n + 16);
    cp.n_batch = (uint32_t) n;
    cp.n_ubatch = (uint32_t) (n < 512 ? n : 512);   // (long prompts: the decode splits the batch; same graph)
    // [fa]: flash attention auto (llama.cpp's default) / on / off - its kernels read K as F16 whatever the cache holds,
    // so an F32 reference needs it off
    const std::string fa = argc > 8 ? argv[8] : "auto";
    cp.flash_attn_type = fa == "off" ? LLAMA_FLASH_ATTN_TYPE_DISABLED
                         : fa == "on" ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                                      : LLAMA_FLASH_ATTN_TYPE_AUTO;
    cp.type_k = ctype;   // (K and V alike)
    cp.type_v = ctype;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    llama_context* ctx = llama_init_from_model(model, cp);
    if (ctx == nullptr) {
        std::fprintf(stderr, "context init failed\n");
        return 1;
    }

    llama_batch b = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; ++i) {
        b.token[i] = toks[(size_t) i];
        b.pos[i] = i;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = 0;
        b.logits[i] = (int8_t) (i >= n - n_last);
    }
    b.n_tokens = n;
    if (llama_decode(ctx, b) != 0) {
        std::fprintf(stderr, "decode failed\n");
        return 1;
    }

    std::FILE* f = std::fopen(argv[4], "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[4]);
        return 1;
    }
    for (int i = n - n_last; i < n; ++i) {
        const float* lg = llama_get_logits_ith(ctx, i);
        if (lg == nullptr) {
            std::fprintf(stderr, "no logits at %d\n", i);
            std::fclose(f);
            return 1;
        }
        std::fwrite(lg, sizeof(float), (size_t) n_vocab, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "oracle: %d tokens, logits for the last %d positions (n_vocab %d) -> %s\n", n, n_last, n_vocab,
                 argv[4]);
    llama_batch_free(b);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
