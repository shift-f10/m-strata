// The official flint tokenizer and chat template: llama.cpp master (e86f5fd1)'s llama_chat_apply_template with the
// GGUF's own template, then llama_tokenize with special tokens parsed (vocab only, no weights).
// Usage: flint_tokenize <model.gguf> <user_text_file> <out_ids.txt> [raw|plain]   (raw: the file is the whole prompt
// text; plain: the file is ordinary text, tokenized with special tokens NOT parsed)
#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s model.gguf user_text_file out_ids.txt [raw]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[2], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    const bool plain = argc > 4 && std::strcmp(argv[4], "plain") == 0;
    const bool raw = plain || (argc > 4 && std::strcmp(argv[4], "raw") == 0);
    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.vocab_only = true;
    llama_model* model = llama_model_load_from_file(argv[1], mp);
    if (model == nullptr) return 1;
    const llama_vocab* vocab = llama_model_get_vocab(model);
    std::string prompt = text;
    if (!raw) {
        const char* tmpl = llama_model_chat_template(model, nullptr);
        if (tmpl == nullptr) {
            std::fprintf(stderr, "the model has no chat template\n");
            return 1;
        }
        llama_chat_message msg{"user", text.c_str()};
        std::vector<char> buf(text.size() * 2 + 4096);
        int n = llama_chat_apply_template(tmpl, &msg, 1, true, buf.data(), (int32_t) buf.size());
        if (n > (int) buf.size()) {
            buf.resize((size_t) n + 1);
            n = llama_chat_apply_template(tmpl, &msg, 1, true, buf.data(), (int32_t) buf.size());
        }
        if (n < 0) {
            std::fprintf(stderr, "llama_chat_apply_template failed (%d)\n", n);
            return 1;
        }
        prompt.assign(buf.data(), (size_t) n);
    }
    std::vector<llama_token> toks(prompt.size() + 16);
    const int n = llama_tokenize(vocab, prompt.c_str(), (int32_t) prompt.size(), toks.data(), (int32_t) toks.size(), false, !plain);
    if (n < 0) return 1;
    std::fprintf(stderr, "prompt (%zu bytes) -> %d tokens:\n%s\n", prompt.size(), n, prompt.c_str());
    std::FILE* f = std::fopen(argv[3], "w");
    if (f == nullptr) return 1;
    for (int i = 0; i < n; ++i) std::fprintf(f, i ? ",%d" : "%d", toks[(size_t) i]);
    std::fclose(f);
    llama_model_free(model);
    return 0;
}
