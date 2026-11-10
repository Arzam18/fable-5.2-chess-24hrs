@echo off
rem Usage: match.bat <engineA.exe path> <engineB.exe path> <rounds> <tc> <tag> [concurrency]
rem Plays A vs B from the UHO book, both colours per opening, Hash 256 (as the rating harness does).
setlocal
set ROOT=%~dp0
set CONC=%6
if "%CONC%"=="" set CONC=14
"%ROOT%..\resources\fastchess\fastchess.exe" -engine cmd=%1 name=A_%~n1 -engine cmd=%2 name=B_%~n2 -each tc=%4 option.Hash=256 -openings file="%ROOT%..\resources\fastchess\UHO.pgn" format=pgn order=random plies=16 -rounds %3 -repeat -concurrency %CONC% -ratinginterval 0 -recover -pgnout file="%ROOT%tests\%5.pgn" > "%ROOT%tests\%5.log" 2>&1
powershell -NoProfile -Command "Get-Content '%ROOT%tests\%5.log' | Select-String -Pattern 'Results of|^Elo|^Games|Ptnml|Timeouts|Crashed|disconnect|illegal|loses on time' | Select-Object -Last 12"
