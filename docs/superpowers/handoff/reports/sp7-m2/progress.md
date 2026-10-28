# SP7 M2 — release-side lane ledger

Lane: SP7 M2 sub-controller (Ed25519, signed release manifest, update completion, packaging, SBOM, offline kit).
Worktree: /home/opencode/projects/multi-strata/.worktrees/sp7m2 (branch sp7-m2-release-side, base 5f5df3d1 — verified via git log).

## Lane start
- 2026-10-22: Lane start. Fresh attempt (previous attempt produced nothing). Ledger created.
- Verified worktree at 5f5df3d1, branch sp7-m2-release-side, clean status.
- Next: read AGENTS.md, subcontroller-common.md, SP7 design spec, SP7 M2 plan.

## Reading pass
- Read: AGENTS.md, subcontroller-common.md, SP7 spec (D-1..D-10; D-9 amended = SPDX schema reality; D-10 binding), SP7 M2 plan (8 tasks, binding Interfaces, verified base facts), controller-resume.md, AGENT_HANDOFF.md (SP7 M1 rows + SP4 rows), PROVENANCE.md, threat-model.md.
- Working note: I am a subagent in OpenCode without the SDD Task-dispatch tooling; the launching brief directs me to run Tasks 1→8 myself with TDD + full gates after each task, synchronous in-turn. Following that.
- Commit discipline (from launch brief, supersedes trailer line in common brief): author shift-f10/40450442+shift-f10@users.noreply.github.com, trailer `Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`, NO Claude-Session trailer (matches owner ruling 2026-10-20).

