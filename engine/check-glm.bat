@echo off
set FE=C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-secure-server.exe
"%FE%" --check --expert ^
  --engine "C:\multi-strata\engine\build\multiengine.exe" ^
  --profile glm53 ^
  --models-root "C:\Maya-data\models" ^
  --model-id "GLM-5.3-Flash-Maya-S-v2-IQ2_XXS-00001-of-00003" ^
  --tokenizer-pack "C:\Maya-data\models\Maya-S-v2-IQ2_XXS\pack\tokenizer" ^
  --state-dir "C:\multi-strata\engine\glm-state" ^
  --secret-root "C:\multi-strata\engine\glm-state" ^
  --host 127.0.0.1 --port 8080
echo CHECK_EXIT %errorlevel%
