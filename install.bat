@echo off
rem install.bat - from a fresh Windows 10/11 PC to a built M-Strata in one step: installs what the build needs where it
rem is missing (Git, Python, Visual Studio Build Tools with C++, the CUDA Toolkit, with winget), sets up CMake/Ninja,
rem llama.cpp and the WebView2 SDK, then runs make. The work is install.ps1 beside it; this only starts it.
rem   install.bat                                   build into ..\M-Strata-app (email alerts and web search)
rem   install.bat -Out D:\Apps\M-Strata -With email,search,window
rem   install.bat -Plan                             only say what it would do
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
exit /b %errorlevel%
