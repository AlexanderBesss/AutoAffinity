@echo off
setlocal

set SLN=%~dp0AutoAffinity.slnx
set OUT=%~dp0x64\Release\AutoAffinity.exe

:: Find MSBuild via vswhere (works even outside a Developer Command Prompt)
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe

if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found.
    echo         Install Visual Studio 2022 with the "Desktop development with C++" workload.
    goto :end
)

for /f "usebackq delims=" %%i in (
    `"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`
) do set MSBUILD=%%i

if not defined MSBUILD (
    echo [ERROR] MSBuild not found via vswhere.
    goto :end
)

echo [*] MSBuild : %MSBUILD%
echo [*] Solution: %SLN%
echo [*] Building Release x64 ...
echo.

"%MSBUILD%" "%SLN%" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
set BUILD_ERR=%errorlevel%

echo.
if %BUILD_ERR% neq 0 (
    echo [ERROR] Build failed ^(exit code %BUILD_ERR%^).
) else (
    echo [OK] Build succeeded.
    echo      Output: %OUT%
)

:end
echo.
pause
