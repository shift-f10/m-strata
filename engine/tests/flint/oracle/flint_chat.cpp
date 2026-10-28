// The official flint chat prompt: llama.cpp master (e86f5fd1)'s common chat templates (its Jinja engine, as
// llama-server --jinja runs it) over the GGUF's own template, then llama_tokenize with special tokens parsed, as the
// server tokenizes a rendered prompt (vocab only, no weights).
// Usage: flint_chat <model.gguf> <conversation.txt> <out_prompt.txt> <out_ids.txt> [no-gen]
// The conversation file holds turns, each a line "@@system", "@@user" or "@@assistant" followed by its content up to
// the next marker line (the newline before a marker is not content). no-gen: no generation prompt.
#include "chat.h"
#include "llama.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s model.gguf conversation.txt out_prompt.txt out_ids.txt [no-gen]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[2], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<common_chat_msg> messages;
    size_t at = 0;
    while (at < text.size()) {
        if (text.compare(at, 2, "@@") != 0) {
            std::fprintf(stderr, "a turn must start with an @@role line\n");
            return 2;
        }
        const size_t eol = text.find('\n', at);
        if (eol == std::string::npos) {
            std::fprintf(stderr, "a role line must end in a newline\n");
            return 2;
        }
        common_chat_msg msg;
        msg.role = text.substr(at + 2, eol - at - 2);
        size_t next = text.find("\n@@", eol);
        const size_t end = next == std::string::npos ? text.size() : next;
        msg.content = text.substr(eol + 1, end - eol - 1);
        messages.push_back(msg);
        at = next == std::string::npos ? text.size() : next + 1;
    }
    const bool gen = !(argc > 5 && std::strcmp(argv[5], "no-gen") == 0);

    llama_backend_init();
    llama_model_params mp = llama_model_default_params();
    mp.vocab_only = true;
    llama_model* model = llama_model_load_from_file(argv[1], mp);
    if (model == nullptr) return 1;
    const llama_vocab* vocab = llama_model_get_vocab(model);
    auto tmpls = common_chat_templates_init(model, "");
    common_chat_templates_inputs inputs;
    inputs.messages = messages;
    inputs.add_generation_prompt = gen;
    inputs.use_jinja = true;
    const common_chat_params params = common_chat_templates_apply(tmpls.get(), inputs);
    const std::string prompt = params.prompt.str();

    std::vector<llama_token> toks(prompt.size() + 16);
    const int n = llama_tokenize(vocab, prompt.c_str(), (int32_t) prompt.size(), toks.data(), (int32_t) toks.size(), false, true);
    if (n < 0) return 1;
    std::FILE* p = std::fopen(argv[3], "wb");
    if (p == nullptr) return 1;
    std::fwrite(prompt.data(), 1, prompt.size(), p);
    std::fclose(p);
    std::FILE* f = std::fopen(argv[4], "w");
    if (f == nullptr) return 1;
    for (int i = 0; i < n; ++i) std::fprintf(f, i ? ",%d" : "%d", toks[(size_t) i]);
    std::fclose(f);
    std::fprintf(stderr, "format %s, %zu prompt bytes -> %d tokens\n", common_chat_format_name(params.format), prompt.size(), n);
    llama_model_free(model);
    return 0;
}
