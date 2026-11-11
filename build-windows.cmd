@echo off
setlocal
set "QT_PREFIX=%~1"
if not defined QT_PREFIX set "QT_PREFIX=%QT_ROOT_DIR%"
if not defined QT_PREFIX goto usage
for %%P in ("%QT_PREFIX%") do set "QT_PREFIX=%%~fP"
if not exist "%QT_PREFIX%\lib\cmake\Qt5\Qt5Config.cmake" goto usage
if not exist "%QT_PREFIX%\bin\windeployqt.exe" goto usage
where cmake.exe >nul 2>&1
if errorlevel 1 (
  echo CMake was not found on PATH.
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing_vs
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT goto missing_vs
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

pushd "%~dp0" || exit /b 1
set "BUILD_DIR=build\windows-x64"
set "APP_DIR=output\x64\Release"
set "PATH=%QT_PREFIX%\bin;%PATH%"
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DQT_DESIRED_VERSION=5 "-DCMAKE_PREFIX_PATH=%QT_PREFIX%" || goto failed
cmake --build "%BUILD_DIR%" --config Release --parallel || goto failed
if not exist "%APP_DIR%\QtScrcpy.exe" goto failed

cmake -E copy_directory keymap "%APP_DIR%\keymap" || goto failed
cmake -E copy_directory config "%APP_DIR%\config" || goto failed
cmake -E copy LICENSE "%APP_DIR%\LICENSE.txt" || goto failed
"%QT_PREFIX%\bin\windeployqt.exe" --release --no-translations --compiler-runtime "%APP_DIR%\QtScrcpy.exe" || goto failed
if not exist "%APP_DIR%\platforms\qwindows.dll" goto failed
if not exist "%APP_DIR%\adb.exe" goto failed
if not exist "%APP_DIR%\scrcpy-server" goto failed

echo Built: %CD%\%APP_DIR%\QtScrcpy.exe
popd
exit /b 0

:usage
echo Usage: build-windows.cmd "C:\Qt\5.15.2\msvc2019_64"
echo Pass the MSVC x64 Qt 5.15.2 prefix or set QT_ROOT_DIR.
exit /b 2

:missing_vs
echo Install Visual Studio 2022 with the Desktop development with C++ workload.
exit /b 1

:failed
echo Build or deployment failed. See the error above.
popd
exit /b 1
