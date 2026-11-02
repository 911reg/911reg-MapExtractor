param([Parameter(Mandatory=$true)][string]$Map,[string]$ReceiptRoot,[string]$EmptyMap)
$ErrorActionPreference='Stop'
$taskExe=Join-Path $PSScriptRoot 'dist/MapExtractor.exe'
$taskMap=[IO.Path]::GetFullPath($Map)
$taskRoot=if($ReceiptRoot) {[IO.Path]::GetFullPath($ReceiptRoot)} else {Join-Path $PSScriptRoot '_tests/events'}
$taskReceipt=Join-Path $taskRoot ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
$taskHash=(Get-FileHash -LiteralPath $taskMap).Hash
$script:taskCall=0
function Invoke-EventExtractor([string[]]$Arguments,[int]$Expected=0) {
    $script:taskCall++
    $taskOut=Join-Path $taskReceipt "$script:taskCall.out"
    $taskErr=Join-Path $taskReceipt "$script:taskCall.err"
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList ($Arguments | ForEach-Object {'"'+$_+'"'}) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput $taskOut -RedirectStandardError $taskErr
    if($taskProcess.ExitCode -ne $Expected) {Get-Content $taskOut,$taskErr; throw "Unexpected exit code $($taskProcess.ExitCode)"}
}
Invoke-EventExtractor @('--verify-events')
Invoke-EventExtractor @('--verify-languages')
Invoke-EventExtractor @('--verify-completion')
Invoke-EventExtractor @('--verify-output',(Join-Path $taskReceipt 'scoped-output'))
foreach($taskWorkers in @(1,4)) {
    Invoke-EventExtractor @('events',$taskMap,'--output',(Join-Path $taskReceipt "workers-$taskWorkers"),'--threads',"$taskWorkers",'--simple-output')
}
$taskFiles=@(Get-ChildItem (Join-Path $taskReceipt 'workers-1') -Filter '*_Events.txt' | Where-Object Name -ne 'README_EVENTS.txt')
if($taskFiles.Count -ne 1) {throw 'Expected one event catalog'}
$taskName=$taskFiles[0].Name
if((Get-Content (Join-Path $taskReceipt 'workers-1/README_EVENTS.txt') -Raw).Contains('Choose only the folder matching your server')) {throw 'Event guidance contains server format instructions'}
if((Get-FileHash $taskFiles[0].FullName).Hash -ne (Get-FileHash (Join-Path $taskReceipt "workers-4/$taskName")).Hash) {throw 'Worker determinism failed'}
$taskText=Get-Content $taskFiles[0].FullName -Raw -Encoding utf8
$taskCount=[int]([regex]::Match($taskText,'(?m)^Entries = (\d+)').Groups[1].Value)
$taskIds=@([regex]::Matches($taskText,'(?m)^Event ID = (-?\d+)') | ForEach-Object {[int]$_.Groups[1].Value})
$taskIdCount=[int]([regex]::Match($taskText,'(?m)^Event IDs = (\d+)').Groups[1].Value)
if($taskIds.Count -ne $taskIdCount -or @($taskIds|Sort-Object -Unique).Count -ne $taskIdCount -or [regex]::Matches($taskText,'(?m)^  Actor = ').Count -ne $taskCount) {throw 'Grouped event counts mismatch'}
if(($taskIds -join ',') -ne (($taskIds|Sort-Object) -join ',') -or $taskText -match '(?m)^(Class|Event Index|Tag|Group) =') {throw 'Catalog is unsorted or contains redundant fields'}
Invoke-EventExtractor @('events',$taskMap,'--output',(Join-Path $taskReceipt 'workers-1')) 1
Invoke-EventExtractor @('--verify-ui-events',$taskMap,(Join-Path $taskReceipt 'ui'))
if((Get-FileHash $taskFiles[0].FullName).Hash -ne (Get-FileHash (Join-Path $taskReceipt "ui/Events/$taskName")).Hash) {throw 'UI and CLI differ'}
$taskSelection=Join-Path $taskReceipt 'selection'
New-Item -ItemType Directory -Path $taskSelection | Out-Null
Copy-Item -LiteralPath $taskMap -Destination (Join-Path $taskSelection '22_22.unr')
Copy-Item -LiteralPath $taskMap -Destination (Join-Path $taskSelection '22_22_Classic.unr')
Invoke-EventExtractor @('events',$taskSelection,'--output',(Join-Path $taskReceipt 'variants'),'--all-variants','--simple-output')
Invoke-EventExtractor @('events',$taskSelection,'--output',(Join-Path $taskReceipt 'variants-serial'),'--all-variants','--simple-output','--threads','1')
foreach($taskFile in @('22_22_Events.txt','Classic/22_22_Events.txt')) {
    if(!(Test-Path (Join-Path $taskReceipt "variants/$taskFile"))) {throw 'Normal/Classic outputs collided'}
    if((Get-FileHash (Join-Path $taskReceipt "variants/$taskFile")).Hash -ne (Get-FileHash (Join-Path $taskReceipt "variants-serial/$taskFile")).Hash) {throw 'Parallel map output differs'}
}
Invoke-EventExtractor @('events',$taskSelection,'--output',(Join-Path $taskReceipt 'prefer-classic'),'--classic','--simple-output')
if(!(Get-Content (Join-Path $taskReceipt 'prefer-classic/22_22_Events.txt') -Raw).Contains('22_22_Classic.unr')) {throw 'Classic preference ignored'}
$taskList=Join-Path $taskReceipt 'selected.txt'
(Join-Path $taskSelection '22_22.unr') | Set-Content $taskList -Encoding utf8
Invoke-EventExtractor @('events',$taskSelection,'--output',(Join-Path $taskReceipt 'selected'),'--all-variants','--map-list',$taskList,'--simple-output')
if(Test-Path (Join-Path $taskReceipt 'selected/Classic')) {throw 'Unselected map exported'}
$taskBad=Join-Path $taskReceipt '23_23.unr'
'invalid package' | Set-Content $taskBad
Invoke-EventExtractor @('events',$taskBad,'--output',(Join-Path $taskReceipt 'invalid')) 2
if(Test-Path (Join-Path $taskReceipt 'invalid/23_23_Events.txt')) {throw 'Invalid map published a catalog'}
if((Get-FileHash -LiteralPath $taskMap).Hash -ne $taskHash) {throw 'Input map changed'}
if($EmptyMap) {
    $taskEmptyHash=(Get-FileHash -LiteralPath $EmptyMap).Hash
    Invoke-EventExtractor @('events',([IO.Path]::GetFullPath($EmptyMap)),'--output',(Join-Path $taskReceipt 'empty'),'--simple-output')
    if(@(Get-ChildItem (Join-Path $taskReceipt 'empty') -Filter '*_Events.txt' -Recurse | Where-Object Name -ne 'README_EVENTS.txt').Count) {throw 'Empty map created an event catalog'}
    if((Get-FileHash -LiteralPath $EmptyMap).Hash -ne $taskEmptyHash) {throw 'Empty input map changed'}
}
Write-Output "PASS: event records, defaults, malformed input, workers, UI/CLI, selection, Classic variants, translations and scoped output. Receipt: $taskReceipt"
