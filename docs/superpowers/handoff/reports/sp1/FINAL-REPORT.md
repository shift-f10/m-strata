# SP1 final report — typed config, canonical paths, tokens

**Status:** COMPLETE
**Branch / HEAD:** `sp1-typed-config-paths-tokens` @ `04210cbf` (worktree `/opt/projects/ai/.worktrees/sp1`), base `17d2452d`. All commits are local: nothing was pushed or merged, and no earlier commit was rebased. Every commit has author `shift-f10 <40450442+shift-f10@users.noreply.github.com>` and the trailer `Co-Authored-By: Claude Fable 5.1`. The Task 1 commit first carried an Opus trailer; the controller fixed that with a message-only amend while it was still HEAD (f61d75f6 → 6be5042e, tree unchanged).

## Commits (`git log --oneline 17d2452d..HEAD`, 36)
```
04210cbf docs: handoff verification counts for the SP1 final fix wave (SP1 final review F4)
c706366d docs: init writes a token only for a file source; Windows token root is opened through its link (SP1 final review F1, F2)
4ad028c0 server: serve prints the actionable startup reason; --check gives its verdict after the token read (SP1 final review I3, F3)
7d59353c engine: pass --native as UTF-8, check the executable's NUL on path::native() (SP1 final review I2)
9e8312b5 config: narrow limits.chat ranges to the enforced caps (SP1 final review I1)
05c9db02 docs: SP1 handoff, deployment, README key table from the schema, limits note; bootstrap deletes a bad archive (SP1 D1-D5)
1904f827 tests: AUTH-04 sweeps derive flags and STRATA_* names from the schema; AUTH-01 tab and duplicate header over TCP (SP1 A4, A1 fix)
3810a67b tests: AUTH-01, AUTH-02, AUTH-04 acceptance (SP1 A1, A2, A4)
792b7651 tests: CFG-05 end to end and CFG-06 at the type level (SP1 K11, K12)
dae07073 config: one JSON tree renderer shared by init and the test helper; init creates nothing on bad input (SP1 K10 fix)
92a8419a server: init --config FILE [--force] writes an owner-only config and a generated token (SP1 K10)
2b480430 secrets: wipe and unset the environment token after reading; non-dumpable, zero core limit (SP1 K8, K9)
137f2b4a auth: one TokenVerifier in the library; fixed 256-byte window comparison without a length oracle (SP1 K6, K7)
f9f146ac token reader: runs on every platform; one trailing LF/CRLF; SecretBytes result; 32-byte floor (SP1 K4, K5, K6; defect c)
013f540a token store(windows): validate the holding directory with open_beneath and pin it across create and move (SP1 K3 review fix 1)
25ba2270 token store(windows): DPAPI-wrapped STRATAK1 file with a protected owner-only DACL; reader unwraps DPAPI (SP1 K3)
510617b5 token store: assert successful rotate; fail closed when the target cannot be inspected (SP1 K2 review fix 1)
e0f4294c token store: atomic owner-only writes (O_EXCL|O_NOFOLLOW temp, fsync, renameat2 NOREPLACE) on Linux (SP1 K2)
5cc8a91f secrets: SecretBytes, CSPRNG token generator and base64url (SP1 K1, K6 type)
264edde1 paths: create directories only within a root; private logs/cache roots; duplicate checks gone (SP1 P8, P9)
56a60c5d inventory: a non-portable directory name fails the inventory (SP1 P6 fix round 1)
96aaf699 inventory, tokenizer: enumerate and open through the canonical-path module (SP1 P6)
ccd7c63b config: refuse config files (and their directories) that other users can write (SP1 C7)
9d9b1fba paths(windows): READ_CONTROL handle walk with reparse and hard-link rejection; Windows token reader uses it (SP1 P5; defect b)
e07b0601 paths: openat2 walk below a root with an openat(O_NOFOLLOW) fallback; Linux token reader uses it (SP1 P4)
5d6232e5 config: roots.secret defaults to the config folder only for a file: token (SP1 P2 fix round 1)
b36dd17a paths: canonical roots and engine executable; engine gets the canonical models root (SP1 P2, P3, P7; defect e)
51059af4 paths: one relative-path grammar for secret sources and model artifacts (SP1 P1)
d9613548 limits: listener, parser, chat schema and chat execution take their limits from FrontendConfig (SP1 C4, C5; defect d)
e6e82aa1 tests: deterministic C3 guard for bind-before-spawn (SP1 C3 fix round 1)
2b0f83b4 server: bind before spawning the engine; process-level NET-01/NET-02 tests (SP1 C3, N3, N4)
67ac645a server: --config and --check through one validator; never echo option values (SP1 C6, C8)
67634f81 config: dotted or empty member names never alias nested keys; duplicate path on insert is an error (SP1 C2 fix)
95ca08ad config: typed FrontendConfig, schema table and validate() collecting every issue (SP1 C2)
828daac8 json: one strict bounded reader for chat schema and tokenizer pack (SP1 C1)
6be5042e config: classify listener addresses from bytes; reject hostnames (SP1 N1, N2; defect a)
```
Task 2 (the shared pre-task, 17d2452d) was already on main and was not re-dispatched.

