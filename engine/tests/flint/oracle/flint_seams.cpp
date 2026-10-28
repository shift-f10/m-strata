// Official-graph per-seam oracle: run llama.cpp master (e86f5fd1, flint) on a token sequence in one batch and
// dump every named F32 tensor (llama.cpp's cb() seams, "<name>-<layer>", plus result_norm/result_output) in full.
// File <dir>/<name>.bin = int64 ne[4] header + raw F32 data (contiguous tensors only; the rest are listed on stderr).
// Usage: flint_seams <model.gguf> <tokens.txt> <out_dir> [n_gpu_layers=0] [threads=16]
#include "ggml-backend.h"
#include "ggml.h"
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

struct Dumper {
    std::string dir;
    std::set<std::string> seen;
    int written = 0;
};

static bool wanted(const char* name) {
    if (name == nullptr || name[0] == 0) return false;
    if (std::strcmp(name, "result_norm") == 0 || std::strcmp(name, "result_output") == 0) return true;
    const char* dash = std::strrchr(name, '-');
    if (dash == nullptr || dash[1] == 0) return false;
    for (const char* p = dash + 1; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;   // "<name>-<layer>"
}

static bool cb(struct ggml_tensor* t, bool ask, void* ud) {
    Dumper* d = (Dumper*) ud;
    const char* name = ggml_get_name(t);
    if (ask) return wanted(name);
    if (!wanted(name)) return true;
    // (each ubatch overwrites: the files end with the last ubatch's tensors, the one holding the last token)
    if (!d->seen.count(name)) d->seen.insert(name);
    if (t->type != GGML_TYPE_F32 || !ggml_is_contiguous(t)) {
        std::fprintf(stderr, "skip %s (type %d contiguous %d)\n", name, (int) t->type, (int) ggml_is_contiguous(t));
        return true;
    }
    const size_t nb = ggml_nbytes(t);
    std::vector<char> buf(nb);
    ggml_backend_tensor_get(t, buf.data(), 0, nb);
    const std::string path = d->dir + "/" + name + ".bin";
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        const int64_t ne[4] = {t->ne[0], t->ne[1], t->ne[2], t->ne[3]};
        std::fwrite(ne, sizeof(int64_t), 4, f);
        std::fwrite(buf.data(), 1, nb, f);
        std::fclose(f);
        d->written = (int) d->seen.size();
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s model.gguf tokens.txt out_dir [n_gpu_layers] [threads]\n", argv[0]);
        return 2;
    }
    const int ngl = argc > 4 ? std::atoi(argv[4]) : 0;
    const int threads = argc > 5 ? std::atoi(argv[5]) : 16;
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

    Dumper dumper;
    dumper.dir = argv[3];
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = ngl;
    llama_model* model = llama_model_load_from_file(argv[1], mp);
    if (model == nullptr) return 1;
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t) (n + 16);
    cp.n_batch = (uint32_t) n;
    cp.n_ubatch = (uint32_t) (n < 512 ? n : 512);
    // [cache type] [fa]: as flint_oracle (f16 / f32 / q8_0 / q4_0; auto / on / off)
    const std::string ct = argc > 6 ? argv[6] : "f16", fa = argc > 7 ? argv[7] : "auto";
    cp.type_k = ct == "f32" ? GGML_TYPE_F32 : ct == "q8_0" ? GGML_TYPE_Q8_0 : ct == "q4_0" ? GGML_TYPE_Q4_0 : GGML_TYPE_F16;
    cp.type_v = cp.type_k == GGML_TYPE_F32 ? GGML_TYPE_F32 : GGML_TYPE_F16;
    cp.flash_attn_type = fa == "off" ? LLAMA_FLASH_ATTN_TYPE_DISABLED
                         : fa == "on" ? LLAMA_FLASH_ATTN_TYPE_ENABLED
                                      : LLAMA_FLASH_ATTN_TYPE_AUTO;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    cp.cb_eval = cb;
    cp.cb_eval_user_data = &dumper;
    llama_context* ctx = llama_init_from_model(model, cp);
    if (ctx == nullptr) return 1;
    llama_batch b = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; ++i) {
        b.token[i] = toks[(size_t) i];
        b.pos[i] = i;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = 0;
        b.logits[i] = (int8_t) (i == n - 1);
    }
    b.n_tokens = n;
    if (llama_decode(ctx, b) != 0) return 1;
    std::fprintf(stderr, "seams: %d tokens, %d tensors written to %s\n", n, dumper.written, argv[3]);
    llama_batch_free(b);
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}
