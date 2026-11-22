@echo off
rem Virtual MPC for Windows: runs windows\vmpc.py with the installed Python (py launcher first).
setlocal
where py >nul 2>nul
if not errorlevel 1 goto py
where python >nul 2>nul
if not errorlevel 1 goto python
echo Python 3 not found. Install it: winget install -e --id Python.Python.3.12
exit /b 1
:py
py -3 "%~dp0vmpc.py" %*
exit /b %errorlevel%
:python
python "%~dp0vmpc.py" %*
exit /b %errorlevel%
