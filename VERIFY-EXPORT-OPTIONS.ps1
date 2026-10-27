param([Parameter(Mandatory=$true)][string]$DoorMap,[Parameter(Mandatory=$true)][string]$WaterMap)
$ErrorActionPreference='Stop'
$taskExe=Join-Path $PSScriptRoot 'dist\MapExtractor.exe'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\options-'+[guid]::NewGuid().ToString('N'))
$taskDoorFolder=Join-Path $taskReceipt 'Doors\Maps'
$taskWaterFolder=Join-Path $taskReceipt 'Water\Maps'
New-Item -ItemType Directory -Path $taskDoorFolder,$taskWaterFolder | Out-Null
$taskDoorHash=(Get-FileHash -LiteralPath $DoorMap).Hash
$taskWaterHash=(Get-FileHash -LiteralPath $WaterMap).Hash
foreach ($taskName in @('24_16.unr','24_16_Classic.unr')) { Copy-Item -LiteralPath $DoorMap -Destination (Join-Path $taskDoorFolder $taskName) }
foreach ($taskName in @('11_23.unr','11_24.unr','11_23_Classic.unr')) { Copy-Item -LiteralPath $WaterMap -Destination (Join-Path $taskWaterFolder $taskName) }
foreach ($taskMode in @('doors','water')) {
    $taskInput=if ($taskMode-eq 'doors') { $taskDoorFolder } else { $taskWaterFolder }
    $taskSplit=Join-Path $taskReceipt ($taskMode+'-split')
    $taskCombined=Join-Path $taskReceipt ($taskMode+'-combined')
    & $taskExe $taskMode $taskInput --all-variants --output $taskSplit
    if ($LASTEXITCODE) { throw "$taskMode split export failed" }
    & $taskExe $taskMode $taskInput --all-variants --combine-xml --output $taskCombined --threads 4
    if ($LASTEXITCODE) { throw "$taskMode combined export failed" }
    if ($taskMode-eq 'doors') {
        foreach ($taskVariant in @('','Classic')) {
            $taskClientFolder=Join-Path $taskCombined $taskVariant
            $taskClientDat=@(Get-Content -LiteralPath (Join-Path $taskClientFolder 'saticobject-e_ClientDat.txt'))
            $taskFileEdit=@(Get-Content -LiteralPath (Join-Path $taskClientFolder 'staticobject-e_FileEdit.txt'))
            [xml]$taskDoorXml=Get-Content (Join-Path (Join-Path $taskCombined 'acis') ($taskVariant+'\doors.xml')) -Raw
            if ($taskClientDat.Count-ne @($taskDoorXml.list.door).Count -or $taskFileEdit.Count-ne $taskClientDat.Count) { throw 'Staticobject companion row count mismatch' }
            $taskPrevious=0
            for ($taskIndex=0;$taskIndex-lt $taskFileEdit.Count;$taskIndex++) {
                $taskFields=$taskFileEdit[$taskIndex].Split("`t")
                if ($taskFields.Count-ne 2 -or [int]$taskFields[0]-le $taskPrevious -or !$taskFields[1] -or $taskFields[1].Contains('.')) { throw 'Invalid or unsorted mesh-name companion' }
                $taskPrevious=[int]$taskFields[0]
                if ($taskClientDat[$taskIndex]-cne "staticobject_begin`tid=$($taskFields[0])`tname=[$($taskFields[1])]`tstaticobject_end") { throw 'ClientDat and FileEdit companion rows differ' }
            }
        }
    }
    $taskElement=if ($taskMode-eq 'doors') { 'door' } else { 'zone' }
    foreach ($taskProfile in @('l2jserver','acis','l2scripts','mobius')) {
        foreach ($taskVariant in @('','Classic')) {
            $taskFolder=Join-Path (Join-Path $taskSplit $taskProfile) $taskVariant
            $taskExpected=@()
            foreach ($taskFile in (Get-ChildItem -LiteralPath $taskFolder -File -Filter '*.xml' | Sort-Object Name)) {
                [xml]$taskXml=Get-Content -LiteralPath $taskFile.FullName -Raw
                $taskExpected+=@($taskXml.SelectNodes('/list/'+$taskElement) | ForEach-Object OuterXml)
            }
            $taskCombinedFolder=Join-Path (Join-Path $taskCombined $taskProfile) $taskVariant
            [xml]$taskXml=Get-Content -LiteralPath (Join-Path $taskCombinedFolder ($taskMode+'.xml')) -Raw
            $taskActual=@($taskXml.SelectNodes('/list/'+$taskElement) | ForEach-Object OuterXml)
            if (!$taskExpected.Count -or ($taskActual -join "`n")-cne ($taskExpected -join "`n")) { throw "$taskProfile $taskMode $taskVariant merge changed rows" }
            if (@(Get-ChildItem -LiteralPath $taskCombinedFolder -File -Filter '*.xml').Count-ne 1) { throw 'Combined export also published per-map XML' }
        }
    }
    $taskSingle=Join-Path $taskReceipt ($taskMode+'-acis')
    & $taskExe $taskMode $taskInput --all-variants --combine-xml --server acis --output $taskSingle
    if ($LASTEXITCODE -or !(Test-Path (Join-Path $taskSingle ('acis\'+$taskMode+'.xml'))) -or (Test-Path (Join-Path $taskSingle 'mobius')) -or (Test-Path (Join-Path $taskSingle 'l2off'))) { throw 'Server profile filter failed' }
}
Copy-Item -LiteralPath $DoorMap -Destination (Join-Path $taskDoorFolder '24_17.unr')
& $taskExe doors $taskDoorFolder --all-variants --combine-xml --server acis --output (Join-Path $taskReceipt 'duplicate-doors')
if (!$LASTEXITCODE -or (Test-Path (Join-Path $taskReceipt 'duplicate-doors\acis\doors.xml'))) { throw 'Duplicate combined door IDs were published' }
if ((Get-FileHash -LiteralPath $DoorMap).Hash-ne $taskDoorHash -or (Get-FileHash -LiteralPath $WaterMap).Hash-ne $taskWaterHash) { throw 'Source map changed' }
Write-Output "PASS: split/combined XML equivalence for all profiles, both variants, server filtering, duplicate-ID rejection and unchanged source maps. Receipt: $taskReceipt"