## Seam inventory (pre-Task-1)
- `secure_frontend/CMakeLists.txt`: helpers `secure_frontend_add_test_executable`, `secure_frontend_add_cases(<binary> <case>... [ARGS][LABELS][TIMEOUT])`, `secure_frontend_add_library(name SOURCES.. LINKS..)`; `SECURE_FRONTEND_TEST_HOOKS` option (ON in test presets); sha256 lib at :495 (self-contained-crypto precedent); test registration block ~:1520+.
- `src/strata_cli.cpp:39,404-416`: exit-12 placeholder (`program_update()` lambda + two early returns) + usage line "program update: not available until SP7, exit 12" — Task 3 removes both.
- `src/strata_cli_network.cpp`: `run_network_verb` dispatch (update falls to `verb_update_models`), `NetworkKit`/`fill_kit` (ConfinedDownloader; B1 `ensure_downloader_cwd` rule), `InterruptScope` covers whole verb, `LoggingProgress` (§7.3).
- `include/secure_frontend/downloader.hpp`: `ConfinedDownloader : HopTransport, DownloaderProbe` — the ONLY allowed runtime fetch path.
- `src/service_ops.hpp`: `service::resolve_current_target` (D-4 seam M1 built for M2's update to write through), `kUnitFileName/kTaskXmlFileName`.
- Config: `src/frontend_config.cpp` schema table rows `{key, T::string|string_array|integer|boolean|decimal, required, min, max, default_i, ..., option, description[, decimal fields]}`; `T::string_array` exists (listener.cors_origins pattern).
- ExitCode has `program_update` (exit 12) — keep the class, repurpose the verb (plan: placeholder removed from usage; update now real).
- CliServices hooks-only seams precedent: `test_cacert` (F-51/R1), `--test-record-file`, `--test-force-no-landlock` (release build rejects them, exit 1).
- release_manifest pinned-key design ruling: plan binds `STRATA_RELEASE_PUBKEY_HEX` (empty = refuse). R-T3-sec: full update flow e2e must be SECURE_FRONTEND_TEST_HOOKS-only (hooks-only `test_pinned_release_key_hex` in CliServices, test_cacert precedent) + a release-only proof that the SHIPPED empty key refuses; release-preset builds get a decoy invalid hex via the preset cache var so the no-key message never appears there (proves the cache var reaches the shipped binary). Cost if wrong: none; every refusal path is still proven.
- Presets: `CMakePresets.json` (184 lines) — release preset runs full suite via `ctest --test-dir out/build/linux-gcc-release` (label-less invocation; debug preset excludes release-only cases).
- Fixtures dir: `secure_frontend/tests/fixtures/` exists.

## Rulings
- R-T0-a: Fetch RFC 8032 + RFC 4634/6234 official test vectors and the RFC 8032 Appendix reference Python via read-only web research (rfc-editor.org). — why: the plan binds "RFC 8032 official test vectors"; fabricating from memory risks wrong constants; free, read-only, nothing in build/product touches the network. — cost if wrong: none (one-time docs read).
- R-T0-b: Task 2 valid-envelope fixtures carry Ed25519 signatures computed by the RFC 8032 Appendix reference Python implementation, committed under tests/fixtures/ with the manifest bytes committed beside them (byte-identical), and cross-checked by the Task 4 signer as the plan's cross-check. — why: "no signing in C++" and Task 2 needs valid envelopes. — cost if wrong: fixture regeneration cost only.

## Task 1 — Ed25519 verifier (+ self-contained SHA-512)
- Research complete: RFC 8032 fetched; §7.1 Ed25519 vectors (TEST 1/2/3/1024/SHA(abc)) extracted and VERIFIED (pk derivation + sign + verify) against the RFC 8032 §6 illustration in Python — constants provably correct. SHA-512 FIPS example digests pinned via RFC 6234 §8.5 test arrays AND hashlib (they agree; my earlier memory of the "abc" tail was SHA-384's — caught by the cross-check). Non-canonical-S (S+L) negative value precomputed from TEST 3.
- Ledger artifacts in /tmp/opencode/sp7m2/rfc8032_ed25519.json (+ rfc6234 fetch); vectors inlined into the test source.
- Design: lib secure_frontend_ed25519 = src/sha512.{hpp,cpp} (FIPS 180-4 SHA-512, streaming, no deps) + src/ed25519_verify.{hpp,cpp} (verify-only, `bool ed25519_verify(pubkey32, msg, sig64)`, RFC 8032 §5.1.7 incl. S<L, point decode, cofactor [8] equation). Field math: 4×64-bit limbs + __int128 schoolbook mul + fold-by-38 reduction; normalized <p after every op (clarity over speed; update verifies a handful of signatures).

## Task 1 corrections found by data-integrity check (before any build)
- R-T0-b correction: my from-memory transcription of RFC 8032 §7.1 was WRONG in
  three places: TEST 1024's message is 1023 bytes starting `08b8b2b7...` (not the
  80a825... block), the group order is L = 2^252 + 27742317777372353535851937790883648493,
  and RFC sec-6 `point_add` uses A=(P1-P0)(Q1-Q0) with return (EF,GH,FG,EH).
  All vector data now comes from `rfc8032.txt` fetched from rfc-editor.org,
  parsed, and verified end-to-end (pk derivation, sign, verify, S+L malleation
  rejected by the canonical `s >= q` check but same group point without it,
  tamper rejected) by the RFC's own Python code (sec 6, lines 1087-1270) run
  verbatim. `/tmp/opencode/sp7m2/rfc8032_verified.json` is the single source;
  the C++ test file is GENERATED from it (no hand-typed hex).
- SHA-512 expectations in tests come from hashlib (== FIPS 180-4 examples:
  "abc" = ddaf35a1...4ca49f, a*1M = e718483d...cc09b, empty = cf83e135...927da3e).

## Task 1 RED evidence (linux-gcc, stubs: sha512 returns zeros, verify returns false)
secure_frontend_ed25519_tests built with -Werror clean; per-case results:
  FAIL sha512_fips_example_digests
  FAIL sha512_million_a_streaming
  PASS sha512_chunk_boundaries_do_not_change_digest   (invariant; stub digests are uniformly zeros)
  FAIL rfc8032_ed25519_official_vectors
  PASS ed25519_rejects_tampered_message               (negatives vacuous under the stub)
  PASS ed25519_rejects_tampered_signature             (negatives vacuous under the stub)
  PASS ed25519_rejects_swapped_pubkey                 (negatives vacuous under the stub)
  FAIL ed25519_rejects_noncanonical_signature         (canonical-first assertion fails)
  PASS ed25519_rejects_invalid_encodings              (negatives vacuous under the stub)
5/9 fail => genuine RED on every value-asserting path. Vectors/runner were
regenerated to repo test conventions (expect/failures + run_cases) meanwhile;
the test file is GENERATED from verified RFC data (see R-T0-b above).

## R-T1-sigma ruling (caught pre-implementation, with proof)
- My FIPS 180-4 section 4.1.3 recollection had the SHA-512 small sigmas
  swapped. Truth (proven three ways): the RFC 6234 C implementation extracted
  verbatim from rfc-editor.org text (compiled, self-tests match hashlib) and a
  clean-room Python over prime-derived constants (each constant checked equal
  to the RFC table, 80/80 and 8/8) give 12/12 hashlib-matching digests with:
    W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16],
    sigma0(x) = ROTR 1 ^ ROTR 8 ^ SHR 7        (applied to W[t-15])
    sigma1(x) = ROTR 19 ^ ROTR 61 ^ SHR 6      (applied to W[t-2])
  The C++ sha512.cpp must use exactly this pairing.
