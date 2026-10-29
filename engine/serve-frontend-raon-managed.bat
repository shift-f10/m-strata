@echo off
rem Serve OUR compiled secure_frontend in MANAGED mode (Models + Download + Monitor tabs) with the raon1 card.
"C:\multi-strata\secure_frontend\out\build\windows-msvc\Release\strata-unified-server.exe" --config "C:\multi-strata\engine\fe-config\raon-managed.json"
echo SERVER_EXIT %errorlevel%
