# C4 compiled/build read: Niko1221/Strata v0.2.1.3 -> v0.2.2

- **Range:** `v0.2.1.3` (tag object `1ab44ace2e4db985503ab9388b65405a63600dd7`, commit
  `cbfd5b926b794b356195d3cb600191b0c3567540`) -> `v0.2.2` (tag object
  `e6fff4ad12908de74952abe53b2bade45a68aa0f`, commit `011c9e600a93bbc9fd451c563501b5831c32bbca`). Both tags unsigned.
- **Command read, in full, file by file:**
  `git -C C:\multi-strata\upstream diff v0.2.1.3 v0.2.2 -- src include cmake CMakeLists.txt third_party`
- **Size:** 33 files, +2991 / -110 (`--numstat`), 4384 diff lines. `cmake/` and `third_party/` have no changes in this range.
- **Reviewer:** agent read on 2026-10-21. Under C9 this counts as the reviewer's read, not the owner's sign-off. Only
  read-only git commands and file reads were used. Nothing from either tree was configured, built or run for this read.
- **Method:** every hunk of every file was read. Where a hunk's meaning depended on context, the surrounding v0.2.2 source
  was read with `git show v0.2.2:<path>`. That context settled: `#if` platform gating (`generate.cpp`,
  `expert_source.cpp`, `gemm.cu`), whether a line runs on the serve path or only the CLI path (`generate.cpp`: the serve
  block is lines 8236-15533 and returns), where diagnostics are written (`stall_report`/`diag` write to `stderr`), and
  the scope of the CMake blocks. The added lines were also checked for non-ASCII bytes (there are none, so no hidden
  Unicode or bidi characters) and cross-checked against the C3 API patterns.
- **Reachability context assumed:** the Windows CUDA build (`strata.exe`, `strata_kq.dll`), the engine in `--serve`
  mode, and the closed environment block from `secure_frontend/src/engine_environment.cpp`. The engine's stderr goes to
  `NUL` (`secure_frontend/src/engine_stdio_process.cpp:204`), and the engine's stdout is the line protocol.
- **Result:** 0 concerns, 12 notes, 21 clean.

## Per-file verdicts

