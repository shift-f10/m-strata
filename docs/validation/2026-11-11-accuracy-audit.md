# Inference accuracy and embedded-template audit — 2026-11-11

Scope: the shipped multiengine launcher and frontend, expert selection, speculative
decoding, sampling controls, and optional use of model-supplied chat templates.
The read-only upstream checkout was not modified. This is a source and regression
test audit; it does not establish real-model quality, GPU logit parity, or Windows
runtime confinement.

## Findings and corrections

Owner follow-up: retain the speed options as explicit card choices, warning about accuracy
tradeoffs. The corrections below now describe the **accuracy defaults**, not permanent removal
of the upstream features. Model-specific Speed improvements controls can override the audited
runtime settings; each has bounded typed values, explanatory help and a reset to accuracy defaults.
Arbitrary per-layer maps and unsafe free-form router rank expressions are not card controls.

| Finding | Evidence in the engine tree | Correction |
| --- | --- | --- |
| A default tail-skip path could omit selected experts on cache misses. | `engine/src/program/generate.cpp` sets `ROUTE_TAIL_SKIP`; `engine/src/core/expert_source.cpp` zeroes skipped contributions without renormalizing. | Require `ROUTE_TAIL_SKIP=0`. |
| Optional resident/prior routing could replace the router's chosen experts. | `engine/src/core/verify.cpp`, `ROUTE_PRIOR` and `ROUTE_RESIDENT`. | Require both off, including resident MTP routing. |
| GLM trunk, disk and miss controls could drop experts; deferred CPU work can leave attention without current contributions. | `engine/third_party/maya/src/core/glm_fast_path.cu`, `TRUNK_KEEP`, `MISS_SKIP`, `GLM_DEFER_CPU`. | Keep all eight selected GLM experts, disable skips and lossy deferral, clear per-layer keep overrides. |
| GLM MTP could keep only two missing draft experts. | The MTP branch of `glm_fast_path.cu`. | Keep the full draft expert set; execute missing experts. |
| DeepSeek disk/RAM thresholds could omit experts; bounded YOCO replay changes results. | `engine/third_party/maya/src/ds4/ds4_model.cu`, disk/RAM skip thresholds and YOCO bounded replay. | Disable both skip thresholds; retain exact dependency-based YOCO and disable bounded replay. |
| A fast path could requantize BF16 weights to int8. | `engine/src/program/generate.cpp`, `HC_REQ8`. | Disable automatic weight requantization. |
| Sampled-draft confidence filtering conditions the proposal distribution, but the verifier uses the unconditioned distribution. | Qwen `STRATA_SPEC_PROB` and DeepSeek `DS4_DSPARK_SAMPLE` paths. | Disable these sampled-draft modes; retain verified exact-match/coupled speculative paths. |
| Explicit temperature zero and top-p one were omitted from the engine wire command. Other sampler keys can activate Maya sampling or leave its inherited nucleus cutoff active. | `secure_frontend/src/engine_protocol_client.cpp`, `render_sampling_keys`. | Serialize explicitly supplied zero/one values. |
| Direct launcher top-k values outside the engine's supported range were silently limited to 64. | Native sampler candidate capacity and launch arguments. | Reject unsupported direct `--top-k` values instead of silently changing the requested sampler. |
| Card controls made decimal adjustment awkward, and the chat drawer could round an untouched value while applying another setting. | `secure_frontend/web/app.js`, `numberControl` and `applySettings`; temperature drawer stopped at 1.5. | Add synchronized decimal sliders/exact numeric entry; extend temperature to 2; only change a drawer value when that slider moved. |
| CUDA compiler fast-math covered floating-point routing, attention, normalization, RoPE, activation and quantization work. | Native CUDA source options in engine/Maya, MMQ targets, Gemma and raon1 source/target options. | Owned CMake overlays remove both fast-math spellings after all targets exist, without editing pristine sources. |

The shared `secure_frontend/include/secure_frontend/engine_accuracy_policy.hpp`
applies accuracy defaults followed by explicit bounded `engine.speed_settings`, after legacy
frontend environment rows and again in `engine/src/multiengine/launcher.cpp` before loading family
libraries. Both `STRATA_` and `MAYA_` aliases are set consistently where applicable; malformed
settings or failure to apply them refuse launch. The frontend sends a closed
`MSTRATA_SPEED_SETTINGS` channel rather than forwarding ambient environment variables.
Caching, prefetch, CPU/GPU placement and exact reuse remain available independently.

The owner explicitly retained the existing int8/q8 KV-cache defaults. Those
defaults and existing saved precision choices are unchanged. Cache quantization
is an accepted memory/precision tradeoff here, not a claim of identical logits;
the option descriptions now say that lower precision can change results.

## CUDA compiler accuracy