- K/H0 constants embedded in C++ are the prime-derived ones (equal to the
  RFC 6234 tables element-by-element; also digest-verified end to end).

## Task 1 GREEN + commit b4ca02b1 (b4ca02b1cffbbe7c270ed53c984ae0ca899484f8)
Root-cause debugging note (R-T1-math): the first C++ field layer failed all
positive verifications; a verbatim Python transliteration of the on-disk
field_mul reproduced the failure exactly, and step-by-step bignum diffing
isolated three genuine bugs, each fixed in the generator + re-verified:
  1. add_u64x3 returned carry1 | carry2; three u64 addends can carry out 2
     (found as an off-by-one in a convolution column).
  2. fold loop kept the pending carry pair as {sum.hi, carry.hi}; the hi
     lane must be pushed into the value lane one step up:
     carry = {sum.hi + carry.hi, 0}.
  3. schoolbook top word c[7] can overflow into 2^512, silently dropped;
     now accumulated as top_carry and folded (2^512 = 38^2 mod p: one 38 in
     the fold loop, one in the tail).
After the fixes: 5007/5007 bignum fuzz in the transliteration; the C++
probe's 133-deep squaring chain (2^(2^k) mod p) matches Python ground
truth for all 133 steps; all 9 ctest cases pass.

Gate battery after Task 1 (all run synchronously):
  linux-gcc:           build clean (-Werror), ctest 876/876 passed, 4 env skips (baseline 867+9)
  linux-gcc-release:   build clean, plain ctest --test-dir 842/842 (baseline 833+9)
  windows-llvm-mingw-cross: build clean, 0 warnings (cross binaries never run)
  pytest:              33 passed
Commit: b4ca02b1, author shift-f10, trailer Co-Authored-By Qwen; 6 files,
+1042 lines (ed25519_verify.{hpp,cpp}, sha512.{hpp,cpp}, ed25519_test.cpp,
CMakeLists lib secure_frontend_ed25519 + test exe with release-also/TIMEOUT 120).

## Task 2 RED evidence (linux-gcc, stub: everything rejects with reason json / "not implemented")
secure_frontend_release_manifest_tests built -Werror clean; per-case rc:
  FAIL good_envelope_verifies                 ("good envelope must verify")
  FAIL tampered_manifest_rejected             (wrong reject reason)
  FAIL tampered_signature_rejected            (wrong reject reason)
  FAIL wrong_pinned_key_rejected              (wrong-key case not reached)
  FAIL no_pinned_key_refuses_exact_line       ("the refusal line is exact" - stub says "not implemented")
  FAIL malformed_envelopes_rejected           (framing/base64/json reasons all wrong)
  FAIL schema_violations_rejected             (schema reason wrong)
7/7 RED, every case asserts its exact reject reason => no vacuous passes.

## R-T2 rulings (Task 2 design, recorded pre-GREEN)
- R-T2-a gate order: framing -> strict base64 (88 chars, canonical padding,
  strict trailing bits, exactly 64 bytes) -> JSON (SP1 parse_json, integer-
  only, dup-key rejecting) -> schema -> Ed25519. Malformed structure rejects
  before any crypto; well-formed-forgeries reach the equation. Schema tests
  reuse the GOOD signature on broken manifests, pinning the order.
