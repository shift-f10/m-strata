@echo off
setlocal
set U=%USERNAME%
echo user=%U%

rem --- marker .gguf INSIDE the pack dir: the frontend enumerates it there and composes --glm-pack = the pack dir;
rem     the engine still resolves the real shards at <pack>/../ (Maya-S-v2-IQ2_XXS). Created before the ACL so it is covered.
> "C:\Maya-data\models\Maya-S-v2-IQ2_XXS\pack\glm-5.3-flash.gguf" echo GGUF-pack-marker

rem --- owner-only ACLs (remove the null/all-users DACL; keep owner + SYSTEM) ---
icacls "C:\Maya-data\models" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /T /C >nul
echo models ACL rc=%errorlevel%

rem --- owner-only copy of the engine (so engine.executable passes the not-writable-by-others gate) ---
if not exist "C:\multi-strata\engine\frontend-engine" mkdir "C:\multi-strata\engine\frontend-engine"
copy /Y "C:\multi-strata\engine\build\multiengine.exe" "C:\multi-strata\engine\frontend-engine\multiengine.exe" >nul
copy /Y "C:\multi-strata\engine\build\multiengine_glm.dll" "C:\multi-strata\engine\frontend-engine\multiengine_glm.dll" >nul
icacls "C:\multi-strata\engine\frontend-engine" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /T /C >nul
echo engine ACL rc=%errorlevel%

rem --- owner-only state + secret dirs ---
if not exist "C:\multi-strata\engine\glm-state" mkdir "C:\multi-strata\engine\glm-state"
if not exist "C:\multi-strata\engine\glm-secret" mkdir "C:\multi-strata\engine\glm-secret"
icacls "C:\multi-strata\engine\glm-state" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /T /C >nul
icacls "C:\multi-strata\engine\glm-secret" /inheritance:r /grant:r "%U%:(OI)(CI)F" "SYSTEM:(OI)(CI)F" /T /C >nul
echo state/secret ACL rc=%errorlevel%
echo SETUP_DONE
