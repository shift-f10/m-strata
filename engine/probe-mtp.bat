@echo off
rem Load GLM in --serve with --spec 4, print the engine's INFO (experts=.. mtp=.. dflash=..) readiness line, then QUIT.
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\x64;%PATH%
echo QUIT| "C:\multi-strata\engine\build\multiengine.exe" --serve --glm-pack C:\Strata-data\packs\glm53 --max-context 4096 --expert-cache auto --prefill auto --kv-streaming on --spec 4 --expert-profile C:\project-maya\data\expert-profile-glm.bin
echo PROBE_EXIT %errorlevel%
