@echo off
rem Usage: build.bat <output-name-without-.exe> <network file relative to source\ | none> [extra g++ flags]
rem Builds source\src\*.cpp into source\build\<name>.exe (static, x86-64-v3, LTO), embedding the network.
setlocal
set ROOT=%~dp0
set OUT=%1
if "%OUT%"=="" set OUT=engine
set NET=%~2
set EXTRA=%3 %4 %5 %6 %7 %8 %9
rem hidden size override: set NNH=512 before calling (cmd splits arguments at "=")
if not "%NNH%"=="" set EXTRA=%EXTRA% -DNN_H=%NNH%
if not "%NNKB%"=="" set EXTRA=%EXTRA% -DNN_KB=%NNKB%
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%src"
if "%NET%"=="" set NET=none
if "%NET%"=="none" (type nul > net.bin) else (copy /y "%ROOT%%NET%" net.bin > nul)
g++ -std=c++20 -O3 -march=x86-64-v3 -flto -static -Wl,--stack,8388608 -DNDEBUG -Wall -Wno-unused-but-set-variable %EXTRA% -o "..\build\%OUT%.exe" attacks.cpp position.cpp eval.cpp nnue.cpp search.cpp uci.cpp datagen.cpp
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
