@echo off
rem Build multiengine and its family libraries on the G: toolchain (Strata moved C:->G: 2026-10-25).  WERROR stays off
rem for the upstream and Maya code, as their own builds have it; our launcher and Gemma 4 sources build with /WX.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
rem NB: the venv's Scripts\cmake.exe launcher broke in the C:->G: move; the real binary under site-packages works.
set CMAKE=G:\Strata\.venv\Lib\site-packages\cmake\data\bin\cmake.exe
set NINJA=G:\Strata\.venv\Scripts\ninja.exe
"%CMAKE%" -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -S C:\multi-strata\engine -B C:\multi-strata\engine\build-g ^
  -DCMAKE_BUILD_TYPE=Release -DSTRATA_ENABLE_CUDA=ON -DSTRATA_BUILD_TESTS=OFF ^
  -DSTRATA_Q6K_EXPERTS=ON -DSTRATA_MMQ_KQUANTS=ON -DCMAKE_CUDA_ARCHITECTURES=89 ^
  -DSTRATA_BUILD_MAYA=ON -DSTRATA_BUILD_RAON1=ON -DSTRATA_GLM_WERROR=OFF ^
  "-DCMAKE_CUDA_COMPILER=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\nvcc.EXE" ^
  "-DCUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0" ^
  -DSTRATA_GGML_DIR=G:\Strata\third_party\llama.cpp || (echo CONFIGURE_FAILED & exit /b 1)
echo CONFIGURE_OK
rem OUR engine = multiengine.exe (the launcher) + its family libraries multiengine_strata.dll (upstream's engine),
rem multiengine_maya.dll / multiengine_maya_ds4.dll / multiengine_maya_mm3.dll (Project Maya's three programs, built in
rem build-g\maya as Maya's own project), multiengine_raon1.dll and multiengine_gemma4.dll, all built beside the exe
rem (cmake/multiengine.cmake).  The target `multiengine` builds them all; upstream's own `strata` target still builds
rem strata.exe as upstream does, and build-g\maya builds Maya's own maya.exe / maya-ds4.exe / maya-mm3.exe on request.
"%CMAKE%" --build C:\multi-strata\engine\build-g --target multiengine -j 12 -- -k 0
echo BUILD_EXIT %errorlevel%