- R-T2-b pinned key: STRATA_RELEASE_PUBKEY_HEX must be exactly 64 lowercase
  hex chars; empty OR malformed == not pinned (refuse identically).
- R-T2-c framing rules: exactly one occurrence of "\n--STRATA-SIGNATURE-V1\n"
  (0 or >=2 => framing); after the marker: strict base64 token, then at most
  one trailing '\n' to end; anything else after the token => framing;
  malformed token => signature_base64.
- Fixtures: real RFC 8032 section-6 reference signatures over the release
  manifest (seed = RFC vector-1 seed => pk d75a9801...== the RFC §7.1 key;
  decoy = vector-2 key 3d4017c3...). Generated (gen_release_fixtures.py +
  gen_release_test.py), re-verified by the reference verify before emit.

## Task 2 GREEN + commit 706fc5db
7/7 GREEN. GREEN-time test-generator bugs (fixture data unchanged, all asserts kept):
raw-string closers, python escape swallowing in the template, schema variants built
from the already-escaped C++ literal (nested raw strings), notes_url-missing variant
leaving a JSON trailing comma (json reason before schema).
Extra proof (not ctest, /tmp probe): pinned compile path -DSTRATA_RELEASE_PUBKEY_HEX
valid-hex VERIFIED / malformed-hex REFUSED / empty REFUSED (exact line).
Gates: linux-gcc 0 warn, ctest 883/883 (876+7); release 849/849 (842+7);
cross rc=0 0 warnings; pytest 33.
Commit 706fc5db (4 files, +559): CMakeLists cache var + lib + tests,
src/release_manifest.{hpp,cpp}, tests/release_manifest_test.cpp (generated).

## Task 3 (in progress) - config support for update.source (pre-RED support slice)
- frontend_config.hpp: struct UpdateSettings final { std::vector<std::string> source; };
  FrontendConfig member `const UpdateSettings update;` (after pull), ctor trailing
  param `UpdateSettings update_value = {}` (existing call sites unchanged), mem-init
  update(std::move(update_value)).
- frontend_config.cpp: kSchema row update.source (string_array, size expr +1);
  kSchemaExtensions 5->6 with is_valid_release_source_url (mirrors parse_source_url:
  https only, host+path, no port/userinfo/query/fragment/%/[], host label chars,
  no leading/trailing '-', no ".."); validate() reads it via read_bounded_strings;
  describe() prints line("update.source", list_text(...)).
- parity_table.cpp: rows update.source (kTyped/config_only) + STRATA_RELEASE_PUBKEY_HEX
  (kMapped/none); kRows 121->123. typed_config_test.cpp: cfg_update_source_typed case.
- README.md config-keys table regenerated via render_config_schema_markdown() (the
  readme_config_table_matches_schema test prints the expected table; splice it verbatim,
  renderer output has NO trailing newline - keep exactly the bytes between markers).
