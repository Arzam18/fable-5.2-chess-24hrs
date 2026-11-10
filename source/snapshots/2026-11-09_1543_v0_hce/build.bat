@echo off
rem Usage: build.bat <output-name-without-.exe> [extra g++ flags]
rem Builds source\src\*.cpp into source\build\<name>.exe (static, x86-64-v3, LTO).
setlocal
set ROOT=%~dp0
set OUT=%1
if "%OUT%"=="" set OUT=engine
set EXTRA=%2 %3 %4 %5 %6 %7 %8 %9
if not exist "%ROOT%build" mkdir "%ROOT%build"
g++ -std=c++20 -O3 -march=x86-64-v3 -flto -static -DNDEBUG -Wall -Wno-unused-but-set-variable %EXTRA% -o "%ROOT%build\%OUT%.exe" "%ROOT%src\attacks.cpp" "%ROOT%src\position.cpp" "%ROOT%src\eval.cpp" "%ROOT%src\search.cpp" "%ROOT%src\uci.cpp" "%ROOT%src\datagen.cpp"
exit /b %ERRORLEVEL%
