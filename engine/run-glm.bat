@echo off
rem End-to-end GLM through the single multiengine.exe: the family switch sees --glm-pack, delay-loads multiengine_glm.dll
rem (found beside the exe), and runs Glm5Model (load_pack -> prefill -> greedy decode). Token ids print one per line.
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\x64;%PATH%
"C:\multi-strata\engine\build\multiengine.exe" --glm-pack C:\Strata-data\packs\glm53 --tokens "9707,11,1879,0" --max-new 8 --gpu 0
echo RUN_EXIT %errorlevel%
