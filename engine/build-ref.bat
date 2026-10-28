@echo off
rem Build KRAFTON's llama.cpp (raon-1-mini-preview) as the per-seam reference oracle.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set CMAKE=G:\Strata\.venv\Lib\site-packages\cmake\data\bin\cmake.exe
set NINJA=G:\Strata\.venv\Scripts\ninja.exe
set REF=G:\Strata-data\krafton-llama-ref
"%CMAKE%" -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -S %REF% -B %REF%\build ^
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89 -DLLAMA_CURL=OFF ^
  "-DCMAKE_CUDA_COMPILER=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\nvcc.EXE" ^
  "-DCUDAToolkit_ROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0" || (echo CFG_FAIL & exit /b 1)
echo CFG_OK
"%CMAKE%" --build %REF%\build --target llama-eval-callback llama-cli -j 12
echo REF_BUILD_EXIT %errorlevel%
