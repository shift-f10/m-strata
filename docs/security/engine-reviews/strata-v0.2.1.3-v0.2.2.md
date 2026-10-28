# Engine diff review: Niko1221/Strata v0.2.1.3 (cbfd5b92) -> v0.2.2 (011c9e60)

Findings: 58 (deps 6, digest 6, js-exec 10, load 1, net 3, py-exec 15, py-load 12, py-net 5); unruled: 0; refused: 0.

| id | rule | where | ruling | text |
| --- | --- | --- | --- | --- |
| `28f6e73e06cf0f40` | deps | `apps/desktop/package.json:0` | benign | `` |
| `5b1230e2cbcf0de9` | deps | `apps/desktop/src-tauri/Cargo.lock:0` | benign | `` |
| `419793c74411379a` | deps | `apps/desktop/src-tauri/Cargo.toml:0` | benign | `` |
| `577933c401dfcb1e` | deps | `apps/host/package-lock.json:0` | benign | `` |
| `daa0dc613a0ca55c` | deps | `apps/host/package.json:0` | benign | `` |
| `403a7c0449e658ca` | js-exec | `apps/host/tests/host-ports.test.mjs:4` | benign | `import { spawn } from 'node:child_process';` |
| `60968bca6c7b018c` | js-exec | `apps/host/tests/host-ports.test.mjs:36` | benign | `const child = spawn(process.execPath, ['server/index.mjs'], { cwd, env: { ...process.env, STRATA_HOME: home, STRATA_PORT` |
| `64bbd7f2ae5278d5` | js-exec | `apps/host/tests/model-folder.test.mjs:3` | benign | `import { execFileSync, spawnSync } from 'node:child_process';` |
| `3a73e6d503777468` | js-exec | `apps/host/tests/model-folder.test.mjs:73` | benign | `execFileSync('subst', [`${letter}:`, dir]);` |
| `04dbad885786fa0e` | js-exec | `apps/host/tests/model-folder.test.mjs:74` | benign | `t.after(() => { try { execFileSync('subst', [`${letter}:`, '/D']); } catch { /* gone */ } });` |
| `eae68b627a33a727` | js-exec | `apps/host/tests/model-folder.test.mjs:85` | benign | `function readdirNames(dir) { return execFileSync('cmd', ['/c', 'dir', '/b', dir]).toString().split(/\r?\n/).filter(Boole` |
| `bd34ef2445873f08` | js-exec | `apps/host/tests/model-folder.test.mjs:94` | benign | `const run = spawnSync(process.execPath, ['--input-type=module', '-e', `const store=await import('./server/store.mjs');co` |
| `32020116b57556b2` | js-exec | `apps/host/tests/model-folder.test.mjs:129` | benign | `const readdirSyncSafe = folder => execFileSync(process.execPath, ['-e', `console.log(require('fs').readdirSync(${JSON.st` |
| `d1b9176f8a546950` | js-exec | `apps/host/tests/models-directory-fallback.test.mjs:3` | benign | `import { spawnSync } from 'node:child_process';` |
| `b247c19d1b07a830` | js-exec | `apps/host/tests/models-directory-fallback.test.mjs:13` | benign | `const run = spawnSync(process.execPath, ['--input-type=module', '-e', code], { cwd, encoding: 'utf8', env: { ...process.` |
| `30486dff2251e553` | digest | `bench/results/2026-10-20-0.2.1-bench/p2003-rtx3060/run1-release-archive-as-shipped/provenance/corpus-manifest.sha256:0` | benign | `` |
| `d5c3f616e5cf3f0f` | digest | `bench/results/2026-10-20-0.2.1-bench/p2003-rtx3060/run2-with-commit-and-optional-concurrency/provenance/corpus-manifest.sha256:0` | benign | `` |
| `e2aee1224059e8a1` | digest | `bench/results/2026-10-20-0.2.1-bench/p2003-rtx3060/run3-with-fixes/provenance/corpus-manifest.sha256:0` | benign | `` |
| `8a15ab2aee605b1d` | digest | `bench/results/2026-10-20-0.2.1-bench/z440-2xa4000/run1-release-archive-as-shipped/provenance/corpus-manifest.sha256:0` | benign | `` |
| `139e4a3aadaaccd4` | digest | `bench/results/2026-10-20-0.2.1-bench/z440-2xa4000/run2-with-commit-and-optional-concurrency/provenance/corpus-manifest.sha256:0` | benign | `` |
| `2336b9b534421f19` | digest | `bench/results/2026-10-20-0.2.1-bench/z440-2xa4000/run3-with-fixes/provenance/corpus-manifest.sha256:0` | benign | `` |
| `c81b5fc742bc8fce` | net | `include/strata/kernels/cpu/pool_plan.hpp:97` | benign | `else if (span) p.limit = "every CPU socket (STRATA_NUMA_SPLIT)";` |
| `105a861b31725515` | py-exec | `packaging/package_terminal.py:292` | benign | `out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip().lower` |
| `df23ca57813f5ce1` | deps | `requirements-nvfp4-local.txt:0` | accepted | `` |
| `ba58663e4f196c0c` | py-net | `setup.py:2463` | accepted | `req = urllib.request.Request(url, headers={"User-Agent": "strata-setup", "Range": f"bytes={pos}-{end}"})` |
| `54e2897755b943ca` | py-net | `setup.py:2485` | accepted | `except urllib.error.HTTPError as e:` |
| `c5b2b0d20bc82846` | py-net | `setup.py:2492` | accepted | `except (OSError, http.client.HTTPException):` |
| `539021aaa9a9fc02` | py-net | `setup.py:2500` | accepted | `except (OSError, http.client.HTTPException) as e:` |
| `ccef38f1be8d38d5` | py-net | `setup.py:2517` | accepted | `except (_NoRange, _Changed, OSError, http.client.HTTPException):` |
| `631ce9c80e658b14` | py-exec | `setup.py:4111` | accepted | `run = subprocess.run([sys.executable, str(ROOT / "strata.py"), "update", "--no-control", "--output", "json"],` |
| `08c3299f4d2eb3f4` | py-exec | `setup.py:4113` | accepted | `except (OSError, subprocess.SubprocessError) as e:` |
| `bf668f29e17cc445` | load | `src/kernels/cpu/pool.cpp:1460` | benign | `static const SetDesc fn = (SetDesc) GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");` |
| `e27e0ad332ac7554` | net | `src/kernels/cpu/pool_tasks_test.cpp:246` | benign | `// the DEFAULT worker count under the split spans every socket (pool_plan.hpp span_packages): on a 2-socket` |
| `97da707ca1cac6a6` | net | `src/kernels/cpu/pool_tasks_test.cpp:254` | benign | `std::printf("pool tasks: default pool under the split: %d workers on %d socket(s), split %s%s (%s)\n",` |
| `f63abbedc39c4592` | py-exec | `strata_control/engine_component.py:554` | benign | `done = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command",` |
| `7e80c5057ce12d49` | py-exec | `strata_control/engine_component.py:556` | benign | `capture_output=True, text=True, timeout=15, stdin=subprocess.DEVNULL,` |
| `2e9d370f8b5fd8c6` | py-exec | `strata_control/engine_component.py:558` | benign | `except (OSError, subprocess.SubprocessError):` |
| `967c5caa207d67bb` | py-load | `strata_control/gpu_roles.py:434` | benign | `code = ("import ctypes,sys,os\n"` |
| `21b4fea5e2e3a777` | py-load | `strata_control/gpu_roles.py:436` | benign | `"lib=ctypes.CDLL('nvcuda.dll' if os.name=='nt' else 'libcuda.so.1')\n"` |
| `e35918425f55d057` | py-load | `strata_control/gpu_roles.py:438` | benign | `"n=ctypes.c_int()\n"` |
| `9429383d88cc9ed4` | py-load | `strata_control/gpu_roles.py:439` | benign | `"assert lib.cuDeviceGetCount(ctypes.byref(n))==0\n"` |
| `36b0991d79dfa2e6` | py-load | `strata_control/gpu_roles.py:441` | benign | `"    d=ctypes.c_int()\n"` |
| `30f344b6eabc32bd` | py-load | `strata_control/gpu_roles.py:442` | benign | `"    assert lib.cuDeviceGet(ctypes.byref(d),i)==0\n"` |
| `3fdee6638166bc17` | py-load | `strata_control/gpu_roles.py:443` | benign | `"    sm=ctypes.c_int(); mhz=ctypes.c_int()\n"` |
| `8d4e276779b8ef9a` | py-load | `strata_control/gpu_roles.py:444` | benign | `"    lib.cuDeviceGetAttribute(ctypes.byref(sm),16,d)\n"` |
| `be5af9297746570d` | py-load | `strata_control/gpu_roles.py:445` | benign | `"    lib.cuDeviceGetAttribute(ctypes.byref(mhz),13,d)\n"` |
| `6ee381f20ba3e0b2` | py-exec | `strata_control/gpu_roles.py:451` | benign | `r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=timeout, env=env)` |
| `567794e9dfe5c218` | py-exec | `strata_control/gpu_roles.py:458` | benign | `except (OSError, ValueError, subprocess.SubprocessError):` |
| `46c62e85ff35c34b` | py-load | `strata_control/winproc.py:49` | benign | `ctypes.POINTER(wintypes.DWORD)]` |
| `97d52408d8778430` | py-load | `strata_control/winproc.py:109` | benign | `buffer = ctypes.create_unicode_buffer(size.value)` |
| `0792c089185534a7` | py-load | `strata_control/winproc.py:110` | benign | `if not _k32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):` |
| `ca7a99c6d09e36fb` | py-exec | `tests/terminal/test_install_scripts_env.py:45` | accepted | `done = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PS1)],` |
| `e1f90396a54fd21e` | py-exec | `tools/idle_gpu_ab.py:29` | benign | `self.proc = subprocess.Popen(["nvidia-smi", "--query-gpu=index,utilization.gpu,power.draw", "--format=csv,noheader,nouni` |
| `04a9136c75414d5e` | py-exec | `tools/idle_gpu_ab.py:30` | benign | `"-lms", "200"], stdout=subprocess.PIPE, text=True)` |
| `9d90d977310dfd58` | py-exec | `tools/idle_gpu_ab.py:236` | benign | `p = subprocess.Popen([exe, "--serve", *args], cwd=cfg.get("cwd"), stdin=subprocess.PIPE, stdout=subprocess.PIPE,` |
| `a7bc41ea0f4e7841` | py-exec | `tools/test_docker_entrypoint.py:231` | benign | `r = subprocess.run([SH, str(self.script)], env=self.env(STUB_BACKEND="cuda", STRATA_DEV_DIR=str(dev), MODEL="Q2_0"),` |
| `6f43fb11c09cc55b` | py-exec | `tools/test_docker_entrypoint.py:243` | benign | `r = subprocess.run([SH, str(self.script)], env=self.env(MODEL="IQ3_XXS", MODEL_PATH=str(missing)), capture_output=True, ` |
| `dbe28a5cb8c90c60` | py-exec | `tools/test_docker_entrypoint.py:249` | benign | `r = subprocess.run([SH, str(self.script)], env=self.env(MODEL="IQ3_XXS", MODEL_PATH=str(empty)), capture_output=True, te` |