## Final test counts (controller re-ran these at 04210cbf)
- linux-gcc `ctest --preset linux-gcc`: **215/215** passed. The baseline was 111.
- `windows-llvm-mingw-cross` cross build with `-Werror`: 0 warnings. Wine `ctest`: **128/128** passed. 193 tests are registered; 65 carry the `windows-runtime` label and are excluded, pending MSYS2.
- `pytest -q` at the worktree root: **13 passed**.
- `ctest -N` lists 59 acceptance-named cases (`net0*`/`auth0*`/`cfg0*`) individually.
- Under wine, `strata-secure-server.exe --help` exits 0, and `--check` on a bad config exits 1 naming the key.
- Not run: linux-clang (clang is not installed here), msvc, the root build, and windows-msys2-clang CI. **Nothing is Windows-runtime-verified.** Every Windows-only case is marked "pending windows-msys2-clang" in `docs/AGENT_HANDOFF.md`.

## Final whole-branch review
- **Verdict:** "With fixes". 0 Critical, 3 Important:
  - I1: the §9.5 config ranges were wider than the caps the server actually enforces.
  - I2: the Windows engine argv path went through the ANSI code page.
  - I3: serve mode dropped the actionable token, tokenizer and inventory reasons.
- **Fix wave:** a single fix wave (9e8312b5..04210cbf) fixed I1–I3 and folds F1–F4 (README init wording, token_reader.hpp R7 comment, `--check` verdict after the token read, handoff gaps and counts). The scoped re-review found **all addressed, no new breakage**.
- **Residual findings (parked, non-blocking, fail closed):**
  - `smoke_server_main.cpp:155,177` still prints generic lines for token and inventory errors.
  - "Engine arguments rejected." prints no reason.
  - The 512 KiB rendered-prompt cap can still produce a 503 if request/body limits are raised above ~512 KiB. It is documented as an internal constant.
  - M1: `init` does not create an explicit absent `roots.secret`. A 0755 config folder passes `--check`, then `init` fails at the token write.
  - M2: six `valid_utf8` copies.
  - M3: the `constant_time_token_equals` wrapper remains. It delegates to the verifier.
  - About 120 deferred Minors remain in the ledger (`progress.md`, `Task N: minor (deferred)`). The final reviewer triaged them: none must be fixed before merge, and six are closed as stale.

## Rulings I made (in order; each: what — why — cost if wrong)
1. The pre-flight conflict scan of the 11k-line plan went to a read-only opus helper (preflight-scan.md), and I ruled on its table. Why: to keep the controller's context. Cost if wrong: a missed conflict surfaces in a review loop instead.
2. Commit trailers use Fable 5.1 per the common brief, overriding the plan's Opus example. Cost if wrong: a trailer amend.
3. R1: Task 8 loop uses `const std::string&` because of `-Werror=range-loop-construct`. Cost if wrong: none.
4. R2: Task 9 keeps `is_below_or_equal_final_path`/`WindowsHandle` until Task 11, and `current_user_sid` uses a file-local RAII. Why: Windows cross build. Cost if wrong: a small cleanup.
5. R3: Task 10 moves `source_relative_path` under the Windows branch, and Task 11 deletes it. Why: Windows cross build. Cost if wrong: a leftover function.
6. R4: Task 16 marks `fail`/`random_suffix` as `[[maybe_unused]]` because of `-Werror=unused-function` on llvm-mingw. Cost if wrong: attribute noise.
7. R5: Task 9's cfg03 e2e case makes a chat request before reading the engine dump. Why: a race. Cost if wrong: a slightly slower test.
8. R6: the Task 22 CFG-06 tests are kept, and the runtime one reads the token via `read_token` first so the scan can fail. Cost if wrong: one extra line.
9. R7: four plan-vs-spec deviations are accepted and recorded in the handoff:
   - `roots.tokenizer_pack` is required.
   - The config file and its directory may be root-owned.
   - The Windows root is opened through its link.
   - Process-defaults hardening applies to every binary.
   Cost if wrong: the owner reverses one later.
