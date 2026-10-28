@echo off
rem Hardening pass: reconfigure with STRATA_GLM_WERROR=ON (/WX on the C++ host compile, matching Maya/Strata) and
rem rebuild strata_glm. Only the C++ TUs recompile under /WX; the cached .cu objects are unchanged.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE=C:\Strata\.venv\Scripts\cmake.exe
set NINJA=C:\Strata\.venv\Scripts\ninja.exe
"%CMAKE%" -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -S C:\multi-strata\engine -B C:\multi-strata\engine\build ^
  -DSTRATA_GLM_WERROR=ON || (echo CONFIGURE_FAILED & exit /b 1)
echo CONFIGURE_OK
"%CMAKE%" --build C:\multi-strata\engine\build --target strata_glm -j 12 -- -k 0
echo WERROR_BUILD_EXIT %errorlevel%
