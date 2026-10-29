@echo off
rem Serve OUR compiled secure_frontend on raon1 (Raon-1-mini) via strata_glm, Strata-data pack. NOT serve.server.
"C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-unified-server.exe" --expert --config "C:\multi-strata\engine\fe-config\raon-frontend.json"
echo SERVER_EXIT %errorlevel%
