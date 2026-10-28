Controller rulings that bind Task 29 (supersede the brief where they differ):
- I21: real handoff heading is "### SP4 (branch `sp4-acquire`)"; "## Verification performed" already has "### SP4 merge into `main` …" — add a separate part-2 verification record for branch `sp4-acquire-part2` (Tasks 25–29). Keep the SP2 and winfix rows as they are.
- CI carry-in: cite CI run 38625663621 (base 557a2cc7: all green, MSYS2 real Windows 488/488, linux-gcc-release job green) when flipping "pending MSYS2" rows for things Tasks 25–29 did NOT change. New Tasks 25–28 windows-runtime cases stay "pending MSYS2" (no CI triggered by us).
- R5: record as a spec §10.1 text deviation: the record file is opened append/create once per spawn, not O_EXCL.
- R12: Known small gaps: acquire.log has one record per *transferred* file; reused/bundle-copied files get none.
- I20: note the spec-text inconsistency: merged label form `reduced:readable:<key>` vs spec §3.2 `reduced (readable: …)` / §7.3 `reduced:<cause>`.
- I15: Windows Ctrl-Break DL-04 case pending MSYS2 CI.