| File | + / - | What the change does | Verdict |
| --- | --- | --- | --- |
| `CMakeLists.txt` | 24 / 3 | Version 0.1.46.2 -> 0.1.47. Adds the executables and `add_test`s `tq_pascal_bench`, `uma_cache_test`, `spec_start_test`, `pool_cores_test` and `numa_pool_test`, and `SKIP_RETURN_CODE 77` on two tests. No commands, fetches or flag changes. | note (N13) |
| `include/strata/core/expert_source.hpp` | 14 / 0 | Declares `numa_slice_ranges()`, `set_extra_drop_files()` and its member. | clean |
| `include/strata/core/uma_cache.hpp` | 34 / 0 | New header: the desktop RAM reserve for an integrated GPU's auto expert cache, from env `STRATA_UMA_RESERVE_GIB`. | note (N9) |
| `include/strata/core/verify.hpp` | 1 / 0 | Adds `launched_[9]` and `launched_nr_[9]` flags ("first launch of this graph"). | clean |
| `include/strata/kernels/cpu/numa_pool.hpp` | 152 / 0 | New header of pure NUMA decisions: counts the nodes in a sysfs folder (reads `cpulist`) and decides whether the CPU pool splits per node. | note (N3, N4) |
| `include/strata/kernels/cpu/pool.hpp` | 83 / 0 | Declarations: `set_pool_cores`, `logical_cpu_layout`, `NumaSplitPlan`, and the per-slice `NodeSync` claim counters. | clean |
| `include/strata/kernels/cpu/pool_plan.hpp` | 211 / 1 | `span_packages` option, the `--pool-cores` parser (`parse_pool_cores`), and helpers for checking, formatting and describing the CPU list. | note (N8) |
| `include/strata/platform/aux_cpus.hpp` | 1 / 0 | Includes `thread_name.hpp`. | clean |
| `include/strata/platform/thread_name.hpp` | 42 / 0 | New header: `name_this_thread` (Linux and macOS use `pthread_setname_np`; the Windows half is in `pool.cpp`). | clean (see N2) |
| `include/strata/prefill/gemm.hpp` | 6 / 0 | Declares `unpad_x` and its buffer members. | clean |
| `include/strata/spec/spec_start.hpp` | 20 / 0 | New `kSpecNotStarted = -1` sentinel and the `spec_loop_runs()` predicate. | clean |
| `src/core/expert_source.cpp` | 292 / 3 | Linux NUMA placement of the cache complement: pinned toucher threads, a `move_pages` query and `MADV_NOHUGEPAGE`. Re-opens the pack and MTP files read-only for `fadvise(DONTNEED)`. Names threads. | note (N4, N6, N7) |
| `src/core/session.cpp` | 1 / 0 | Names the host thread `strata-host`. | clean |
| `src/core/uma_cache_test.cpp` | 32 / 0 | Unit test for `uma_cache.hpp`. Pure arithmetic. | clean |
| `src/core/verify.cpp` | 38 / 2 | `early_ple_flag_mode()` (env `STRATA_VERIFY_EARLY_PLE_FLAG`): on the first launch of each window graph, raises the host PLE flag before the launch. The default is 0 (unchanged) on CUDA and 2 on HIP. | note (N9) |
| `src/kernels/cpu/native_hadamard_test.cpp` | 1 / 1 | Returns 77 (skip) instead of 1 when ggml has no trellis types. | clean |
| `src/kernels/cpu/numa_pool_test.cpp` | 146 / 0 | New test: builds a fake sysfs node tree under the system temp directory, then removes it. | note (N14) |
| `src/kernels/cpu/pool.cpp` | 455 / 37 | Logical-CPU layout, `--pool-cores` placement, NUMA auto split (Linux, two or more nodes) with the per-slice claim protocol, the start-up placement line, and the Windows `SetThreadDescription` lookup. Adds the env reads `STRATA_TEST_SYSFS_ROOT`, `STRATA_NUMA_POOL`, `STRATA_NUMA_SPLIT` and `STRATA_EXCHANGE_ROTATE`. | note (N1, N3, N4, N5, N9) |
| `src/kernels/cpu/pool_cores_test.cpp` | 123 / 0 | New test: builds real pools and checks where the workers are placed. Pins only its own threads. | clean |
| `src/kernels/cpu/pool_plan_test.cpp` | 120 / 0 | Unit tests for `span_packages`, interleave, `parse_pool_cores`, `check_pool_cores` and the placement line. | clean |
| `src/kernels/cpu/pool_tasks_test.cpp` | 283 / 2 | NUMA-split parity tests. Sets and unsets its own environment. On Linux it writes a fake sysfs tree under temp and points `STRATA_TEST_SYSFS_ROOT` at it. | note (N14) |
| `src/kernels/cpu/tq_cpu_parity.cpp` | 2 / 2 | Returns 77 (skip) instead of 1 when ggml has no trellis types. | clean |
| `src/kernels/cuda/iq_kernels.cu` | 323 / 39 | Gyro trellis kernels (ggml 144/145/146): the codebook is staged in shared memory, 8-lane gate/up and down rows, and the row-chunk loops run on cc 6.0. Adds `__ldg` loads on cc < 6.1 and the env reads `STRATA_TQ_STAGE`, `STRATA_TQ_SUB8` and `STRATA_TQ_ROWCH`. | note (N10) |
| `src/kernels/native_expert_bench.cpp` | 4 / 0 | The bench prints an FNV-1a hash of its output to stdout (bench binary only). | clean |
| `src/kernels/tq_pascal_bench.cpp` | 306 / 0 | New GPU bench/test on synthetic random blocks: compares the old and new kernels bitwise and prints checksums. No file or network I/O. | clean (build scope: N13) |
| `src/prefill/gemm.cu` | 39 / 4 | A padded activation that no kernel takes is now copied unpadded on the device and the GEMM is re-run, instead of `exit(1)`. The padded path is gated on `strata_pf_padded_gemm_available()`. | note (N12) |
| `src/prefill/prefill.cpp` | 13 / 1 | `pf_pad()` refuses padding when the device has no padded product. Names three helper threads. | clean |
| `src/prefill/wmma_gemm.cu` | 5 / 0 | `strata_pf_padded_gemm_available()`: the gfx11 check, or `false` when built without gfx11. | clean |
| `src/prefill/wmma_gemm.h` | 8 / 0 | Declares it for HIP; an inline `false` for CUDA. | clean |
| `src/program/generate.cpp` | 138 / 14 | Adds the `--pool-cores`/`STRATA_POOL_CORES` flag, the placement line, the UMA cache reserve (HIP Linux only), the `GPU_MAX_HW_QUEUES` check (HIP Linux only), the pack/MTP drop list, the #3465 batched-retry change, the `spec_pos` sentinel, the `STRATA_SPEC_WATCH` watchdog (CLI only), and thread names. | note (N6, N8, N9, N11) |
| `src/qwen35/q36_engine.cu` | 4 / 1 | Debug (`VDEBUG`) stderr print: the host-side half-to-bits conversion now uses `memcpy`. | clean |
| `src/spec/spec_start_test.cpp` | 22 / 0 | Unit test for `spec_loop_runs()`. | clean |
| `src/spec/suffix_drafter_test.cpp` | 48 / 0 | Randomized soundness test of the suffix drafter. The xorshift seed is the hex digits of pi. In-memory only. | clean |

