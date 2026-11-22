@echo off
rem PixInsight Chinese installer (GUI entry)
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0install_gui.ps1"
exit /b
