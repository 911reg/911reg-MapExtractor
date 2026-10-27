param([Parameter(Mandatory=$true)][string]$DoorMap,[Parameter(Mandatory=$true)][string]$WaterMap,[Parameter(Mandatory=$true)][string]$GeoMap)
$ErrorActionPreference='Stop'
$taskExe=Join-Path $PSScriptRoot 'dist\MapExtractor.exe'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\simple-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
& $taskExe --verify-preferences (Join-Path $taskReceipt 'preferences.ini')
if ($LASTEXITCODE) { throw 'Saved server preference failed' }
& $taskExe --verify-completion
if ($LASTEXITCODE) { throw 'Visible completion reasons failed' }
foreach ($taskMode in @('doors','water')) {
    $taskInput=if ($taskMode-eq 'doors') { $DoorMap } else { $WaterMap }
    $taskOutput=Join-Path $taskReceipt $taskMode
    & $taskExe $taskMode $taskInput --all-variants --server acis --combine-xml --simple-output --output $taskOutput
    if ($LASTEXITCODE) { throw 'Simple export failed' }
    [xml]$taskXml=Get-Content (Join-Path $taskOutput ($taskMode+'.xml')) -Raw
    if (!$taskXml.list -or !(Test-Path (Join-Path $taskOutput ('README_'+$taskMode.ToUpper()+'.txt')))) { throw 'Copy-ready XML or readme missing' }
    if ((Test-Path (Join-Path $taskOutput 'acis')) -or (Test-Path (Join-Path $taskOutput 'raw')) -or @(Get-ChildItem $taskOutput -Recurse -File -Filter '*.tsv').Count) { throw 'Simple output contains format wrappers or raw data' }
    if (@(Get-ChildItem $taskOutput -Recurse -File | Where-Object Name -Like '*report*').Count) { throw 'Simple output contains technical reports' }
    $taskDebug=Join-Path $taskReceipt ($taskMode+'-debug')
    & $taskExe $taskMode $taskInput --all-variants --server acis --combine-xml --output $taskDebug
    if ($LASTEXITCODE) { throw 'Debug export failed' }
    if ((Get-FileHash (Join-Path $taskOutput ($taskMode+'.xml'))).Hash-ne (Get-FileHash (Join-Path $taskDebug ('acis\'+$taskMode+'.xml'))).Hash) { throw 'Simple mode changed server geometry' }
}
$taskUi=Join-Path $taskReceipt 'ui'
& $taskExe --verify-ui-simple-geodata $GeoMap $taskUi
if ($LASTEXITCODE -or !(Test-Path (Join-Path $taskUi 'Geodata\README_GEODATA.txt'))) { throw 'Simple UI geodata failed' }
if (@(Get-ChildItem $taskUi -File).Count -or @(Get-ChildItem $taskUi -Recurse -File -Filter '*.tsv').Count -or @(Get-ChildItem $taskUi -Recurse -File -Filter '*.log').Count) { throw 'UI emitted technical files while debug was off' }
$taskBroken=Join-Path $taskReceipt 'Client\Maps'
New-Item -ItemType Directory -Path $taskBroken | Out-Null
Copy-Item -LiteralPath $DoorMap -Destination (Join-Path $taskBroken '24_16.unr')
[IO.File]::WriteAllText((Join-Path $taskBroken '24_17.unr'),'not a map')
& $taskExe --verify-ui-simple-doors (Split-Path $taskBroken) (Join-Path $taskReceipt 'partial-ui')
if ($LASTEXITCODE -or !(Test-Path (Join-Path $taskReceipt 'partial-ui\Doors\doors.xml'))) { throw 'Partial export did not show skipped reasons in Activity' }
Write-Output "PASS: saved server profile, clean flat XML, identical geometry, task readmes, hidden debug files and visible skipped reasons. Receipt: $taskReceipt"
