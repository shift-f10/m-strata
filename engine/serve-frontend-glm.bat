@echo off
rem Serve OUR compiled secure_frontend (strata-secure-server --expert): its own chat page + api-key login + engine
rem supervisor, fronting the single multiengine.exe on GLM from the Maya-data pack. NOT serve.server / the Strata web UI.
"C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-secure-server.exe" --expert --config "C:\multi-strata\engine\fe-config\glm-frontend.json"
