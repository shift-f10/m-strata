# Native Jinja interpreter provenance

Source: https://github.com/ggml-org/llama.cpp/tree/3cf03257f219afbe7334045ff7c6a06ac68c627d

The files in `jinja/` come from `common/jinja/` at that exact commit;
`unicode.cpp` and `unicode.h` come from `common/`. `LICENSE` is the upstream
MIT license. `UPSTREAM.sha256` records the original git object bytes, verified
directly against that commit before the local adaptations listed here. Local
checkout line endings may differ. No runtime download is needed.

Local adaptations:

- Removed the `common_json` conversion dependency. The confined helper uses
  the frontend's bounded JSON parser and explicitly marks untrusted strings.
- Kept input-origin marking through string/JSON transforms and array/object
  coercion, character indexing and dynamic date formats. Ambiguous mixed
  results are entirely untrusted; macro output keeps
  its marked segments. This prevents request text from becoming control tokens.
- Made unused header utility functions inline for the project's strict
  warnings-as-errors compilation; warnings are not suppressed.
- Native MSVC portability: explicit UTF-8 byte and character-case conversions
  with unsigned input to character classification, non-shadowing local names,
  `localtime_s`/`localtime_r`, and validation of the full 64-bit integer base
  before narrowing to the C++ conversion API's `int` argument.

The application wrapper is separate: `src/template_main.cpp` renders in a
fresh process with mandatory time/memory limits and no registered host access.
Linux additionally installs a syscall allowlist before parsing any request.
Windows applies a nested resource-limiting Job Object; this is not an
AppContainer boundary against native interpreter memory-corruption exploits.
Errors never return template source, request content or interpreter exception
details. The installed helper's license is `licenses/llama-jinja/LICENSE`.
