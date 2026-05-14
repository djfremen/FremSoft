@echo off
REM Switch FremSoft back into RECORD mode (the default).
REM
REM Tech2Win talks to the real Chipsoft adapter again, the shim
REM forwards every call and writes a normal cstech2win_shim_*.log
REM in %TEMP% as before.

setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting elevation...
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
    exit /b
)

reg add "HKLM\SOFTWARE\OpenSAAB\Collector" /v Mode /t REG_SZ /d record /f >nul
if %errorlevel% neq 0 goto fail

echo.
echo === FremSoft mode = record ===
echo Tech2Win will talk to the real ECM via Chipsoft as normal.
echo Re-launch Tech2Win for the change to take effect.
echo.
pause
exit /b 0

:fail
echo === Failed to write registry. Run as administrator? ===
pause
exit /b 1
