@echo off
rem TEST-ONLY (Gemma 4 plans 2-3 Task 8, spec D7): builds engines\gemma4\tools\g4_llama_dump.cpp against the static
rem libraries of the test-only llama.cpp build (out\llama-server-3cf03257, pinned 3cf03257; read only, never rebuilt
rem here) into out\g4-llama-dump\g4_llama_dump.exe. Never shipped.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "SRC=C:\multi-strata\out\engine-cuda-v0.2.2\_deps\strata_llamacpp-src"
set "LLAMA_LIB=C:\multi-strata\out\llama-server-3cf03257"
set "CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\lib\x64"
set "OUT=C:\multi-strata\out\g4-llama-dump"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /EHsc /MD /std:c++17 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /I"%SRC%\include" /I"%SRC%\ggml\include" /Fo"%OUT%\\" ^
  C:\multi-strata\engines\gemma4\tools\g4_llama_dump.cpp /Fe"%OUT%\g4_llama_dump.exe" /link ^
  "%LLAMA_LIB%\src\llama.lib" "%LLAMA_LIB%\ggml\src\ggml.lib" "%LLAMA_LIB%\ggml\src\ggml-cpu.lib" "%LLAMA_LIB%\ggml\src\ggml-cuda\ggml-cuda.lib" ^
  "%LLAMA_LIB%\ggml\src\ggml-base.lib" "%CUDA%\cudart.lib" "%CUDA%\cublas.lib" "%CUDA%\cublasLt.lib" "%CUDA%\cuda.lib" ^
  kernel32.lib user32.lib advapi32.lib || exit /b 1