- Debugging lesson (cost 2h, worth recording): I dropped ONE closing paren in the ctor
  call edit (HEAD had glm))); my line ended ...)}); single paren). GCC 13.3's cascade
  pointed at the WRONG line (shared_ptr<( line, "expected primary-expression"); clang-18
  (.toolchains/clang18/usr/bin/clang++-18 -fsyntax-only) named the exact missing ')'
  with the matching-'(' note. When GCC 13 parse-cascades look nonsensical, re-check with
  clang-18 before bisecting. All files untouched were fine; git diff was the truth.
- Verified: g++ -Werror compile of frontend_config.cpp OK; ctest -R 'typed_config|parity'
  64/64; full linux-gcc 883/883 + 4 env skips (baseline).

## Task 3 RED (update CLI suite) + rulings
Seams added first (so RED compiles): CliServices gains test_pinned_release_key_hex +
test_self_executable (SP7 M2 Task 3; hooks-compiled builds only, mirrors test_cacert);
CliHarness::write_config gained extra_top_level (raw JSON after the pull object).
Suite: tests/update_cli_test.cpp GENERATED (gen_update_fixtures.py signs the REAL
component payload bytes with the verbatim RFC §6 signer, re-verify before emit;
gen_update_test.py substitutes every byte constant). Fixtures: pk1 = RFC vector-1
key d75a9801...; frontend 28 B sha 57c050a8...; engine 29 B sha bfbd3d34...;
envelope 587 B; tampered = version 9.9.9 signed-over-good-bytes (reference verify
of the tampered pair returns False, checked in the generator).
CMake: secure_frontend_update_cli_tests; 2 no-hook cases run in release too,
8 hook cases under if(SECURE_FRONTEND_TEST_HOOKS); LABELS release-also TIMEOUT 120.
RED evidence (ctest -R update_, placeholder verb): 10/10 update_cli cases FAILED,
every case asserting its exact line/exit (no vacuous pass):
 update_refuses_without_update_source (out line), update_refuses_without_pinned_key,
 update_refuses_plain_install, update_installs_and_flips_current (exit 1, no golden
 lines), update_declines_without_yes (12), update_digest_mismatch_keeps_current,
 update_tampered_envelope_rejected, update_falls_back_to_next_source,
 update_rollback_flips_both_directions, update_rollback_without_previous.
Rulings (binding on GREEN):
- R-T3-a URL layout: update.source entries are mirror BASE urls; envelope at
  <base>/envelope, components at <base>/<file>; components fetched from the mirror
  that delivered the envelope.
- R-T3-c version safety: manifest version must match [A-Za-z0-9._-]{1,64}, not "."
  or "..", before any path use.
- R-T3-d exit: every program-update failure exits 12 (ExitCode::program_update),
  incl. transport/verification stages (report() still prints the acquire line to err).
- R-T3-e/j: bare `strata update` (no --config) becomes the standard usage error
  (exit 1); `--config f update` is the program verb now. Exit-12 placeholder line
  removed from usage. The §6.2 no-network invariant moves into the new cases
  (recorded seams prove refusals happen before any fetch).
- R-T3-g order: args -> update.source -> versioned-install check -> pinned key ->
  fetch envelope (per source, first valid wins; report per-source failure + fixed
  fallback line) -> show current/offered/components -> confirm (--yes) -> fetch+verify
  every component into <install>/.update-staging/<version>/bin -> all digests ok ->
  atomic rename to <install>/<version>/ -> flip current (previous=old) -> hint.
- R-T3-h components install under <version>/bin/<file> (exe layout of D-4).
- R-T3-i failure leaves no staging behind (staging dir removed); current untouched.
- Fixed texts: "strata update: update.source is not configured"; "no release key
  pinned" (Task 2 line); "strata update: not a versioned install (this program is
  not started through a current pointer)"; "update: current version V";
  "update: offered version V (channel C)"; "update: component NAME: FILE, N bytes,
  sha256 HEX"; "update: installed version V with N components"; "update: current ->
  V (previous kept: P)"; "update: restart strata to run the new version"; "update:
  declined; current version V untouched"; "update: failed; current version V
  untouched"; "update: source N refused the envelope; trying source M"; "update:
  rollback: current V, kept P"; "update: rollback nothing"? -> "update: nothing kept
  to roll back to"; "update: restart strata to run the rolled-back version".

## Task 3 GREEN + commit (update verb)
Implementation: strata_cli_network.cpp verb_update_program (+verb_update splitter:
--models -> unchanged models half; anything else -> program half). Confined-stack
fetch: fill_kit (probe/spawn rules identical to pull), hop_request + resolver pin
+ map_curl_exit (never a bare curl), envelope into memory (1 MiB cap, own head
rules: 200 only, redirect rejected), components streamed+SHA-256-verified with
classify_head. Staging <install>/.update-staging/<version>/bin -> atomic rename ->
pointer flip (Linux symlink+rename; Windows current.txt/previous.txt with the
remove-then-rename gap documented for the owner-box). Placeholder + its two
dispatch short-circuits removed from strata_cli.cpp (R-T3-e/j); usage line updated.
R-T3-l (GREEN-time test-driven correction): pinned-key refusal precedes the
install-layout check on the fetch path; rollback skips the key entirely (pure
pointer flip, no crypto). Test 594 (plain install) therefore pins the key explicitly.
px7_nothing_but_the_update_verb_touches_update_source: scans every src *.cpp/.hpp
for the member-access token ".update.source"; only strata_cli_network.cpp and
frontend_config.cpp may hold it (ARGS=${CMAKE_CURRENT_SOURCE_DIR}/src).
acquire_cli_test.cpp program_update_exits_12_without_network rewritten (R-T3-j):
bare update -> usage 1; --config update -> 12 + new fixed line; still zero seams.
README parity blocks +2 rows (update.source typed; STRATA_RELEASE_PUBKEY_HEX
mapped); tools/secure/test_parity_readme_coverage.py row gate 121->123 (its own
message prescribes updating gate+README together; a test changed only as the
contract grows, never weakened).
GREEN evidence: update_cli 11/11; program_update_exits_12 rewritten PASSES.
Gates: linux-gcc 0 warn, ctest 894/894 (883+11) + 4 env skips; release plain
852/852 (849+3: the two no-hook cases + px7); windows-llvm-mingw-cross rc=0 0
warnings (Windows write_pointer/read_pointer branches compile-proved only);
pytest 33/33 (after the parity-count + README rows update). typed_config|parity
64/64 on both presets after README edits.

## Task 4 RED/GREEN + commit (release key material + signer)
RED (two interlocked halves): new C++ case python_signed_fixture_verifies failed
("fixture ... exists"); tools/secure/test_release_signing.py 7/7 failed (scripts/
fixtures absent).
GREEN: scripts/release/sign.py — the RFC 8032 section-6 reference code inserted
VERBATIM from the fetched RFC text (assembler gen tooling in the lane; only page
headers dropped), plus envelope()/verify_envelope()/self_test() + CLI (sign,
--verify, --self-test). Canonical envelope shape: manifest + framing marker +
base64 + exactly ONE trailing newline (matches the Task 2 pinned 844-byte
envelope; R-T2-c already permits it). scripts/release/keygen.py — deterministic
key_material(seed_hex) or ephemeral os.urandom(32); --out-key writes the seed
0600 O_EXCL (never overwrite, never committed); prints the pinned hex verbatim.
Fixtures committed under secure_frontend/tests/fixtures/release/:
rfc8032-vectors.json (the lane's fetched+re-derived official vectors),
manifest.json (Task 2 manifest bytes), signed-envelope.txt (generated BY the
signer CLI), pubkey.hex (via keygen). Interlocks: the C++ verifier accepts the
fixture (python_signed_fixture_verifies, ARGS=tests/fixtures) AND the fixture
equals the C++ suite's pinned R"ENV(...)ENV" literal byte-for-byte (regex-locked
in the Python test) AND sign.py reproduces the fixture from the RFC vector-1 seed
AND sign.py --self-test re-derives all 4 official vectors.
CLI e2e test uses ephemeral keys in tmp_path only.
Gates: ctest 895/895 (+1), release plain 853/853 (+1), cross rc=0 0 warnings,
pytest 40/40 (33+7).

## Task 5 (manifest + SBOM generators) — contract study + R-T5 rulings
Contract re-verified against scripts/secure_distribution_check.py source
(lines 89-129): profile/network_default/legacy_components/privacy-9-booleans/
components-non-empty; extra top-level keys accepted; SBOM existence-checked
only; deployment.md sample pins component schema {name,version,source}.
R-T5-truth (caught during implementation, BEFORE any commit): my first draft
manifest_lib hand-transcribed WRONG submodule SHAs and invented a
non-existent "strata-attach" binary + GPU "optional_unbuilt" entries.
Ruling: generators NEVER hardcode facts read from the repo — submodule pins
via `git submodule status`, URLs via .gitmodules, license/copyright parsed
from the LICENSE files themselves (each heading asserted to a known license or
the generator refuses); shipped binaries verified against CMakeLists add_executable
(strata, strata-secure-server only). Tests likewise derive pins from live git
and scrape the 9 privacy keys from the check script — never restate them.
R-T5-det: --version required (caller supplies); --created-utc pinnable;
manifest --source defaults to live `git rev-parse HEAD`; JSON dumped
indent=2 sort_keys + one trailing newline -> byte-reproducible.

## Task 5 RED
tools/secure/test_release_manifest_and_sbom.py (6 tests) -> 6 failed
(make_manifest.py/sbom.py absent, exit 2 from subprocess; existence test fails).

## Task 5 GREEN + commit
scripts/release/manifest_lib.py (PRIVACY_KEYS, SHIPPED_BINARIES,
release_manifest()), make_manifest.py (CLI), sbom.py (SPDX-2.3 JSON:
frontend package + 2 vendored packages w/ live pin in versionInfo
"vX (40-hex)", downloadLocation=git URL, purl externalRef
pkg:github/..., the license concluded+declared from each LICENSE heading, copyrightText
from LICENSE lines; DESCRIBES relationships for every package).
GREEN evidence: 6/6. e2e: staged dir (bin stubs + both files) ->
check --release rc=0; tampered analytics:true -> rc!=0 naming analytics
(negative control). Real demo run: SBOM shows upstream v0.2.1.3 @
cbfd5b926b794b356195d3cb600191b0c3567540 and maya-ref v1.1.8 @
5c970a863ce2fdd87b6e23ffb4822f6d76522b1c (both MIT).
Gates: linux-gcc 0 warn, ctest 895/895 + 4 env skips (unchanged, no C++ added);
release plain 853/853; windows-llvm-mingw-cross rc=0, 0 warnings;
pytest 46/46 (40+6).

## Ledger correction (found at Task 5 commit time)
Commit 16264a48 (Task 4) was missing the CMakeLists hunk registering
python_signed_fixture_verifies (secure_frontend_add_cases ... ARGS
tests/fixtures LABELS release-also) — the T4 gate numbers (895/853) were
produced with the hunk in the worktree but it was never staged, so a clean
checkout of 16264a48 holds 894/852 and the C++/Python interlock case.
No test was weakened; the registration itself was never in any commit.
The hunk is committed as part of the Task 5 commit, and the Task 5 gate
numbers above (895/853) already include it — they stay valid for HEAD after
this commit.
COMMIT: 792647fa (5 files, 411 insertions; includes the Task 4 registration hunk).

## Task 6 (packaging builders) — rulings
R-T6-a: secure_distribution_check.py --release stays byte-unchanged (Task 5
contract); D-9's on-top evidence (SHA256SUMS completeness via re-verify,
signed D-3 envelope verify via sign.py, exact `unsigned: not releasable`
line) lives in build_release.sh/stage_windows.sh gate steps.
R-T6-b (schema truth found in src/release_manifest.cpp): D-3 component names
are UNIQUE in the closed set {frontend, engine, cuda-runtime} -> the update
manifest carries exactly one file per class; frontend->strata (the CLI that
performs updates). strata-secure-server ships in the bundle (bin/) but is
NOT updateable via `strata update` under D-3 as specified — FLAGGED for the
controller/owner; not relitigated in code. notes_url must be https non-empty.
R-T6-c: release trees are hooks-free BY CONSTRUCTION: stage_windows.sh with
no --build-dir configures+builds its own windows-llvm-mingw-release dir
(preset windows-llvm-mingw-cross + -DSECURE_FRONTEND_TEST_HOOKS=OFF, targets
strata strata-secure-server only); the distribution judge is the last step
of BOTH builders and refuses hooks binaries (negative control test stages
the hooks-carrying cross gate dir and asserts refusal).
R-T6-d: build_release.sh: version charset [A-Za-z0-9._-]{1,64} (R-T3-c);
refuses pre-existing bundle/deb-root dirs (no rm -rf); CUDA libs copied from
the build tree only when present (libcudart.so.*, maxdepth 2 -> bundle lib/);
engine binaries optional (strata-engine|secure-strata-engine); order:
binaries -> evidence -> D-3 manifest -> sign-or-mark -> judge -> SHA256SUMS
(all files but itself) -> sha256sum -c -> tarball -> dpkg-deb (when present;
usr/bin + usr/share/strata-secure; DEBIAN dirs chmod 0755 — umask 077 hit).
Signed run: sign.py envelope manifest.json.sig (manifest bytes verbatim
prefix), keygen-derived pin re-verifies it; STRATA_SIGNING_KEY names the
seed file; key never in the tree (deb copies only .sig).
NSIS attempt (plan: recorded either way): apt-get download nsis 3.09-4ubuntu1
-> .toolchains/downloads/nsis_*.deb, dpkg-deb -x -> .toolchains/nsis;
makensis binary present but baked /usr/share/nsis stub paths; unprivileged
bind via unshare -rm DENIED (no uid_map write). stage_windows.sh PROBES
`makensis -VERSION` (exit code, not just existence — Debian prints a version
while failing) and records available/present-unusable/unavailable truth in
NSIS_STATUS.txt; makensis RUN stays owner-box.

