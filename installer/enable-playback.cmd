@echo off
REM Switch FremSoft into PLAYBACK mode.
REM
REM Tech2Win will load the shimmed CSTech2Win.dll which serves the
REM data path from the recording at PlaybackRecording instead of
REM forwarding to the real Chipsoft DLL. No ECM needed.
REM
REM Re-launch Tech2Win after running this. Use enable-record.cmd to
REM switch back.

setlocal
set REC=%~dp0recordings\2026-05-13-check-codes.json

REM Self-elevate if not running as admin.
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting elevation^...
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
    exit /b
)

reg add "HKLM\SOFTWARE\OpenSAAB\Collector" /v Mode /t REG_SZ /d playback /f >nul
if %errorlevel% neq 0 goto fail
reg add "HKLM\SOFTWARE\OpenSAAB\Collector" /v PlaybackRecording /t REG_SZ /d "%REC%" /f >nul
if %errorlevel% neq 0 goto fail

echo.
echo === FremSoft mode = playback ===
echo Recording: %REC%
echo.
echo Launch Tech2Win to test. Open the tray app's
echo "Open decoded console (scapy)" menu to watch decoded UDS replies
echo come back from the recording in real time.
echo.
pause
exit /b 0

:fail
echo === Failed to write registry. Run as administrator? ===
pause
exit /b 1
