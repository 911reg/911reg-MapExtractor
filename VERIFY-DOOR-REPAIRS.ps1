param([Parameter(Mandatory=$true)][string]$Client,[string]$Executable)
$ErrorActionPreference='Stop'
$taskExe=if ($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $PSScriptRoot 'dist\MapExtractor.exe' }
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\repairs-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
$taskMaps=@('19_26_Classic','20_22','20_22_Classic','21_17','21_24','21_24_Classic','22_19','22_19_Classic','22_25','23_17','23_17_Classic','23_20','23_20_Classic','23_21','23_22','23_22_Classic','23_25','25_13_Classic','25_19','25_19_Classic') | ForEach-Object { Join-Path ([IO.Path]::GetFullPath($Client)) ('Maps\'+$_+'.unr') }
$taskHashes=@{}
foreach ($taskMap in $taskMaps) { $taskHashes[$taskMap]=(Get-FileHash -LiteralPath $taskMap).Hash }
$taskList=Join-Path $taskReceipt 'selection.txt'
[IO.File]::WriteAllLines($taskList,$taskMaps,[Text.UTF8Encoding]::new($false))
foreach ($taskWorkers in @(1,4)) {
    $taskOutput=Join-Path $taskReceipt ('workers-'+$taskWorkers)
    & $taskExe doors $Client --all-variants --map-list $taskList --threads $taskWorkers --output $taskOutput *> (Join-Path $taskReceipt ('workers-'+$taskWorkers+'.log'))
    if ($LASTEXITCODE) { throw 'Affected maps still reject door actors' }
}
$taskOutput=Join-Path $taskReceipt 'workers-4'
foreach ($taskFile in (Get-ChildItem -LiteralPath $taskOutput -File -Recurse)) {
    $taskRelative=[IO.Path]::GetRelativePath($taskOutput,$taskFile.FullName)
    if ((Get-FileHash -LiteralPath $taskFile.FullName).Hash-ne (Get-FileHash -LiteralPath (Join-Path (Join-Path $taskReceipt 'workers-1') $taskRelative)).Hash) { throw "Workers changed $taskRelative" }
}
$taskDuplicates=@(Import-Csv (Join-Path $taskOutput 'raw\Classic\19_26.duplicates.tsv') -Delimiter "`t")
$taskIds=@($taskDuplicates | Group-Object id)
if ($taskIds.Count-ne 21) { throw 'Expected 21 unique duplicate IDs in regression map' }
foreach ($taskRow in $taskDuplicates) { if ([double]$taskRow.keptZ-le [double]$taskRow.discardedZ) { throw 'Upper-placement policy kept a lower actor' } }
$taskSummary=Get-Content (Join-Path $taskReceipt 'workers-4.log') -Raw
if (!$taskSummary.Contains('19_26_Classic: 21 duplicate door IDs. IDs:') -or !$taskSummary.Contains('Duplicate door IDs:')) { throw 'End-of-activity duplicate summary missing' }
$taskRaw=@(Get-ChildItem (Join-Path $taskOutput 'raw') -Recurse -Filter '*.tsv' | Where-Object { !$_.Name.EndsWith('.duplicates.tsv') })
foreach ($taskFile in $taskRaw) {
    foreach ($taskGroup in (Import-Csv -LiteralPath $taskFile.FullName -Delimiter "`t" | Group-Object id)) {
        $taskNodes=@($taskGroup.Group | Sort-Object { [int]$_.node })
        if ($taskNodes.Count-ne 4) { throw 'Door has missing or duplicate nodes' }
        $taskSign=0
        for ($taskIndex=0;$taskIndex-lt 4;$taskIndex++) {
            $taskA=$taskNodes[$taskIndex]; $taskB=$taskNodes[($taskIndex+1)%4]; $taskC=$taskNodes[($taskIndex+2)%4]
            $taskCross=([double]$taskB.exportX-[double]$taskA.exportX)*([double]$taskC.exportY-[double]$taskA.exportY)-([double]$taskB.exportY-[double]$taskA.exportY)*([double]$taskC.exportX-[double]$taskA.exportX)
            if ($taskCross) { if ($taskSign -and [Math]::Sign($taskCross)-ne $taskSign) { throw 'Export polygon crosses itself' }; $taskSign=[Math]::Sign($taskCross) }
            $taskNearest=($taskNodes | ForEach-Object { [Math]::Max([Math]::Abs([double]$_.worldX-[double]$taskA.exportX),[Math]::Abs([double]$_.worldY-[double]$taskA.exportY)) } | Measure-Object -Minimum).Minimum
            if ($taskNearest-ge 1.000001) { throw 'Quantization moved a corner by more than one unit' }
        }
        if (!$taskSign) { throw 'Export polygon is still degenerate' }
    }
}
[xml]$taskXml=Get-Content (Join-Path $taskOutput 'l2jserver\Classic\19_26.xml') -Raw
if ($taskXml.SelectSingleNode('/list/door[@id="19260003"]').pos-ne '-11975;275812;-9065') { throw 'Upper reference placement changed' }
foreach ($taskMap in $taskMaps) { if ((Get-FileHash -LiteralPath $taskMap).Hash-ne $taskHashes[$taskMap]) { throw 'Source map changed' } }
Write-Output "PASS: 20 affected maps, upper duplicates, unique-ID summary, valid quantized polygons, worker determinism and unchanged sources. Receipt: $taskReceipt"
