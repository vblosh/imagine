@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
python "%SCRIPT_DIR%run_ui_tests.py" %*
if errorlevel 1 exit /b %errorlevel%