## Task 6 RED
tools/secure/test_build_release.py (8 tests incl. hooks-refusal negative
control) -> 7 failed at first write (scripts absent), 6 failed mid-GREEN
when the judge caught hooks binaries + dpkg umask (RED caught real bugs).

## Task 6 GREEN + commit
scripts/release/build_release.sh, stage_windows.sh (embedded installer.nsi:
user-level, no network, both exes + evidence files, versioned OutFile),
make_update_manifest.py (D-3 sizes/sha256 computed from shipped bytes).
GREEN evidence: 8/8; demo runs: linux bundle (deb built:
dpkg-deb -c shows usr/bin/strata+server), signed run verifies, windows
staging PASSes the judge, hooks dir refused.
Gates: pytest 54/54 (46+8); linux-gcc 0 warn, ctest 895/895 + skips;
release plain 853/853; windows-llvm-mingw-cross rc=0, 0 warnings.
COMMIT: ddb1cdbd (Task 6)

## Task 7 (offline kit docs) + commit
docs-only: docs/validation/sp7-offline-kit.md (0 prereqs driver>=580/memlock/
CUDA, 1 build w- pinned key in cache + STRATA_SIGNING_KEY + record SHAs,
2 unshare -rn unpack + sha256sum -c, 3 doctor/doctor --deep, 4 serve+local
chat under closed netns, 5 update accept + update --rollback both directions
+ two refusal proofs (unpinned key, tampered component), 6 results hash ->
PROVENANCE row). Invocation form corrected to the REAL CLI: --config before
the verb, global, mandatory (bare update is the usage error per R-T3-e/j).
docs/validation/README.md kit row #4; docs/PROVENANCE.md new "Release
evidence (SP7 M2 — placeholder)" 13-field blank table + no-claim sentence
before the link block. No tests to write (docs); gates run anyway:
pytest 54/54; ctest 895/895; release 853/853; cross rc=0 0 warnings.
COMMIT: 620b4d1a (Task 7)