## Concerns

None. No change in this range was judged a concern. Specifically, none of the following was found in the added or
changed lines:

- network, socket, DNS, HTTP, named-pipe or other IPC use;
- new stdout records on the serve path. The serve block has no new stdout writes; the only new stdout text is on the
  CLI `generate --spec` stats line (N11);
- process creation or shell execution;
- runtime code loading beyond the one `GetProcAddress` (N1), JIT, or writable-executable memory;
- file writes outside the system temp directory (test binaries only, N14);
- registry, autostart or scheduled-task access;
- privilege or token changes, or process-wide or system-wide priority, power, page-file or other settings changes;
- behaviour gated on time, date, host name or user name;
- encoded strings, XOR loops, or constant tables that are not plausible kernel or hash constants. The only new constants
  are the FNV-1a offset and prime, a pi-digits test seed, and fp16 bit patterns;
- build-time commands or downloads, or compiler and linker flag changes.

Where something is ambiguous rather than cleared, it is said so in the notes (N5, N6, N10).

## Notes

**N1 — Windows dynamic function lookup.** `src/kernels/cpu/pool.cpp:1455-1467` (the `GetProcAddress` call is on line
1460).
- `GetModuleHandleW(L"kernel32.dll")` only returns the already-mapped module; it does not load a library.
- Only `SetThreadDescription` is resolved. It is called once per thread on `GetCurrentThread()`.
- The name comes from `snprintf` of a compile-time constant base and an `int` index. It is widened into `wchar_t[64]`
  with a 63-character bound.
- The pointer is cached in a function-local static.
- No other dynamic loading is added. This is C3 finding `bf668f29e17cc445`. For C5, the string
  `SetThreadDescription` will appear in the binary, and `kernel32` is already imported.

**N2 — Thread names.** `include/strata/platform/thread_name.hpp:21-38` (POSIX) and N1 (Windows).
- About 17 call sites, all with constant names: `strata-pool-N`, `strata-host`, `strata-io`, `strata-lookahead`,
  `strata-ram`, `strata-fill`, `strata-hand`, `strata-stage`, `strata-issue`, `strata-adapt`, `strata-adapt-job`,
  `strata-ckpt-N`, `strata-stdin` and `strata-watchdog`.
- Other local processes can see the names (Process Explorer, `/proc/<pid>/task/*/comm`). They carry no data.

**N3 — New default: the NUMA split now turns itself on (Linux, two or more NUMA nodes).** `src/kernels/cpu/pool.cpp:743-925`
and `include/strata/kernels/cpu/numa_pool.hpp:71-85`.
- Until v0.2.1.3 the split was opt-in (`STRATA_NUMA_SPLIT=1`). Now, with neither `STRATA_NUMA_SPLIT` nor
  `STRATA_NUMA_POOL` set on a two-node Linux host, the pool does three things:
  - interleaves its worker cores over the nodes;
  - lets the default worker count span both sockets;
  - places the cache complement per node, which forces `drop_file_cache()` and the paced pin path. That overrides
    `STRATA_PIN_GUARD=0` (`src/core/expert_source.cpp:3746-3761`).
- `STRATA_NUMA_POOL=0|off|false|no` is the off switch.
- With one node, the path is unchanged (`Unchanged`, silent).
- Windows: `numa_node_count()` uses `GetNumaHighestNodeNumber` (`pool.cpp:621`). On a multi-node Windows machine the
  automatic attempt is refused ("the split needs Linux"), the topology is restored, and one stderr line is printed.