10. R8: implementers anchor on quoted text, not line numbers, and Task 24 replaces whole deployment.md bullets. Cost if wrong: doc touch-ups.
11. R9: under wine, Linux-absolute ARGS are run as planned, and a case is labelled only if it fails on the path alone. Cost if wrong: a few wine skips. No label was needed.
12. Task 1: the Opus trailer was fixed by a message-only amend (6be5042e). Cost if wrong: none (local).
13. Task 4: the dotted-member duplicate bypass was fixed despite the plan text, because spec §3.1 binds. Cost if wrong: a flat dotted form is rejected.
14. Task 6: the C3 guard was reworked to a deterministic non-program engine file, and the false "failed before" claim was corrected in the fix commit (no amend). Cost if wrong: one test differs from the plan, and history keeps a corrected line.
15. Task 7: the test asserts 400 rather than 413 for an over-bound Content-Length, because spec C4 only requires a rejection. Cost if wrong: one assertion.
16. Task 7: `EngineProtocolLimits` and the 256 model-field bound were carried to Task 24/final review. Cost if wrong: a gap found late. Both were later resolved by I1 and D5.
17. Task 9: `roots.secret` defaults to the config folder only for a `file:` token (spec §3.2/§4). Cost if wrong: none material.
18. Task 9: absent logs/cache roots were interim-rejected until Task 14, which restored acceptance with a `--check`-creates-nothing test. Cost if wrong: none, since it is closed.
19. Task 10: `read_token(path)` and the smoke server's `--secret-root` must be absolute (spec §3.4). Cost if wrong: devs must pass absolute paths.
20. Task 13:
    - Non-portable non-.gguf FILES are ignored.
    - Non-portable directories, bad .gguf names, symlinks and case-fold duplicates fail with the entry named.
    - Windows `FindNextFileW` error checking and the reason for a listing failure were folded in.
    Cost if wrong: an odd-named folder fails startup with an actionable message.
21. Task 16: the rotate test asserts success and a content change, and an `fstatat` non-ENOENT error fails closed (spec K2). Cost if wrong: none.
22. Task 17: the Windows writer opens the holding dir via `open_beneath`, pins it without FILE_SHARE_DELETE with a file-ID match, and the round-trip tests compare values. Cost if wrong: Windows-only code that is unverified until MSYS2.
23. Task 21:
    - The JSON tree renderer is deduplicated (`render_config_literals`).
    - `--force` refuses a non-regular config target.
    - `init` validates before creating a directory.
    Cost if wrong: a small helper refactor.
24. Task 23: the AUTH-04 sweeps are derived from the schema (including `STRATA_ENGINE_READY_S`), and AUTH-01 tab/duplicate cases run over TCP. Cost if wrong: a test-only change.
25. §9.5: the schema ranges are narrowed to the enforced caps:
    - max_messages ≤ 128
    - max_message_bytes ≤ 65536
    - max_prompt_tokens ≤ 262144
    - the 512 KiB prompt cap is documented as internal

    Cost if wrong: operators cannot set values that were never honoured.
26. Final fix wave: I1–I3 plus F1–F4 in one wave. M1–M3 stay deferred. Cost if wrong: narrower ranges and small message changes.
27. The plan workspace is not deleted: this report lives in it and the guard blocks `rm -rf`. Cost if wrong: one git-ignored directory.

## Parked items / for the owner
- **Pending windows-msys2-clang:** the full list is in AGENT_HANDOFF.md, covering:
  - DPAPI and protected-DACL store/read
  - junction and symlink rejection
  - `READ_CONTROL` defect (b)
  - `GetExtendedTcpTable` NET-01
  - the Windows env scrub
  - the Windows root and executable DACL checks
  - non-ASCII engine argv (I2's RED was shown only under wine)
  - init, cfg05 and cfg06 on Windows
- **Windows handle-relative create and move (`NtCreateFile` RootDirectory)** is deferred. Protection against ancestor rename relies on unverified NTFS behaviour.
- **Fixed socket-test ports 20000–20099** are still parked (SP3). The SP2 worktree ran ctest concurrently on the same host; no collision was observed.
- **Deferred Minors** are in `progress.md`. M1 (init with an explicit absent `roots.secret`) is the most user-visible one.
- **Host header with `listener.host=localhost`:** `allowed_hosts` is the canonical `127.0.0.1:port`, so `curl http://localhost:port` gets 403. This is WEB-*/SP3 and was noted for the product spec.

## Concerns
- No real Windows run happened. All Windows-only logic was judged by reading, plus wine smoke, which cannot model DACLs, DPAPI semantics or junctions.
- Implementers' harnesses showed an "Opus 5.5" attribution reminder, while the owner brief mandates Fable 5.1. Every commit carries Fable 5.1 as the brief requires; a few implementers flagged the mismatch for the owner to confirm.
