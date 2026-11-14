@echo off
rem Runs the SCDA P2P installer (Install.ps1) in the folder this file lives in.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*
if errorlevel 1 pause