- The effect is on behaviour and performance, not security. However, the frontend's closed environment cannot pass
  `STRATA_NUMA_POOL` today, so on a multi-socket Linux host the frontend cannot turn the automatic split off.

**N4 — New read-only OS queries.**
- Linux:
  - `/sys/devices/system/cpu/cpuN/topology/{physical_package_id,core_id}` (`pool.cpp:91-137`, `logical_cpu_layout`);
  - `<sysfs>/devices/system/node/nodeN/cpulist` (`numa_pool.hpp:40-53`);
  - `access()` probes of `<sysfs>/devices/system/cpu/cpuN/nodeK` (`pool.cpp:602-611`);
  - `/sys/devices/system/node/nodeN/meminfo` (`expert_source.cpp:542-560`).
- Windows: `GetLogicalProcessorInformationEx`, `GetActiveProcessorGroupCount` and `GetProcessAffinityMask`
  (`pool.cpp:91-114`).
- All of these are queries; none of them writes.

**N5 — A test hook env variable in production code: `STRATA_TEST_SYSFS_ROOT`.** `src/kernels/cpu/pool.cpp:596-600`
(Linux only).
- It replaces `/sys` as the root for NUMA node discovery: the `cpulist` reads and the `access()` probes.
- It only reads and stats paths under the given directory. Its only effect is to change the NUMA decision, and so the
  thread pinning and the first-touch memory placement.
- It is not in the frontend's allowlist.
- Ambiguity: this is a test-only path redirect compiled into the release binary. It is harmless as written, but it is
  an env-driven path input. Keep it out of any future allowlist.

**N6 — Re-opening the pack and MTP files to drop their page cache.**
- `src/program/generate.cpp:8074-8084` runs on all platforms. It collects `o.pack` and `o.mtp`: either the path itself
  when it is a regular file, or the regular files of that directory (not recursive).
- `src/core/expert_source.cpp:3702-3721` is Linux-only. When the drop runs, it opens each path with
  `O_RDONLY|O_CLOEXEC`, calls `posix_fadvise(..., POSIX_FADV_DONTNEED)` and closes it.
- No writes. The paths are documented inputs only, although `is_regular_file` follows symlinks.
- Side effect: the system-wide page cache of those files is dropped (performance only).
- On Windows the list is built (a directory listing) but never used.
- Robustness, not security: the range-for over `directory_iterator(d, ec)` advances with the throwing `operator++`, so
  an I/O error part-way through the listing would throw at start-up.

**N7 — Thread pinning and memory-placement APIs (Linux).**
- `src/core/expert_source.cpp:620-650` spawns two threads per paced step (about 1 GiB each). Each thread pins itself
  with `sched_setaffinity(0, ...)`. That call is thread-scoped, and the cores are ones the pool had already chosen.
  The threads then touch one byte per page of the engine's own arena.
- `madvise(MADV_NOHUGEPAGE)` applies only to the engine's own `posix_memalign` arena (line 3826).
- `syscall(SYS_move_pages, 0, cnt, pages, nullptr, status, 0)` (line 682) passes a null node list, so it only queries
  where this process's pages are. It migrates nothing.
- There are no `mbind`, `set_mempolicy` or process-wide affinity, priority or system-setting changes.
- The pool workers keep the existing `pin_this_thread`. It now prints a stderr line when pinning fails under the split
  (`pool.cpp:930`).

**N8 — New flag and env variable: `--pool-cores LIST` / `STRATA_POOL_CORES`** (`generate.cpp:2512`, `2862-2873`).
- The list is parsed by `parse_pool_cores` (`pool_plan.hpp:183-226`). Each number is capped at 100000 and the list at
  4096 entries.
- Duplicates are removed by a linear search per added entry. A very long string of repeated wide ranges could therefore
  cost noticeable CPU time at start-up. This is bounded and controlled by argv or env, both of which the frontend owns.
- On a parse error, the raw text is echoed to stderr. The frontend sends stderr to `NUL`, so this is no injection path.
- `check_pool_cores` drops processors outside the process's layout. The result is used only for thread pinning.
- Not checked: whether the frontend's user-arguments passthrough can carry this flag to the engine.

