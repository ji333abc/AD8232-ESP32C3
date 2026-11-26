param([string]$Compiler = 'C:/msys64/ucrt64/bin/gcc.exe')
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath (Join-Path $PSScriptRoot '../..')
New-Item -ItemType Directory -Force Build | Out-Null
New-Item -ItemType Directory -Force Build/ecg_frames | Out-Null
$arguments = @('-std=c11','-Os','-ITests/host','-ICore/Inc',
    'Tests/host/lvgl_smoke.c','Core/Src/lvgl_port.c','Core/Src/ecg_sim.c')
$arguments += @(Get-ChildItem Drivers/lvgl/src -Filter *.c -Recurse | ForEach-Object { '"' + $_.FullName.Replace('\','/') + '"' })
$arguments += @('-o','Build/lvgl_smoke.exe','-lm')
Set-Content -LiteralPath Build/host_args.rsp -Value $arguments -Encoding utf8
& $Compiler '@Build/host_args.rsp'
if ($LASTEXITCODE -ne 0) { throw 'Host test compilation failed.' }
& ./Build/lvgl_smoke.exe
if ($LASTEXITCODE -ne 0) { throw 'LVGL smoke test failed.' }
