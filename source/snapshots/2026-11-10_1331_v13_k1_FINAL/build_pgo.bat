@echo off
rem Usage: build_pgo.bat <output-name-without-.exe> <network file relative to source\ | none> [extra g++ flags]
rem Profile-guided build: instrumented build -> bench run -> optimised build. Output source\build\<name>.exe
setlocal
set ROOT=%~dp0
set OUT=%1
set NET=%~2
set EXTRA=%3 %4 %5 %6 %7 %8 %9
if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%ROOT%build\pgo" mkdir "%ROOT%build\pgo"
del /q "%ROOT%build\pgo\*.gcda" 2>nul
pushd "%ROOT%src"
if "%NET%"=="" set NET=none
if "%NET%"=="none" (type nul > net.bin) else (copy /y "%ROOT%%NET%" net.bin > nul)
set SRC=attacks.cpp position.cpp eval.cpp nnue.cpp search.cpp uci.cpp datagen.cpp
set FLAGS=-std=c++20 -O3 -march=x86-64-v3 -flto -static -Wl,--stack,8388608 -DNDEBUG -Wall -Wno-unused-but-set-variable %EXTRA%
g++ %FLAGS% -fprofile-generate -fprofile-dir="%ROOT%build\pgo" -o "..\build\%OUT%_instr.exe" %SRC%
if errorlevel 1 (popd & exit /b 1)
"..\build\%OUT%_instr.exe" bench 13 > nul
g++ %FLAGS% -fprofile-use -fprofile-dir="%ROOT%build\pgo" -fprofile-correction -Wno-missing-profile -o "..\build\%OUT%.exe" %SRC%
set ERR=%ERRORLEVEL%
del /q "..\build\%OUT%_instr.exe" 2>nul
popd
exit /b %ERR%
