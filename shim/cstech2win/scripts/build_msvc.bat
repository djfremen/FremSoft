@echo off
REM Build the CSTech2Win shim DLL with MSVC.
REM Targets 32-bit (x86) because the real CSTech2Win.dll is PE32 x86.
REM Run from anywhere; this script chdir's to the shim repo root.

setlocal
pushd "%~dp0\.."

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 goto fail

if not exist build mkdir build

REM Ensure forwarders.c is MSVC-syntax (gen_shim.py emits GCC syntax by default).
where py >nul 2>nul && py scripts\fix_msvc.py || python scripts\fix_msvc.py

cl /nologo /LD /O2 /MT /D_USRDLL /D_WINDLL /Fobuild\ ^
   /I ..\..\src ^
   /I ..\..\src\vendor ^
   src\dllmain.c src\log.c src\wrappers.c src\forwarders.c ^
   ..\..\src\fremsoft.c ^
   ..\..\src\recording.c ^
   ..\..\src\scheduler.c ^
   ..\..\src\unknown_log.c ^
   /link /DEF:src\cstech2win.def /OUT:build\CSTech2Win.dll ^
         /IMPLIB:build\CSTech2Win.lib advapi32.lib
if errorlevel 1 goto fail

echo.
echo === build OK: build\CSTech2Win.dll ===
dir /B build\CSTech2Win.dll
popd
endlocal
exit /b 0

:fail
echo === build FAILED ===
popd
endlocal
exit /b 1
