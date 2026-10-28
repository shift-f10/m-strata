@echo off
rem Serve OUR compiled secure_frontend in MANAGED mode with the Qwen3.8-Flash-Next IQ3_S, Gemma 4 and flint Q8_0 cards
rem and NO model started: load one from the Models tab. Each card names its engine deployment (cards\*.json): the
rem launcher multiengine.exe with its family libraries, copied from engine\build-g (flint: engine\frontend-engine-flint,
rem multiengine.exe + multiengine_flint.dll; its tokenizer pack C:\multi-strata\out\flint-pack\tokenizer).
"C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-secure-server.exe" --config "C:\multi-strata\engine\fe-config\multi-test.json"
echo SERVER_EXIT %errorlevel%
