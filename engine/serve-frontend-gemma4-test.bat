@echo off
rem Serve OUR compiled secure_frontend in MANAGED mode with the Gemma 4 card only and NO model started: load it from
rem the Models tab. The engine is multiengine.exe (Gemma 4 as its strata_gemma4 library) copied to
rem engine\frontend-engine-gemma4. No glm block: its keys make the server refuse every non-GLM card.
"C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-secure-server.exe" --config "C:\multi-strata\engine\fe-config\gemma4-test.json"
echo SERVER_EXIT %errorlevel%
