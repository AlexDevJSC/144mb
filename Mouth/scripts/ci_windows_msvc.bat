@echo off
setlocal
rem CI: locate available MSVC toolchain instead of assuming VS 2022.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found.
  exit /b 1
)
for /f "delims=" %%V in ('"%VSWHERE%" -latest -products * -property installationPath') do set "VSPATH=%%V"
if not defined VSPATH (
  echo ERROR: no Visual Studio installation detected.
  exit /b 1
)
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: vcvars64.bat not found under %VSPATH%
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
call "%~dp0build_windows_msvc.bat"
exit /b %errorlevel%
