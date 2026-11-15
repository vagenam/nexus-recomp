@echo off
setlocal
cd /d "%~dp0"

if exist "%~dp0tools\.portable\python.exe" (
    set "PYTHON_EXE=%~dp0tools\.portable\python.exe"
    set "CMAKE_EXE=%~dp0tools\.portable\Lib\site-packages\cmake\data\bin\cmake.exe"
    set "NINJA_EXE=%~dp0tools\.portable\Scripts\ninja.exe"
    set "PATH=%~dp0tools\.portable;%~dp0tools\.portable\Scripts;%~dp0tools\.portable\Lib\site-packages\ziglang;%PATH%"
) else (
    set "VENV_DIR=%~dp0..\.venv"
    set "CMAKE_EXE=%VENV_DIR%\Lib\site-packages\cmake\data\bin\cmake.exe"
    set "NINJA_EXE=%VENV_DIR%\Scripts\ninja.exe"
    set "PYTHON_EXE=%VENV_DIR%\Scripts\python.exe"
    set "PATH=%VENV_DIR%\Scripts;%PATH%"
)

echo [1/4] Checking CMake build configuration...
if not exist "build-win\build.ninja" goto do_cmake
goto do_build

:do_cmake
echo Configuring CMake project in build-win...
"%CMAKE_EXE%" -S "%~dp0windows" -B "%~dp0build-win" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%~dp0windows/toolchain/zig-windows.cmake" -DSDL2_DIR="%~dp0tools/sdl2-win/lib/cmake/SDL2" -DPython3_EXECUTABLE="%PYTHON_EXE%"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

:do_build
echo [2/4] Building NexusRecomp.exe with Ninja...
"%NINJA_EXE%" -C "%~dp0build-win" -j10 NexusRecomp.exe
if errorlevel 1 (
    echo [ERROR] Build failed!
    exit /b 1
)

echo [3/4] Staging binary and configuration...
if not exist "%~dp0RecompFolder" mkdir "%~dp0RecompFolder"
copy /y "%~dp0build-win\game\NexusRecomp.exe" "%~dp0RecompFolder\NexusRecomp.exe" >nul

echo [4/4] Setting up RecompFolder\settings.txt...
if not exist "%~dp0RecompFolder\settings.txt" (
    if exist "%~dp0settings.template.txt" (
        copy /y "%~dp0settings.template.txt" "%~dp0RecompFolder\settings.txt" >nul
        echo Created default RecompFolder\settings.txt.
    )
) else (
    echo RecompFolder\settings.txt already exists, preserving user settings.
)

echo.
echo ========================================================
echo BUILD SUCCESSFUL!
echo Output staged at: %~dp0RecompFolder\NexusRecomp.exe
echo Settings located at: %~dp0RecompFolder\settings.txt
echo ========================================================
exit /b 0