The fast-math opt-in is not limited to integer matrix multiplication. Affected
sources include `native_router.cu` and `native_flash_attn.cu` (`expf`),
`native_gdn_preprocess.cu` (`expf`, division, `rsqrtf`, `log1pf`),
`native_rope.cu` (`powf` and trigonometry), and ggml's MMQ quantization helpers
(floating scale division and rounding). The existing `mrope.hpp` comments also
describe fast-math angle error at long contexts. The
[NVCC manual](https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/)
specifies that `--use_fast_math` enables flush-to-zero, approximate division and
square root, and FMA; the
[CUDA best-practices guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/)
explains the substitution of fast intrinsic implementations for math calls.

`engine/cmake/multiengine_accuracy.cmake` removes `--use_fast_math` and
`-use_fast_math` from target and directory-scoped source compile options by default,
including generator expressions. The root and separate Maya overlays both run
it after creating their targets. Shared upstream executable targets receive the
same removal as the family libraries. No pristine engine/vendor/toolchain source
is changed by this correction. Maya's external configure step also tracks both
overlay inputs explicitly, alongside its nested build regeneration dependencies.
The policy does not disable normal FMA or rewrite
source-explicit intrinsics such as `__expf` and `__fdividef`; those are remaining
numerical implementation choices, not a guarantee of exact reference logits.
External compiler flags supplied manually outside these overlays are not covered.
Existing oracle tolerances may reflect upstream's fast-math baseline; numerical
comparisons on the owner's GPU remain necessary after recompiling all families.
The owner subsequently requested both variants in the default package. Packaging builds separate
`MULTIENGINE_FAST_MATH=OFF` and `ON` trees, checks their engine/Maya caches, and records file hashes
in `engine/variants.json`. The card's CUDA fast math switch selects the corresponding executable
and matching libraries on the next model start. This selects precompiled code rather than changing
compiler behavior inside a running library. Accuracy remains the default; INT8/Q8 is unchanged.

## Embedded template boundary

The existing fixed frontend renderers remain the default. Embedded templates are
an explicit per-card selection after reviewing the source. The review reads
`tokenizer.chat_template` from the GGUF or the extracted tokenizer-pack template,
within the validated model root. Source must be valid UTF-8 without NUL, no more
than 256 KiB, and within the line-count bound. Its SHA-256 identifies the exact
reviewed source.

The page displays source through text nodes, with line numbers, highlighted risk
locations, and explanatory findings. A heuristic scan is not a safety or
correctness guarantee. Selecting an embedded template requires explicit
acknowledgement of the reviewed hash, even when warnings remain. Switching cards
clears the review and ignores stale responses. Saving applies at the next model
start; changed source requires another review. No custom-template editor is added.
Directional controls are made visible in the source display. A separate frontend
restore action remains usable if the embedded source cannot be read, and a
changed-source response explicitly asks for another review.

Rendering uses a native value-only Jinja helper, not Python Jinja or host-object
evaluation. No filesystem, command, import, or network functions are registered.
The helper is a separate process with input/output bounds, memory and CPU limits,
and a parent wall-clock deadline. Linux additionally uses a syscall allowlist;
Windows uses process/job resource limits. Windows resource containment is not an
OS filesystem/network sandbox and must not be described as one. The language
surface is the primary host-access boundary there.

Input provenance must survive template operations so user/tool strings cannot
become special control tokens merely by being rendered. The audit identified
array/object string coercion as a bypass of the first transform guard; regression
coverage must include direct concatenation and implicit coercion, as well as
ordinary string filters. Templates can still intentionally change or omit prompt
content; host isolation cannot guarantee that a model-supplied prompt is truthful
or appropriate.

## Verification record

### Owner-requested opt-in speed controls follow-up

- Source audit maps Qwen routing to the supported 512-expert/top-10 path, tail skips to
  Qwen/Gemma/flint, GLM skip/deferral knobs to GLM and raon1, and replay/DSpark controls
  to DeepSeek. The page does not offer nonexistent controls for MiniMax. Help distinguishes
  target-output tradeoffs from draft-only changes that can affect acceptance or speed.
- RED evidence covered missing controls, explicit override support, card bounds excluding
  the accuracy reset, engine numeric spellings (`.5`, `01`, `04`), and replay beyond context.
  Shared list validation also prevents empty or semicolon-packed entries from being laundered
  into valid settings by option application. No arbitrary environment passthrough was added.
- Full top-level serial Linux suite: **1520 passed, 11 environment skips, zero failures**.
  Final shared-validation changes passed 26 model-option and 3 configuration/environment
  checks; optimized Release gates passed **30/30**, plus **30/30** speed/card/config checks.
- Policy/build/package Python tests: **42/42**. Node-backed card/template UI tests: **4/4**.
  Tests verify positive choices, unchanged unspecified defaults, both engine aliases, pre-DLL
  initialization, malformed-value refusal, persistence, restart and explicit reset behavior.
- Full Windows LLVM-MinGW cross-build passed, including final validation changes. Native
  Windows package built and its final source rebuild/layout check passed in
  `out/dist-speed-controls`; generated engine/Maya flags retain accuracy-default math.
  Fast-math ON is covered by generated-command tests that match upstream options exactly;
  no separate fast-math GPU binary or numerical benchmark is claimed.
