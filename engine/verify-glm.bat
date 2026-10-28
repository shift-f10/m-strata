@echo off
rem Build the full multiengine.exe with the GLM family DLL wired in (STRATA_BUILD_GLM already ON in the cache), then
rem verify the DLL's export surface and that --families now lists glm5-next.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE=C:\Strata\.venv\Scripts\cmake.exe
"%CMAKE%" --build C:\multi-strata\engine\build --target strata -j 12 || (echo STRATA_BUILD_FAILED & exit /b 1)
echo STRATA_BUILD_OK
copy /Y C:\multi-strata\engine\build\third_party\project-maya\multiengine_glm.dll C:\multi-strata\engine\build\multiengine_glm.dll >nul
echo === multiengine_glm.dll EXPORTS (expect only glm_main) ===
dumpbin /EXPORTS C:\multi-strata\engine\build\third_party\project-maya\multiengine_glm.dll | findstr /R /C:" glm_main" /C:"ordinal" /C:"number of"
echo === strata --families ===
C:\multi-strata\engine\build\multiengine.exe --families
echo VERIFY_EXIT %errorlevel%
