# Retrain cycle: stop datagen gracefully, warm-start train the next net on all data, build engine(s),
# restart datagen with the new net.
# usage (from project root):
#   powershell -ExecutionPolicy Bypass -File source\retrain.ps1 -prev n8_256 -next n9_256 -ver v9 -seed 13
param(
    [string]$prev, [string]$next, [string]$ver = "v9", [int]$seed = 1,
    [int]$epochs = 6, [string]$lr = "0.0004", [int]$threads = 10, [int]$dgThreads = 10, [int]$nodes = 2000,
    [int]$hidden = 256
)
$ErrorActionPreference = "Continue"
"stop" | Out-File source\data\STOP
$n = 0
while ((Get-Process | Where-Object { $_.ProcessName -like "v*n*" -and $_.Threads.Count -ge 8 }) -and $n -lt 90) { Start-Sleep -Seconds 2; $n++ }
Remove-Item source\data\STOP -ErrorAction SilentlyContinue
"datagen stopped $(Get-Date -Format o)"
$files = (Get-ChildItem source\data\g*.bin | ForEach-Object { $_.FullName })
& source\build\trainer.exe --out "source\build\$next.nnue" --hidden $hidden --epochs $epochs --lr $lr --lr-final 0.00003 --threads $threads --lambda 1.0 --init "source\build\$prev.nnue.f32" @files |
    Tee-Object -FilePath "source\tests\train_$next.log" | Select-String "validation|epoch" | ForEach-Object { $_.Line }
$name = "$ver" + ($next -replace "_256", "")
cmd /c "source\build.bat $name build\$next.nnue 2>nul"
"build $name exit=$LASTEXITCODE"
cmd /c "source\build.bat ${name}tune build\$next.nnue -DTUNE 2>nul"
cmd /c "source\build\$name.exe bench 12"
Start-Process -FilePath "source\build\$name.exe" -ArgumentList "datagen $dgThreads source\data\g_$next $nodes 900000000 $seed" -RedirectStandardOutput "source\data\g_${next}_log.txt" -NoNewWindow
"datagen restarted with $name $(Get-Date -Format o)"
