param([string]$Executable,[string]$Map)
$ErrorActionPreference='Stop'
$taskExe=if ($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $PSScriptRoot 'dist\MapExtractor.exe' }
$taskReceipt=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))) ('Tests\MapExtractor\'+[guid]::NewGuid().ToString('N'))
$taskClient=Join-Path $taskReceipt 'Client'
New-Item -ItemType Directory -Path (Join-Path $taskClient 'System'),(Join-Path $taskClient 'Maps') -Force | Out-Null
function Add-Word($Bytes,[uint32]$Value) { $Bytes.AddRange([BitConverter]::GetBytes($Value)) }
function Add-Index($Bytes,[int]$Value) {
    $taskV=[Math]::Abs([long]$Value); $taskFirst=[int]($taskV-band 63); $taskV=$taskV-shr 6
    if ($Value-lt 0) { $taskFirst=$taskFirst-bor 128 }; if ($taskV) { $taskFirst=$taskFirst-bor 64 }
    $Bytes.Add([byte]$taskFirst)
    while ($taskV) { $taskPart=[int]($taskV-band 127); $taskV=$taskV-shr 7; if ($taskV) { $taskPart=$taskPart-bor 128 }; $Bytes.Add([byte]$taskPart) }
}
function Add-Point($Bytes,[single]$X,[single]$Y,[single]$Z) { foreach ($taskN in @($X,$Y,$Z)) { $Bytes.AddRange([BitConverter]::GetBytes([single]$taskN)) } }
function Write-NativeFixture([string]$Path,[int]$OriginX,[int]$Z) {
    $taskNames=@('None','Core','Package','Class','Engine','Level','Model','myLevel','WorldModel')
    $taskNameBytes=[Collections.Generic.List[byte]]::new()
    foreach ($taskName in $taskNames) { Add-Index $taskNameBytes ($taskName.Length+1); $taskNameBytes.AddRange([Text.Encoding]::ASCII.GetBytes($taskName)); $taskNameBytes.Add(0); Add-Word $taskNameBytes 0 }
    $taskImports=[Collections.Generic.List[byte]]::new()
    foreach ($taskTuple in @(@(1,2,0,4),@(1,3,-1,5),@(1,3,-1,6))) {
        Add-Index $taskImports $taskTuple[0]; Add-Index $taskImports $taskTuple[1]; $taskImports.AddRange([BitConverter]::GetBytes([int32]$taskTuple[2])); Add-Index $taskImports $taskTuple[3]
    }
    $taskLevel=[Collections.Generic.List[byte]]::new(); $taskLevel.Add(0); Add-Word $taskLevel 0; Add-Word $taskLevel 0
    1..5 | ForEach-Object { $taskLevel.Add(0) }; Add-Word $taskLevel 0; Add-Word $taskLevel 1; Add-Index $taskLevel 2
    $taskModel=[Collections.Generic.List[byte]]::new(); $taskModel.Add(0)
    Add-Point $taskModel $OriginX 0 ($Z-1); Add-Point $taskModel ($OriginX+128) 128 ($Z+1); $taskModel.Add(1)
    Add-Point $taskModel ($OriginX+64) 64 $Z; $taskModel.AddRange([BitConverter]::GetBytes([single]100))
    Add-Index $taskModel 0; Add-Index $taskModel 4
    Add-Point $taskModel $OriginX 0 $Z; Add-Point $taskModel ($OriginX+128) 0 $Z; Add-Point $taskModel ($OriginX+128) 128 $Z; Add-Point $taskModel $OriginX 128 $Z
    Add-Index $taskModel 1; Add-Point $taskModel 0 0 1; $taskModel.AddRange([BitConverter]::GetBytes([single]$Z)); $taskModel.AddRange([byte[]]::new(9))
    Add-Index $taskModel 0; Add-Index $taskModel 0; 1..5 | ForEach-Object { Add-Index $taskModel -1 }
    $taskModel.AddRange([byte[]]::new(34)); $taskModel.Add(4); $taskModel.AddRange([byte[]]::new(20))
    Add-Index $taskModel 1; Add-Index $taskModel 0; Add-Word $taskModel 0; $taskModel.AddRange([byte[]]::new(26))
    Add-Index $taskModel 4; 0..3 | ForEach-Object { Add-Index $taskModel $_; Add-Index $taskModel 0 }
    $taskNameOffset=64; $taskImportOffset=$taskNameOffset+$taskNameBytes.Count; $taskLevelOffset=$taskImportOffset+$taskImports.Count
    $taskModelOffset=$taskLevelOffset+$taskLevel.Count; $taskExportOffset=$taskModelOffset+$taskModel.Count
    $taskExports=[Collections.Generic.List[byte]]::new()
    foreach ($taskTuple in @(@(-2,7,$taskLevel.Count,$taskLevelOffset),@(-3,8,$taskModel.Count,$taskModelOffset))) {
        Add-Index $taskExports $taskTuple[0]; Add-Index $taskExports 0; Add-Word $taskExports 0; Add-Index $taskExports $taskTuple[1]; Add-Word $taskExports 0; Add-Index $taskExports $taskTuple[2]; Add-Index $taskExports $taskTuple[3]
    }
    $taskBytes=[Collections.Generic.List[byte]]::new(); Add-Word $taskBytes 2653586369; $taskBytes.AddRange([byte[]]@(123,0,0,0)); Add-Word $taskBytes 0
    foreach ($taskN in @($taskNames.Count,$taskNameOffset,2,$taskExportOffset,3,$taskImportOffset)) { Add-Word $taskBytes $taskN }
    $taskBytes.AddRange([byte[]]::new(16)); Add-Word $taskBytes 1; Add-Word $taskBytes 2; Add-Word $taskBytes $taskNames.Count
    foreach ($taskPart in @($taskNameBytes,$taskImports,$taskLevel,$taskModel,$taskExports)) { $taskBytes.AddRange($taskPart.ToArray()) }
    [IO.File]::WriteAllBytes($Path,$taskBytes.ToArray())
}
Write-NativeFixture (Join-Path $taskClient 'Maps\20_18.unr') 0 0
Write-NativeFixture (Join-Path $taskClient 'Maps\20_18_Classic.unr') 0 128
Write-NativeFixture (Join-Path $taskClient 'Maps\21_18.unr') 32768 0
$taskSample=Join-Path $taskClient 'Maps\20_18.unr'
& $taskExe --verify-collision $taskSample
if ($LASTEXITCODE) { throw 'Native collision unit fixtures failed' }
foreach ($taskWorkers in @(1,4)) {
    & $taskExe geodata $taskSample --output (Join-Path $taskReceipt "workers-$taskWorkers") --threads $taskWorkers
    if ($LASTEXITCODE) { throw 'Native single-map generation failed' }
}
Add-Type -Path (Join-Path $PSScriptRoot 'VerifyGeoFormats.cs')
[GeoFormatCheck]::Run((Join-Path $taskReceipt 'workers-1'),'20_18',$false)
foreach ($taskFile in @('L2J\20_18.l2j','L2OFF\20_18_conv.dat')) {
    if ((Get-FileHash (Join-Path $taskReceipt "workers-1\$taskFile")).Hash -ne (Get-FileHash (Join-Path $taskReceipt "workers-4\$taskFile")).Hash) { throw 'Native worker determinism failed' }
}
& $taskExe geodata $taskClient --output (Join-Path $taskReceipt 'classic-client') --classic
if ($LASTEXITCODE) { throw 'Client-scope Classic generation failed' }
foreach ($taskStem in @('20_18','21_18')) { [GeoFormatCheck]::Run((Join-Path $taskReceipt 'classic-client'),$taskStem,$false) }
$taskSources=Get-Content (Join-Path $taskReceipt 'classic-client\911reg-map-sources.tsv') -Raw
if (!$taskSources.Contains('20_18_Classic.unr') -or !$taskSources.Contains('21_18.unr') -or (Test-Path (Join-Path $taskReceipt 'classic-client\L2J\20_18_Classic.l2j'))) { throw 'Native Classic preference, fallback or naming failed' }
if ((Get-FileHash (Join-Path $taskReceipt 'workers-1\L2J\20_18.l2j')).Hash -eq (Get-FileHash (Join-Path $taskReceipt 'classic-client\L2J\20_18.l2j')).Hash) { throw 'Classic height did not affect generated data' }
$taskSelection=Join-Path $taskReceipt 'selected.txt'
$taskSelectedMap=Join-Path $taskClient 'Maps\21_18.unr'
[IO.File]::WriteAllText($taskSelection,"$taskSelectedMap`n$taskSelectedMap`n",[Text.UTF8Encoding]::new($false))
foreach ($taskMode in @('scan','doors','water','geodata')) {
    $taskSelectedOutput=Join-Path $taskReceipt ('selected-'+$taskMode)
    & $taskExe $taskMode $taskClient --map-list $taskSelection --output $taskSelectedOutput
    if ($LASTEXITCODE) { throw "Selected maps failed for $taskMode" }
    if ($taskMode-eq 'scan') {
        $taskInventory=Get-Content -LiteralPath $taskSelectedOutput -Raw | ConvertFrom-Json
        if ($taskInventory.map_count-ne 1) { throw 'Selected inventory included unchecked maps or duplicates' }
    } else {
        $taskProvenance=Get-Content -LiteralPath (Join-Path $taskSelectedOutput '911reg-map-sources.tsv') -Raw
        if (!$taskProvenance.Contains('21_18.unr') -or $taskProvenance.Contains('20_18')) { throw "Unchecked map exported by $taskMode" }
    }
    if ($taskMode-eq 'geodata') {
        [GeoFormatCheck]::Run($taskSelectedOutput,'21_18',$false)
        if (Test-Path (Join-Path $taskSelectedOutput 'L2J\20_18.l2j')) { throw 'Unchecked geodata tile published' }
    }
}
[IO.File]::WriteAllText($taskSelection,'',[Text.UTF8Encoding]::new($false))
& $taskExe scan $taskClient --map-list $taskSelection --output (Join-Path $taskReceipt 'empty-selection.json')
if (!$LASTEXITCODE -or (Test-Path (Join-Path $taskReceipt 'empty-selection.json'))) { throw 'Empty selection silently exported the whole client' }
[IO.File]::WriteAllText($taskSelection,(Join-Path $taskClient 'Maps\20_18_Classic.unr'),[Text.UTF8Encoding]::new($false))
& $taskExe scan $taskClient --classic --map-list $taskSelection --output (Join-Path $taskReceipt 'selected-classic.json')
if ($LASTEXITCODE -or (Get-Content (Join-Path $taskReceipt 'selected-classic.json') -Raw | ConvertFrom-Json).map_count-ne 1) { throw 'Classic subset failed' }
& $taskExe scan $taskClient --map-list $taskSelection --output (Join-Path $taskReceipt 'wrong-classic.json')
if (!$LASTEXITCODE -or (Test-Path (Join-Path $taskReceipt 'wrong-classic.json'))) { throw 'Classic mismatch silently selected another map' }
& $taskExe --verify-languages
if ($LASTEXITCODE) { throw 'Language verification failed' }
& $taskExe geodata $taskClient --all-variants --output (Join-Path $taskReceipt 'both-variants')
if ($LASTEXITCODE) { throw 'Both-variant native geodata failed' }
[GeoFormatCheck]::Run((Join-Path $taskReceipt 'both-variants'),'20_18',$false)
[GeoFormatCheck]::Run((Join-Path $taskReceipt 'both-variants\Classic'),'20_18',$false)
if ((Get-FileHash (Join-Path $taskReceipt 'both-variants\L2J\20_18.l2j')).Hash-eq (Get-FileHash (Join-Path $taskReceipt 'both-variants\Classic\L2J\20_18.l2j')).Hash) { throw 'Classic geodata overwrote normal geodata' }
& $taskExe --verify-ui-geodata $taskSample (Join-Path $taskReceipt 'ui')
if ($LASTEXITCODE) { throw 'Native geodata UI failed' }
if (!(Test-Path (Join-Path $taskReceipt 'ui\Geodata\L2J\20_18.l2j')) -or !(Test-Path (Join-Path $taskReceipt 'ui\Summary.txt'))) { throw 'UI results are not directly under the chosen output folder' }
$taskUiLogHash=(Get-FileHash (Join-Path $taskReceipt 'ui\911reg.log')).Hash
& $taskExe --verify-ui-geodata $taskSample (Join-Path $taskReceipt 'ui')
if (!$LASTEXITCODE -or (Get-FileHash (Join-Path $taskReceipt 'ui\911reg.log')).Hash-ne $taskUiLogHash) { throw 'Existing output was overwritten without confirmation' }
& $taskExe --verify-output (Join-Path $taskReceipt 'clear-fixture')
if ($LASTEXITCODE) { throw 'Scoped output clearing failed' }
$taskLogs=@(Get-ChildItem (Join-Path $taskReceipt 'ui') -Filter 911reg.log -Recurse)
if ($taskLogs.Count-ne 1 -or !(Get-Content $taskLogs[0].FullName -Raw).Contains('native client collision')) { throw 'Native UI log missing' }
if ($Map) {
    $taskHash=(Get-FileHash -LiteralPath $Map).Hash
    & $taskExe --check-collision $Map
    if ($LASTEXITCODE) { throw 'Real map collision audit incomplete' }
    & $taskExe geodata $Map --output (Join-Path $taskReceipt 'real-map')
    if ($LASTEXITCODE) { throw 'Real map generation incomplete' }
    $taskStem=[IO.Path]::GetFileNameWithoutExtension($Map)-replace '_Classic$',''
    [GeoFormatCheck]::Run((Join-Path $taskReceipt 'real-map'),$taskStem,$false)
    Copy-Item -LiteralPath $Map -Destination (Join-Path $taskClient 'Maps\24_16.unr')
    & $taskExe geodata (Join-Path $taskClient 'Maps\24_16.unr') --output (Join-Path $taskReceipt 'missing-packages')
    if ($LASTEXITCODE-ne 2 -or (Test-Path (Join-Path $taskReceipt 'missing-packages\L2J\24_16.l2j'))) { throw 'Missing packages published incomplete native tile' }
    if ((Get-FileHash -LiteralPath $Map).Hash-ne $taskHash) { throw 'Real source map changed' }
}
Write-Output "PASS: native BSP/material/simple collision fixtures, real UNR workflow, worker determinism, three binary formats, client scope, Classic preference/fallback and UI logs. Receipt: $taskReceipt"
