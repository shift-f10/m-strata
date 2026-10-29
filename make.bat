@echo off
rem make.bat - build every binary and engine into one runtime-only folder (make.py; Windows).
rem   make.bat --out D:\M-Strata-app [--ggml-dir D:\llama.cpp] [--cuda-arch auto] [--without maya] [--with email,search]
rem See `make.bat --help`. Runs make.py with the repository's dev Python when there is one, else py -3 or python - one
rem of them only, so a make that fails is never run a second time (no `a && b || c`, which runs c when b fails).
setlocal
set "HERE=%~dp0"
if not exist "%HERE%.venv-dev\Scripts\python.exe" goto system_python
"%HERE%.venv-dev\Scripts\python.exe" "%HERE%make.py" %*
exit /b %errorlevel%

:system_python
where py >nul 2>nul
if errorlevel 1 goto plain_python
py -3 "%HERE%make.py" %*
exit /b %errorlevel%

:plain_python
python "%HERE%make.py" %*
exit /b %errorlevel%
