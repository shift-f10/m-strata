@echo off
setlocal
set U=%USERNAME%
set PACK=G:\Strata-data\models\raon1_q8\pack
set ENG=C:\multi-strata\engine\frontend-engine-raon
rem --- the enumeration marker .gguf lives in the pack dir (created already); the engine resolves the real
rem     Q8_0 gguf at <pack>\.. via native_experts.txt.
> "%PACK%\raon1-mini.gguf" echo GGUF-pack-marker
rem --- owner-only copy of the raon1 engine (exe + its two DLLs) so engine.executable passes the gate ---
if not exist "%ENG%" mkdir "%ENG%"
copy /Y "C:\multi-strata\engine\build-g\multiengine.exe" "%ENG%\multiengine.exe" >nul
copy /Y "C:\multi-strata\engine\build-g\multiengine_glm.dll" "%ENG%\multiengine_glm.dll" >nul
copy /Y "C:\multi-strata\engine\build-g\strata_kq.dll" "%ENG%\strata_kq.dll" >nul
icacls "%ENG%" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /grant "%U%:F" "SYSTEM:F" /T /C >nul
echo engine ACL rc=%errorlevel%
rem --- owner-only state + secret dirs; reuse the GLM api token ---
if not exist "C:\multi-strata\engine\raon-state" mkdir "C:\multi-strata\engine\raon-state"
if not exist "C:\multi-strata\engine\raon-secret" mkdir "C:\multi-strata\engine\raon-secret"
rem The development API key (owner's rule, 2026-10-27): the dev placeholder below is the only key the repository may
rem hold, and every server started for development and tests uses it. Must be >= 32 bytes. Never a real install's key.
<nul set /p="strata-secret-key-dev-placeholder" > "C:\multi-strata\engine\raon-secret\token"
icacls "C:\multi-strata\engine\raon-state"  /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /grant "%U%:F" "SYSTEM:F" /T /C >nul
icacls "C:\multi-strata\engine\raon-secret" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /grant "%U%:F" "SYSTEM:F" /T /C >nul
echo secret ACL rc=%errorlevel%
rem --- owner-only pack dir (small: dense.bin + tokenizer + index; the 30GB gguf in the parent is not a root) ---
icacls "%PACK%" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /grant "%U%:F" "SYSTEM:F" /T /C >nul
echo pack ACL rc=%errorlevel%
echo SETUP_DONE
