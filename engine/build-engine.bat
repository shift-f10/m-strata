@echo off
rem Build our ported engine (from Strata source) in our repo. Mirrors Strata's build-strata.bat recipe,
rem but with S/B pointed at C:\multi-strata\engine and ggml referenced from the vendored llama.cpp.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE=C:\Strata\.venv\Scripts\cmake.exe
set NINJA=C:\Strata\.venv\Scripts\ninja.exe
"%CMAKE%" -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -S C:\multi-strata\engine -B C:\multi-strata\engine\build ^
  -DCMAKE_BUILD_TYPE=Release -DSTRATA_ENABLE_CUDA=ON -DSTRATA_BUILD_TESTS=OFF ^
  -DSTRATA_Q6K_EXPERTS=ON -DSTRATA_MMQ_KQUANTS=ON -DCMAKE_CUDA_ARCHITECTURES=89 ^
  "-DCMAKE_CUDA_COMPILER=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\nvcc.EXE" ^
  "-DCUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0" ^
  -DSTRATA_GGML_DIR=C:\Strata\third_party\llama.cpp || exit /b 1
"%CMAKE%" --build C:\multi-strata\engine\build --target strata -j 12 || exit /b 1
echo BUILD_OK
