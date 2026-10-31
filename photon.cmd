@echo off
rem photon on Windows: run the Python CLI in scripts\photon with Python 3.9+.
rem macOS and Linux use the photon shell script beside this file.
setlocal
set "PHOTON_PY="
rem The WindowsApps python.exe is a Microsoft Store redirect, not an interpreter.
rem An older python on PATH is skipped, the same as the photon shell script does.
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PHOTON_PY (
    echo %%P| findstr /i /c:"\WindowsApps\" >nul || (
        "%%P" -c "import sys; sys.exit(sys.version_info < (3, 9))" >nul 2>nul && set "PHOTON_PY="%%P""
    )
)
if not defined PHOTON_PY (
    py -3 -c "import sys; sys.exit(sys.version_info < (3, 9))" >nul 2>nul && set "PHOTON_PY=py -3"
)
rem A fresh machine: photon setup installs Python first, since everything else runs on it.
if not defined PHOTON_PY if /i "%~1"=="setup" (
    echo Python is missing. Installing Python 3.13 with winget first.
    winget install --id Python.Python.3.13 -e --accept-package-agreements --accept-source-agreements --override "/quiet InstallAllUsers=0 PrependPath=1 Include_test=0"
    if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "PHOTON_PY="%LOCALAPPDATA%\Programs\Python\Python313\python.exe""
)
if not defined PHOTON_PY (
    echo photon needs Python 3.9 or newer. Run photon setup to install it with winget,
    echo or install it from https://www.python.org/downloads/ and open a new terminal.
    exit /b 1
)
%PHOTON_PY% "%~dp0scripts\photon" %*
exit /b %ERRORLEVEL%