**N9 — Other new env reads.** All of them tune performance or diagnostics. None changes a file path or enables writes or
execution, and none is in the frontend's allowlist.
- `STRATA_NUMA_POOL`, `STRATA_NUMA_SPLIT` (`pool.cpp:753-755`). An unknown value is echoed to stderr.
- `STRATA_EXCHANGE_ROTATE` (`pool.cpp:633`).
- `STRATA_VERIFY_EARLY_PLE_FLAG` (`verify.cpp:280-290`). On CUDA the default is 0, which keeps the old order; on HIP it
  is 2.
- `STRATA_TQ_STAGE`, `STRATA_TQ_SUB8`, `STRATA_TQ_ROWCH` (`iq_kernels.cu:3310-3356`). `ROWCH` is clamped to 1..32.
- `STRATA_UMA_RESERVE_GIB` (`uma_cache.hpp:18-27`). It is used only under `STRATA_USE_HIP && !_WIN32`. A huge value
  overflows the `double` -> `uint64_t` cast, which is undefined behaviour, but it is reachable through env only.
- `GPU_MAX_HW_QUEUES` (`generate.cpp:2261`). HIP Linux only; the value is echoed to stderr.
- `STRATA_SPEC_WATCH` (`generate.cpp:16229-16251`). It applies only on the CLI `generate --spec` path, after the serve
  block, not in serve mode. It starts a detached watchdog that writes a stall report to stderr and calls `std::abort()`
  after N seconds without progress.

**N10 — Kernel changes** (`src/kernels/cuda/iq_kernels.cu`).
- There is a new default route for the Gyro trellis types (ggml 144/145/146) on every CUDA card with compute capability
  6.1 or higher (`tq_sub8_on`, line 3334). Cards below 6.1 (the Tesla P100) get the shared-memory staging instead
  (`tq_stage_on`, line 3310).
- The int8 codebook is copied from the existing `tq_lut_i8` table into shared memory (line 1752): 1024 words into a
  buffer `IqGridWords` sizes at 1024 words.
- Staged activations go into `s_x[GRP_NC*80*9]`, and only when `xb <= 80` (line 1969).
- The staged down rows go into `s_hq_buf[GRP_NC*20*9]`, with `n <= GRP_NC` (line 2142).
- The new row-chunk loops step by `gridDim.x`, so a grid larger or smaller than planned stays in bounds.
- No new constant tables were added.
- These paths are reached only for models that use these types.
- Checked: the shared-memory bounds. Not verified: the claimed bitwise equivalence of the arithmetic.
- `row_dot_160_sub8` assumes an activation stride of 80 blocks (`ent_tok[e] * 80`), but the launch checks only
  `nb * ipb == 160`. That is consistent for `n_embd` 2560 with 128-value blocks. The geometry comes from the model
  files, not from the protocol.

**N11 — Behaviour changes on the serve and CLI paths** (`src/program/generate.cpp`).
- Issue #3465 (lines 12396-12404 and 12911-12916): after non-finite logits in a prompt window, the next request reads
  its whole prompt through the batched path (`short_read` 0), once. The only output is on stderr.
- The `spec_pos` sentinel changes from 0 to -1 (line 15538, `spec_start.hpp`). This is on the CLI path only. Its one
  consumer (line 16185) runs only when `spec_loop_runs()` holds, which needs `spec_pos >= 0`. A one-token prompt on a
  native pack can now enter the speculative loop.
- The serve block (lines 8236-15533) gains no stdout writes. The only new stdout text is `"; numa slices: ..."` on the
  CLI `--spec` stats line (line 16450), printed only while the split is on.

**N12 — Prefill unpad fallback.**
- `src/prefill/gemm.cu:566-583` and `1283-1300` replace an `exit(1)` with a device-to-device `cudaMemcpy2DAsync` into a
  buffer that grows as needed, followed by a recursive `native(..., ldx=0)` call, which terminates.
- It is reachable only with `STRATA_PF_PAD=1` on HIP. On CUDA, `strata_pf_padded_gemm_available()` is an inline `false`
  (`wmma_gemm.h:47-51`), so `pf_pad()` is false (`prefill.cpp:468-482`) and nothing pads.

