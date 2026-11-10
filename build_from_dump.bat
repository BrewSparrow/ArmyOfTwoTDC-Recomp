@echo off
rem Builds Army of Two: The Devil's Cartel (AO2DC-Recomp) from YOUR OWN disc dump.
rem
rem Put the extracted disc files in the assets\ folder next to this script (default.xex at
rem assets\default.xex), then run this script from a normal Command Prompt or by double-clicking.
rem The dump is only read: nothing in assets\ is changed, moved or deleted.
rem See BUILDING.md for prerequisites and step-by-step instructions.

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "ROOT=%CD%"
set "ASSETS=%ROOT%\assets"
set "BUILD_DIR=%ROOT%\out\build\win-amd64-release"
set "PRESET=win-amd64-release"

echo.
echo === AO2DC-Recomp: build from your own dump ===
echo Project: %ROOT%
echo.

rem ---------------------------------------------------------------- 1. input files
echo [1/7] Checking the dump in assets\ ...
set "MISSING="
for %%F in (default.xex AO4.Main.xex Engine.BuildInfo.xex) do (
    if not exist "%ASSETS%\%%F" set "MISSING=!MISSING! %%F"
)
for %%D in (Data Movies) do (
    if not exist "%ASSETS%\%%D\" set "MISSING=!MISSING! %%D\"
)
if defined MISSING (
    echo.
    echo ERROR: these files or folders are missing from assets\:!MISSING!
    echo.
    echo Copy the whole extracted disc into assets\ so that default.xex is at
    echo   %ASSETS%\default.xex
    echo next to AO4.Main.xex, Engine.BuildInfo.xex, Data\, Movies\ and the other disc files.
    echo See BUILDING.md, "Getting the game files".
    goto :fail
)
echo       All required files found.

rem ---------------------------------------------------------------- 2. hashes
echo [2/7] Verifying SHA-256 hashes of the XEX files ...
set "BAD="
call :checkhash default.xex          76de620af99c6a1f4c15972b726074e9c6219950842a50aecd45971dcf98bf38
call :checkhash AO4.Main.xex         bedc9c918716454fdf6573af154073052272597c8b43329ed24c62b62cc2c137
call :checkhash Engine.BuildInfo.xex 575af0d4d5ceb1e51c1a0fc5c918d35467d02b06fc11dc6fa765c7c5b5cb0e52
if defined BAD (
    echo.
    echo ERROR: unsupported dump. These files do not match the supported release:!BAD!
    echo.
    echo Supported: Army of Two: The Devil's Cartel, Xbox 360 retail disc, Title ID 454109AB,
    echo Media ID 60F8EF51, version 0.0.0.11, with no title update applied.
    echo A different region or version, a title update, or a modified or damaged dump gives
    echo different hashes. Re-dump or re-extract the disc ^(see BUILDING.md^).
    echo Your files were not changed.
    goto :fail
)
echo       Dump matches the supported release.

rem ---------------------------------------------------------------- 3. tools
echo [3/7] Finding the build tools ...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio 2022 ^(Build Tools^) is not installed.
    echo Install "Desktop development with C++" - see BUILDING.md, "Prerequisites".
    goto :fail
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
    echo ERROR: Visual Studio was found but without the C++ x64 build tools.
    echo In the Visual Studio Installer, add "Desktop development with C++".
    goto :fail
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found under "%VSDIR%". Repair the Visual Studio installation.
    goto :fail
)
rem vcvars64 prints a harmless "'vswhere.exe' is not recognized" on some installs; hide its output.
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (
    echo ERROR: setting up the Visual Studio x64 environment failed.
    goto :fail
)
rem LLVM must come first on PATH so CMake picks clang, not MSVC.
if exist "%ProgramFiles%\LLVM\bin\clang.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
call :needtool clang   "LLVM/Clang: winget install LLVM.LLVM"
call :needtool cmake   "CMake 3.25 or newer: winget install Kitware.CMake"
call :needtool ninja   "Ninja: included with the Visual Studio C++ CMake tools, or winget install Ninja-build.Ninja"
call :needtool git     "Git: winget install Git.Git"
if defined TOOLFAIL goto :fail
echo       Tools found.

rem ---------------------------------------------------------------- 4. SDK submodule
echo [4/7] Checking the ReXGlue SDK submodule ...
if not exist "%ROOT%\thirdparty\rexglue-sdk\CMakeLists.txt" (
    echo       SDK missing - downloading it with git submodule update --init --recursive ...
    git -C "%ROOT%" submodule update --init --recursive
    if errorlevel 1 (
        echo ERROR: could not fetch the SDK submodule. Clone with --recursive, or check your
        echo internet connection, then run this script again.
        goto :fail
    )
)
echo       SDK present.

rem ---------------------------------------------------------------- 5. configure + codegen
rem The generated module targets only exist once codegen has run, so the project is configured,
rem codegen is built on its own, and the project is configured again before the full build.
echo [5/7] Configuring and recompiling the game code (needs ~16 GB free RAM; this takes a while) ...
cmake --preset %PRESET%
if errorlevel 1 (
    echo ERROR: CMake configure failed ^(see the messages above^).
    goto :fail
)
cmake --build "%BUILD_DIR%" --target ao2dc_codegen
if errorlevel 1 (
    echo ERROR: code generation failed ^(see the messages above^).
    goto :fail
)

rem ---------------------------------------------------------------- 6. compile
echo [6/7] Compiling ...
cmake --preset %PRESET%
if errorlevel 1 (
    echo ERROR: CMake configure after code generation failed.
    goto :fail
)
cmake --build "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: compiling failed ^(see the messages above^).
    goto :fail
)

rem ---------------------------------------------------------------- 7. result
echo [7/7] Checking the result ...
set "MISSING="
for %%F in ("ArmyOfTwoTDC X360 Recomp.exe" ao2dc_AO4_Main.dll ao2dc_Engine_BuildInfo.dll rexruntime.dll rexgpu-xenos.dll) do (
    if not exist "%BUILD_DIR%\%%~F" set "MISSING=!MISSING! %%~F"
)
if defined MISSING (
    echo ERROR: the build finished but these files are missing from %BUILD_DIR%:!MISSING!
    goto :fail
)
echo.
echo === Done ===
echo Run: "%BUILD_DIR%\ArmyOfTwoTDC X360 Recomp.exe"
echo The game reads its data from assets\; your dump was not changed.
endlocal
if not defined AO2DC_NO_PAUSE pause
exit /b 0

:checkhash
rem %1 = file in assets\, %2 = expected lowercase SHA-256
set "H="
set "AO2DC_HASH_FILE=%ASSETS%\%~1"
for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath $env:AO2DC_HASH_FILE).Hash.ToLower()"`) do set "H=%%H"
if /i "!H!"=="%~2" (
    echo       OK  %~1
) else (
    echo       BAD %~1  got !H!
    set "BAD=!BAD! %~1"
)
exit /b 0

:needtool
where %1 >nul 2>nul
if errorlevel 1 (
    echo ERROR: %1 not found. Install %~2
    set "TOOLFAIL=1"
)
exit /b 0

:fail
echo.
echo Build stopped. Nothing in assets\ was changed.
endlocal
if not defined AO2DC_NO_PAUSE pause
exit /b 1
