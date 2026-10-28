Controller rulings that bind Task 28 (supersede the brief where they differ):
- R6: priv01_positive_control_doctor_downloader_probe_is_recorded also runs the strata child under LD_PRELOAD=<connect-audit shim> with SF_CONNECT_AUDIT on Linux and asserts the audit is empty; register with extra ARG $<TARGET_FILE:secure_frontend_connect_audit>. Windows: no shim.
- R7: DL-01 Windows also enumerates GetExtendedUdpTable(…UDP_TABLE_OWNER_PID…) for the pid and fails on any non-loopback local binding.
- R9: priv01_local_verbs_never_spawn_downloader asserts each verb's expected exit code; add one `update --models` against an outdated hooks fixture catalog model (fixture-qwen:t1 installed at an older sequence) answered No (exit 2).
- R10: lifecycle() calls wait_engine_ready() and asserts a 200 chat; the crash/restart case polls /health until a restart is observed (not-ready then ready again) before auditing. Generous timeouts.
- I17: connect-audit shim treats every family other than AF_INET/AF_INET6 as local.
- I18: add_dependencies(secure_frontend_privacy_tests …) lists strata-supervision-fake-engine, secure_frontend_connect_audit, secure_frontend_resolve_probe too.
- I16/I19: already ruled in the plan (record-file tripwire; no --release until SP7).
