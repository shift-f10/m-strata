@echo off
rem Configure the engine with the GLM family DLL ON, then build ONLY the strata_glm target (its deps ggml-cpu/base
rem are already built). First bring-up pass: -Werror is the later STRATA_GLM_WERROR=ON pass.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE=C:\Strata\.venv\Scripts\cmake.exe
set NINJA=C:\Strata\.venv\Scripts\ninja.exe
"%CMAKE%" -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -S C:\multi-strata\engine -B C:\multi-strata\engine\build ^
  -DCMAKE_BUILD_TYPE=Release -DSTRATA_ENABLE_CUDA=ON -DSTRATA_BUILD_TESTS=OFF ^
  -DSTRATA_Q6K_EXPERTS=ON -DSTRATA_MMQ_KQUANTS=ON -DCMAKE_CUDA_ARCHITECTURES=89 ^
  -DSTRATA_BUILD_GLM=ON -DSTRATA_GLM_WERROR=OFF ^
  "-DCMAKE_CUDA_COMPILER=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\nvcc.EXE" ^
  "-DCUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0" ^
  -DSTRATA_GGML_DIR=C:\Strata\third_party\llama.cpp || (echo CONFIGURE_FAILED & exit /b 1)
echo CONFIGURE_OK
"%CMAKE%" --build C:\multi-strata\engine\build --target strata_glm -j 12 -- -k 0
echo BUILD_EXIT %errorlevel%
