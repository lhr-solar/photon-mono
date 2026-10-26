@echo off
rem Called from PowerShell 7, Windows PowerShell would inherit pwsh's module path and fail
rem to load its own cmdlets (Get-FileHash). Unset, it builds its default path.
set "PSModulePath="
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\photon.ps1" %*