- Published all 48 runtime files to `out/dist-tool-calling`; replaced files were backed up under
  `out/backup-before-speed-controls-*`. Full before/after SHA-256 checks confirmed all 373 data files
  unchanged. No running application was stopped.

### Initial accuracy/template implementation

- Decimal controls: behavioral RED showed no card slider. Drawer RED showed
  `0.375` rounded to `0.38` on Apply. Narrowed card bounds RED showed slider minimum
  above maximum. GREEN: two Node-backed Python tests exercise drag updates, exact
  saved/default values, open-zero top-p, narrowed bounds, integer/auto controls,
  and preservation of unrelated values.
- Template review UI: RED before the review controls existed. GREEN: one
  Node-backed Python test covers literal source display, multi-line highlighting,
  required acknowledgement/hash, approval despite warnings, frontend reset,
  unavailable templates, frontend recovery after a failed inspection,
  changed-source warnings, visible directional controls, and stale responses
  after switching cards.
- JavaScript syntax check passed. The behavioral harness tests real control
  functions with DOM stubs; it is not a browser rendering proof.
- CUDA compile policy: two RED tests showed fast-math in generated commands and
  missing production overlay wiring. A further RED case caught trailing
  whitespace changing a retained compiler argument, and a dependency RED case
  covered Maya's external configure inputs. GREEN: three WSL tests
  generate root/child source commands with scoped source options, quoted and
  list-valued generator expressions, repeated policy application, and preserved
  lambda/FMA/warning/definition options, and check overlay/configure wiring. The
  command fixture uses a host C++ compiler to inspect generated commands; it does
  not compile or execute CUDA kernels.
- Combined top-level Linux build and serial CTest: **1518 passed, 11 environment
  skips, zero failures** (1529 registered cases selected, release-only excluded).
  This includes supervisor/protocol, client tools, template approval/extraction,
  and startup/hash/error-path integration. Final interpreter portability changes
  were rebuilt and the 16 template/helper integration cases passed again.
- The full hooks-OFF Release build ran 1487 cases: 11 environment skips and two
  global socket-observation failures under parallel fixtures. Both passed in a
  serial isolated rerun; all 1476 runnable cases passed across these runs.
  Earlier debug failures exposed stale documentation/wire expectations and the
  host-dependent systemd test; these were corrected without relaxing assertions.
  A hot-model startup timing failure passed in isolation and in the full serial run.
- Python full suite: **183 passed, 9 skipped** in WSL. The subsequently added CUDA
  overlay tests passed **3/3**. Node-backed fractional-control and template-review
  UI tests passed **3/3** on the host (they skip when Node is unavailable in WSL).
- Full LLVM-MinGW Windows cross-build passed with warnings as errors, including
  test executables; the final interpreter portability edits were rebuilt too.
- Native Windows CUDA package completed through `make.bat`, including every family,
  frontend, window/search programs, native template helper and interpreter license.
  Frontend MSVC compilation uses `/W4 /WX`; portability warnings were fixed without
  suppressions. Existing copied CUDA targets retain their documented vendor warning
  policy, including diagnostic 221-D for the negative-infinity sentinel conversion;
  this is not a claim that all vendor CUDA compilation uses warnings as errors.
  Both generated engine and Maya Ninja files contain no `use_fast_math` flags.
- Runtime output was built in `out/dist-accuracy-templates` and all 48 runtime
  files published to the existing `out/dist-tool-calling` installation. Replaced
  files were backed up under `out/backup-before-accuracy-*`; before/after SHA-256
  checks confirmed all 373 existing data files unchanged. No running app was stopped.
  No Windows runtime or GPU numerical acceptance is claimed by these build gates.

No real-model output comparison or GPU routing trace was produced by this audit.
Final acceptance should compare greedy output/logits and sampled behavior with
the same weights, cache precision, sampler, prompt and seed, including cache
misses and low-residency cases. A build or a fixture test alone cannot establish
that all backend numerical paths match a reference model.

## Subsequent dual-variant package

The owner approved bundling both compiler modes and choosing them per card. This supersedes
the earlier build-only opt-in and single-variant package descriptions. The native package
`out/dist-dual-math` builds both modes independently and includes complete dependencies.
All 58 manifest records were independently checked against installed sizes and SHA-256;
all seven family DLLs differ between modes. Generated flags contain no compiler fast math
for the accuracy variant, and 72 outer / 33 Maya flag lines for the fast variant.

Final tests: full Linux **1527 passed / 11 environment skips / 0 failures**; Release safety
gates **30/30** plus variant/card/config **35/35**; Python policy/package **61/61** plus
the subsequently added Linux SONAME materialization test; host UI **4/4**. Linux, Release,
LLVM-MinGW cross and native CUDA/frontend builds passed. Variant tests cover default-OFF,
selection and library order, switching back, missing/tampered bytes, malformed manifests,
and link/path rejection. This remains compilation and synthetic-fixture evidence, not a
GPU numerical or performance comparison.

Published 78 runtime files to `out/dist-tool-calling`, with previous binaries backed up under
`out/backup-before-dual-math-*`. All 373 existing data files matched their before/after SHA-256.
