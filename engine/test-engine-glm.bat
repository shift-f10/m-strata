@echo off
rem Direct engine test: the single multiengine.exe runs GLM from the Maya-data pack (the engine reads files directly;
rem no ACL / filesystem-format checks at the engine level). Prints decoded token ids.
set PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin\x64;%PATH%
"C:\multi-strata\engine\build\multiengine.exe" --glm-pack "C:\Maya-data\models\Maya-S-v2-IQ2_XXS\pack" --tokens "9707,11,1879,0" --max-new 16
echo RUN_EXIT %errorlevel%
