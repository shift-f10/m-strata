@echo off
rem Serve OUR engine on GLM via the reviewed serve layer. serve.server launches multiengine.exe --serve --glm-pack ...
rem (the family switch routes --glm-pack to the strata_glm DLL's glm_pack_generate driver) and speaks GEN/T.
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\x64;%PATH%
cd /d C:\Strata
.venv\Scripts\python.exe -m serve.server --engine strata --config C:\multi-strata\engine\our-glm.json --port 8080
