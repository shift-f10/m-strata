# flint's official-graph oracle (test-only)

Never built by the engine's CMake and never shipped. These programs link a llama.cpp checkout that has flint
(llama.cpp master `e86f5fd1e9f584b0fcec71d7da9a92de44e8bcbf`, MIT, kept outside this repository at
`G:\Strata-data\flint-llama-ref`, built CUDA sm_89 with shared libraries) and are the reference the flint engine is
checked against (design `docs/superpowers/specs/2026-10-27-flint-engine-design.md`, section 5).

- `flint_tokenize.cpp` - the GGUF's tokenizer (`raw`: the file is the whole prompt; else llama.cpp's built-in chat
  template, which differs from the GGUF's Jinja template by a newline after `<|user|>`).
- `flint_oracle.cpp` - the logits of the last N positions of a token file.
- `flint_seams.cpp` - every named tensor of one batch (`<name>-<layer>.bin`: int64 ne[4], then F32).
- `f_kl.py` - teacher-forced KL / top-1 / NLL of logit files against a reference.
- `f_seams_cmp.py` - multiengine-flint's seams (`STRATA_FLINT_SEAMS=<dir>`, the prompt path) against the oracle's.

Build (MSVC x64 prompt; R = the llama.cpp checkout):

```
cl /O2 /EHsc /std:c++17 /I"%R%\include" /I"%R%\ggml\include" flint_oracle.cpp /link "%R%\build\src\llama.lib" "%R%\build\ggml\src\ggml.lib" "%R%\build\ggml\src\ggml-base.lib"
```

The checks of 2026-10-27 (AGENT_HANDOFF entry l): a 25-token chat prompt plus 199 forced tokens, and a 1,511-token
prompt plus 63 forced tokens, on the owner's Q8_0 file, against `flint_oracle ... 0 12 f16 off` (CPU, f16 KV cache).
