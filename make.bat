@echo off
rem make.bat - build every binary and engine into one runtime-only folder (make.py; Windows).
rem   make.bat --out D:\SecureStrata [--ggml-dir G:\Strata\third_party\llama.cpp] [--cuda-arch native] [--without maya]
rem See `make.bat --help`. Runs make.py with the repository's dev Python when there is one, else py -3 or python.
setlocal
set "HERE=%~dp0"
if exist "%HERE%.venv-dev\Scripts\python.exe" (
  "%HERE%.venv-dev\Scripts\python.exe" "%HERE%make.py" %*
) else (
  where py >nul 2>nul && (py -3 "%HERE%make.py" %*) || (python "%HERE%make.py" %*)
)
exit /b %errorlevel%
