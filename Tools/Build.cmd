@echo off
rem Builds MacZoom with MSBuild and packages the Inno Setup installer into the build directory.
rem Usage: Tools\Build.cmd [BuildDir]   (default: ..\Build\Windows beside this checkout)
rem   ISCC  the Inno Setup compiler (default: Inno Setup 6 under Program Files (x86))
setlocal
set "SOURCE=%~dp0.."
set "BUILD=%~1"
if "%BUILD%"=="" set "BUILD=%~dp0..\..\Build\Windows"
if "%ISCC%"=="" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if "%MSBUILD%"=="" (
    echo %~nx0: no MSBuild found by "%VSWHERE%"; install the VS 2022 C++ build tools 1>&2
    exit /b 2
)
if not exist "%ISCC%" (
    echo %~nx0: Inno Setup compiler not found at "%ISCC%"; set ISCC to its path 1>&2
    exit /b 3
)

"%MSBUILD%" "%SOURCE%\MacZoom.sln" /m /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 "/p:BuildRoot=%BUILD%" || exit /b 1
"%ISCC%" /Q "/DBuildRoot=%BUILD%" "%SOURCE%\Tools\Installer\MacZoom.iss" || exit /b 1