## Task 8 (truth pass) + commit
docs/AGENT_HANDOFF.md: new SP7 M2 section at the top of "What has changed"
(base f4dff56d = main with M1 merged; verified via git log), 8-task table
with commit shas + proof columns, gate paragraph (895/895, 853/853, cross
0w, pytest 54), declared-gaps paragraph: no pinned key exists (by design),
D-3 one-file-per-class -> strata-secure-server not update-distributable
(FLAGGED), Windows pointer-flip remove-then-rename gap + compile-proved-only
status, makensis-unusable truth, kit/GPU/Windows claims wait for owner rows,
uninstall install-metadata stays vacuous by design (R-T5-b comment verified
at strata_cli_network.cpp:1126). docs/PROVENANCE.md: new "Vendored crypto
(SP7 M2, D-2)" section — verification-path-only C++ lib, RFC 8032 section-6
verbatim Python signer, fetch-and-rederive test-vector rule, IETF Trust
license note, cited RFC title CORRECTED against the fetched RFC itself
("Edwards-curve Digital Signature Algorithm (EdDSA)") — my remembered title
was wrong, caught by primary source (research rule).
Gates: pytest 54/54; ctest 895/895; release plain 853/853; cross rc=0 0 warnings.
COMMIT: 5b2ba5a7 (Task 8 - truth pass)
