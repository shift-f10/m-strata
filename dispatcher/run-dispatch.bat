@echo off
rem Our engine's dispatcher: one API on :8080 that spins up the right family backend on demand
rem (qwen -> Strata lineage, glm -> Maya lineage) and swaps when the requested model's family changes.
rem Lazy by default: the first request for a family loads that backend (tens of GB, 1-3 min cold).
rem Pass --prewarm qwen (or glm) to load one at start.
title Our Engine Dispatcher
cd /d "C:\multi-strata\dispatcher"
"C:\Strata\.venv\Scripts\python.exe" dispatch.py --port 8080 %*
if errorlevel 1 pause