**N13 — Build system** (`CMakeLists.txt`).
- Only `add_executable`, `add_test` and `set_tests_properties` lines are added.
- `tq_pascal_bench` (lines 997-1000) sits inside `if(STRATA_ENABLE_CUDA OR STRATA_ENABLE_HIP)` (line 335), not under
  `STRATA_BUILD_TESTS`. A full build of a CUDA tree (no `--target`) therefore now also compiles and links this GPU bench.
  `--target strata` does not depend on it.
- `uma_cache_test`, `spec_start_test`, `pool_cores_test` and `numa_pool_test` are under `STRATA_BUILD_TESTS`.
- `SKIP_RETURN_CODE 77` is added to `native_hadamard_test` and `tq_cpu_parity`.
- Nothing of the following is added: `add_custom_command`, `add_custom_target`, `execute_process`, `file(DOWNLOAD)`, a
  `FetchContent` change, a compiler or linker flag, or a post-build step.
- Pre-existing and unchanged: the llama.cpp `FetchContent` at `GIT_TAG 3cf03257f219afbe7334045ff7c6a06ac68c627d`
  (lines 1563-1570) and the `execute_process` numpy probe (line 934).
- The sources of `strata_kq.dll` (`src/prefill/mmq_kq_lib.cu`, `src/prefill/ggml_cuda_host.cu`; target at line 1825)
  are not changed in this range.

**N14 — Tests that touch the filesystem or environment when ctest runs them** (the runbook runs upstream's suites).
- `numa_pool_test.cpp` (lines 26, 43, 139) and `pool_tasks_test.cpp` (lines 300-366) create directories under
  `temp_directory_path()` and later `remove_all` them.
- The directory names have predictable `std::time` suffixes. On a shared Linux `/tmp`, another user could pre-create
  the name and make the test fail. `remove_all` does not follow symlinks.
- `pool_tasks_test` sets and unsets environment variables of its own process.
- `pool_cores_test` and `pool_tasks_test` build real pools that pin their own threads.
- No test writes outside temp.

**Memory-safety observations (not deep-dived).**
- The `uma_cache.hpp` overflow (N9).
- `static int per_dev[64]` in `tq_stage_on` (`iq_kernels.cu:3317`) is a non-atomic cache, a benign race.
- `g_numa_plan` and `g_numa_owner` are plain globals (`pool.cpp`), which the authors' own CBA comment acknowledges. They
  are safe while pools are built and destroyed on one thread.
- The assumed stride in N10.
- No unchecked copies from protocol input, and no format strings built from input, were added.

## What this read did NOT verify

- Nothing was built, run or tested. All behaviour statements come from reading source.
- The numerical correctness or claimed bitwise equivalence of the kernel changes (`iq_kernels.cu`) was not verified, nor
  the lock-free correctness of the new per-slice claim protocol in `pool.cpp`. A deliberately subtle change inside
  kernel arithmetic would not necessarily be caught by this read, as the contract states.
- Only the changed hunks and the context needed to judge them were read. Unchanged code they call was not re-reviewed:
  - `pin_this_thread`;
  - `detail::get_thread_affinity`;
  - `free_memory_bytes`;
  - `host_available_memory`;
  - `total_physical_memory`;
  - the existing `GetProcAddress(GetModuleHandleA("amdhip64_7.dll"), "hipKernelNameRefByPtr")` in `verify.cpp:2356`.
- Files outside the C4 pathspec were not part of this read:
  - glanced only, both clean: `tests/core/file_expert_source_test.cpp` (+46/-0, a `numa_slice_ranges` test) and
    `tests/hip/prefill_mmq_parity.cpp` (+2/-1, a null-name guard);
  - glanced, benign: `tools/vision/CMakeLists.txt` (+3, `CMAKE_POSITION_INDEPENDENT_CODE ON` under `STRATA_VISION_HIP`);
  - not read: the `sycl/` and `metal/` trees (not built here), and all Python, scripts, Docker and packaging files (C3
    scope only).
- The llama.cpp sources fetched by `FetchContent` were not re-read; the pin did not move.
- The built binaries were not inspected (that is C5).
- Only the local tag objects were resolved. Whether the remote tags still point at these commits was not checked.
- Whether the frontend's user-arguments passthrough can deliver `--pool-cores` or other new flags to the engine was not
  checked.
- The superproject's index still records `upstream` at `cbfd5b92`; the working tree is at `011c9e60`. This read
  covers the range between those two commits.
