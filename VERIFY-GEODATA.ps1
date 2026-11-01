param([string]$Executable,[string]$ReceiptRoot)
$ErrorActionPreference='Stop'
$taskExe=if ($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $PSScriptRoot 'dist\MapExtractor.exe' }
$taskReceiptRoot=if ($ReceiptRoot) { [IO.Path]::GetFullPath($ReceiptRoot) } else { Join-Path $PSScriptRoot '_tests\geodata' }
$taskReceipt=Join-Path $taskReceiptRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
$script:taskInvocation=0
function Invoke-Extractor {
    $script:taskInvocation++
    $taskStdout=Join-Path $taskReceipt "$script:taskInvocation.out"
    $taskStderr=Join-Path $taskReceipt "$script:taskInvocation.err"
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList ($args | ForEach-Object { '"'+$_+'"' }) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
    Get-Content -LiteralPath $taskStdout,$taskStderr
    $script:LASTEXITCODE=$taskProcess.ExitCode
}
$taskObj=Join-Path $taskReceipt '20_18.obj'
@'
v 0 0 0
v 128 0 0
v 128 128 0
v 0 128 0
v 0 0 128
v 128 0 128
v 128 128 128
v 0 128 128
v 64 0 0
v 64 128 0
v 64 128 96
v 64 0 96
v 256 256 -16
v 640 256 -16
v 640 640 -16
v 256 640 -16
f 1 2 3
f 1 3 4
f 5 6 7
f 5 7 8
f 9 10 11
f 9 11 12
f 13 14 15
f 13 15 16
'@ | Set-Content -LiteralPath $taskObj -Encoding ascii
$taskHash=(Get-FileHash $taskObj).Hash
Invoke-Extractor --verify-geodata
if ($LASTEXITCODE) { throw 'Geometry regression failed' }
foreach ($taskWorkers in @(1,4)) {
    Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt "workers-$taskWorkers") --threads $taskWorkers --geo both --air-radius 8.2 --air-half-height 22.5
    if ($LASTEXITCODE) { throw 'Geodata generation failed' }
}
foreach ($taskFile in @('L2J\20_18.l2j','L2OFF\20_18_conv.dat')) {
    if ((Get-FileHash (Join-Path $taskReceipt "workers-1\$taskFile")).Hash -ne (Get-FileHash (Join-Path $taskReceipt "workers-4\$taskFile")).Hash) { throw 'Worker counts changed binary output' }
}
Add-Type -Path (Join-Path $PSScriptRoot 'VerifyGeoFormats.cs')
[GeoFormatCheck]::Run((Join-Path $taskReceipt 'workers-1'))
if ((Get-FileHash $taskObj).Hash -ne $taskHash) { throw 'Source mesh changed' }
Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt 'large-body') --threads 4 --geo both --air-radius 40 --air-half-height 30
if ($LASTEXITCODE) { throw 'Large air body generation failed' }
[GeoFormatCheck]::Run((Join-Path $taskReceipt 'large-body'),'20_18',$false)
$taskReport=Get-Content (Join-Path $taskReceipt 'large-body\reports\20_18.txt') -Raw
if (!$taskReport.Contains('radius 40.000000, half-height 30.000000')) { throw 'Air body settings did not reach generation' }
Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt 'default-body') --threads 1 --geo both
if ($LASTEXITCODE) { throw 'Default air body generation failed' }
foreach ($taskFile in @('L2J\20_18.l2j','L2OFF\20_18_conv.dat')) {
    if ((Get-FileHash (Join-Path $taskReceipt "large-body\$taskFile")).Hash -ne (Get-FileHash (Join-Path $taskReceipt "default-body\$taskFile")).Hash) { throw 'Default air dimensions differ from 40/30' }
}
foreach ($taskOption in @('--air-radius','--air-half-height')) {
    foreach ($taskValue in @('0','-1','513','nan','inf','40oops')) {
        $taskInvalid=Join-Path $taskReceipt "invalid-body-$taskOption-$taskValue"
        Invoke-Extractor geodata-fixture $taskObj --output $taskInvalid $taskOption $taskValue
        if ($LASTEXITCODE -ne 1 -or (Test-Path $taskInvalid)) { throw 'Invalid air body setting was accepted or created output' }
    }
    Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt 'missing-body') $taskOption
    if ($LASTEXITCODE -ne 1) { throw 'Missing air body setting was accepted' }
}
Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt 'workers-1')
if ($LASTEXITCODE -ne 1) { throw 'Existing output was not refused' }
'f 1 2 3 4' | Add-Content -LiteralPath $taskObj -Encoding ascii
Invoke-Extractor geodata-fixture $taskObj --output (Join-Path $taskReceipt 'invalid')
if ($LASTEXITCODE -ne 1 -or (Test-Path (Join-Path $taskReceipt 'invalid\L2J\20_18.l2j'))) { throw 'Invalid collision input published geodata' }
$taskInput=Join-Path $taskReceipt 'selection'
New-Item -ItemType Directory -Path $taskInput | Out-Null
foreach ($taskName in @('22_22.unr','22_22_Classic.unr','23_22.unr')) { Set-Content (Join-Path $taskInput $taskName) 'invalid package fixture' }
foreach ($taskClassic in @($false,$true)) {
    $taskArgs=@('scan',$taskInput,'--output',(Join-Path $taskReceipt "selection-$taskClassic.json"))
    if ($taskClassic) { $taskArgs+='--classic' }
    Invoke-Extractor @taskArgs
    if ($LASTEXITCODE -ne 2) { throw 'Selection fixture expected incomplete scan' }
    $taskJson=Get-Content (Join-Path $taskReceipt "selection-$taskClassic.json") -Raw
    if ($taskClassic -ne $taskJson.Contains('22_22_Classic.unr') -or !$taskJson.Contains('23_22.unr')) { throw 'Classic preference or normal fallback failed' }
}
Write-Output "PASS: L2J/L2OFF cell equivalence, PTS counters, signed flat heights, walls, deterministic workers, output refusal, malformed input and Classic preference/fallback. Receipt: $taskReceipt"
