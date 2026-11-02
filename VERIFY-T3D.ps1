param([Parameter(Mandatory=$true)][string]$Map,[string]$ReceiptRoot)
$ErrorActionPreference='Stop'
$taskExe=Join-Path $PSScriptRoot 'dist/MapExtractor.exe'
$taskMap=[IO.Path]::GetFullPath($Map)
$taskRoot=if($ReceiptRoot) {[IO.Path]::GetFullPath($ReceiptRoot)} else {Join-Path $PSScriptRoot '_tests/t3d'}
$taskReceipt=Join-Path $taskRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
$taskHash=(Get-FileHash -LiteralPath $taskMap).Hash
$script:taskCall=0
function Invoke-T3dExtractor([string[]]$Arguments,[int]$Expected=0) {
    $script:taskCall++
    $taskOut=Join-Path $taskReceipt "$script:taskCall.out"
    $taskErr=Join-Path $taskReceipt "$script:taskCall.err"
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList ($Arguments | ForEach-Object {'"'+$_+'"'}) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $taskOut -RedirectStandardError $taskErr
    if($taskProcess.ExitCode -ne $Expected) {Get-Content $taskOut,$taskErr; throw "Unexpected exit code $($taskProcess.ExitCode)"}
}
Invoke-T3dExtractor @('--verify-t3d')
Invoke-T3dExtractor @('--verify-languages')
Invoke-T3dExtractor @('--verify-completion')
Invoke-T3dExtractor @('--verify-output',(Join-Path $taskReceipt 'scoped-output'))
foreach($taskWorkers in @(1,4)) {
    Invoke-T3dExtractor @('t3d',$taskMap,'--output',(Join-Path $taskReceipt "workers-$taskWorkers"),'--threads',"$taskWorkers",'--simple-output')
}
Invoke-T3dExtractor @('t3d',$taskMap,'--output',(Join-Path $taskReceipt 'workers-1')) 1
Invoke-T3dExtractor @('--verify-ui-t3d',$taskMap,(Join-Path $taskReceipt 'ui'))
$taskBase=Join-Path $taskReceipt 'workers-1'
$taskFiles=@(Get-ChildItem -LiteralPath $taskBase -Filter '*.t3d' -Recurse)
if(!$taskFiles.Count) {throw 'No T3D files generated'}
$taskActors=0
$taskPolygons=0
$taskObjects=0
foreach($taskFile in $taskFiles) {
    $taskRelative=$taskFile.FullName.Substring($taskBase.Length+1)
    $taskFileHash=(Get-FileHash -LiteralPath $taskFile.FullName).Hash
    foreach($taskOther in @('workers-4','ui/T3D')) {
        if((Get-FileHash -LiteralPath (Join-Path $taskReceipt "$taskOther/$taskRelative")).Hash -ne $taskFileHash) {throw "Worker/UI output differs: $taskRelative"}
    }
    $taskText=[IO.File]::ReadAllText($taskFile.FullName,[Text.UTF8Encoding]::new($false,$true))
    if(!$taskText.StartsWith("Begin Map`r`n") -or !$taskText.EndsWith("End Map`r`n")) {throw "Invalid T3D envelope: $taskRelative"}
    $taskStack=[Collections.Generic.Stack[string]]::new()
    foreach($taskLine in ($taskText -split '\r\n')) {
        if($taskLine -match '^\s*Begin (\w+)') {$taskStack.Push($Matches[1])}
        elseif($taskLine -match '^\s*End (\w+)') {if(!$taskStack.Count -or $taskStack.Pop() -ne $Matches[1]) {throw "Unbalanced T3D: $taskRelative"}}
    }
    if($taskStack.Count) {throw "Unclosed T3D: $taskRelative"}
    if($taskText -match '(?m)^ +' -or $taskText -cmatch '(?m)=(True|False)(?=[,\)\r\n])' -or $taskText.Contains('Begin Surface')) {throw "Reference indentation/boolean/envelope mismatch: $taskRelative"}
    foreach($taskVector in [regex]::Matches($taskText,'(?m)^(\t+)(Origin   |Normal   |TextureU |TextureV |Vertex   )([^\r\n]+)')) {
        if($taskVector.Groups[1].Length -lt 4 -or $taskVector.Groups[3].Value -notmatch '^[+-]\d{5,}\.\d{6},[+-]\d{5,}\.\d{6},[+-]\d{5,}\.\d{6}$') {throw "Reference polygon vector format mismatch: $taskRelative"}
    }
    if($taskText -match "(?<=[=(,])(?!(?:Package)')\w+'[^'.\r\n]+'") {throw "Unqualified local object reference: $taskRelative"}
    $taskClass=($taskFile.BaseName -split '\.')[-1]
    foreach($taskActor in [regex]::Matches($taskText,'(?m)^Begin (?:Actor|LevelObject) Class=(\S+) Name=(\S+)')) {
        if(($taskActor.Groups[1].Value -split '\.')[-1] -ne $taskClass) {throw 'Class grouping mismatch'}
        $taskActors++
    }
    $taskPolygons+=[regex]::Matches($taskText,'(?m)^\s*Begin Polygon').Count
    $taskObjects+=[regex]::Matches($taskText,'(?m)^\s*Begin Object').Count
}
if((Get-FileHash -LiteralPath $taskMap).Hash -ne $taskHash) {throw 'Source map changed'}
[ordered]@{map=$taskMap;actors=$taskActors;polygons=$taskPolygons;nestedObjects=$taskObjects;files=$taskFiles.Count;sourceSHA256=$taskHash;exeSHA256=(Get-FileHash $taskExe).Hash} | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $taskReceipt 'receipt.json')
Write-Output "PASS: typed serialization, boundaries, worker/UI equality, class grouping, balanced T3D and unchanged source. Actors=$taskActors, polygons=$taskPolygons, nested objects=$taskObjects. Receipt: $taskReceipt"
