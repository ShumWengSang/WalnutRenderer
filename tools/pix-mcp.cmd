@echo off
rem Launches a PIX MCP server from the newest PIX install over stdio.
rem   pix-mcp.cmd timing   -> PixTimingCapturesMcp.exe (Timing Captures, .wpix)
rem   pix-mcp.cmd dxdmp    -> DxDmpMcp.exe (DirectX dump files, .dxdmp)
rem Nothing may be written to stdout except by the server itself (it speaks JSON-RPC on stdout).
setlocal
set "PIXROOT=%ProgramFiles%\Microsoft PIX"
set "PIXDIR="
for /f "delims=" %%d in ('dir /b /ad /o-n "%PIXROOT%" 2^>nul') do if not defined PIXDIR if exist "%PIXROOT%\%%d\pixtool.exe" set "PIXDIR=%PIXROOT%\%%d"
if not defined PIXDIR (
    echo PIX is not installed under "%PIXROOT%". Get it from https://devblogs.microsoft.com/pix/download/ 1>&2
    exit /b 1
)

if /i "%~1"=="timing" (
    "%PIXDIR%\PixTimingCapturesMcp.exe"
    exit /b %ERRORLEVEL%
)
if /i "%~1"=="dxdmp" (
    "%PIXDIR%\DxDmpMcp.exe"
    exit /b %ERRORLEVEL%
)
echo Usage: %~nx0 timing^|dxdmp 1>&2
exit /b 2
