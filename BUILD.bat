@echo off
setlocal
cmake -S "%~dp0." -B "%~dp0build" -A x64
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0build" --config Release --parallel
if errorlevel 1 exit /b %errorlevel%
cmake --install "%~dp0build" --config Release --prefix "%~dp0dist"
exit /b %errorlevel%
