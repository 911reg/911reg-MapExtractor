param(
    [Parameter(Mandatory=$true)][string]$DoorMap,
    [Parameter(Mandatory=$true)][string]$WaterMap,
    [string]$ReferenceScript,
    [string]$Executable
)
$ErrorActionPreference='Stop'
$taskExe=if ($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $PSScriptRoot 'dist\MapExtractor.exe' }
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
$taskHashes=@{}
foreach ($taskPath in @($DoorMap,$WaterMap)) { $taskHashes[$taskPath]=(Get-FileHash -LiteralPath $taskPath).Hash }
function Read-OffText([string]$Path) {
    $taskBytes=[IO.File]::ReadAllBytes($Path)
    if ($taskBytes.Length -lt 2 -or $taskBytes[0] -ne 255 -or $taskBytes[1] -ne 254 -or $taskBytes.Length%2) { throw 'L2OFF output is not UTF-16LE with BOM' }
    $taskText=[Text.UnicodeEncoding]::new($false,$true,$true).GetString($taskBytes,2,$taskBytes.Length-2)
    if ($taskText -match '(?<!\r)\n' -or $taskText.Contains('{{')) { throw 'Invalid L2OFF newline or unresolved placeholder' }
    return $taskText
}
& $taskExe --verify-water-core
if ($LASTEXITCODE) { throw 'Water/L2OFF geometry checks failed' }
foreach ($taskKind in @('doors','water')) {
    $taskMap=if ($taskKind -eq 'doors') { $DoorMap } else { $WaterMap }
    & $taskExe $taskKind $taskMap --output (Join-Path $taskReceipt $taskKind)
    if ($LASTEXITCODE) { throw "$taskKind export was incomplete" }
}
$taskDoorStem=[IO.Path]::GetFileNameWithoutExtension($DoorMap)
$taskDoorText=Read-OffText (Join-Path $taskReceipt "doors\l2off\$taskDoorStem.txt")
$taskRaw=@(Import-Csv (Join-Path $taskReceipt "doors\raw\$taskDoorStem.tsv") -Delimiter "`t")
$taskPoint='\{\s*(?<n>-?\d+)\s*;\s*(?<n>-?\d+)\s*;\s*(?<n>-?\d+)\s*\}'
$taskRange='range=\{\s*'+(($taskPoint,$taskPoint,$taskPoint,$taskPoint) -join '\s*;\s*')+'\s*\}'
$taskRecords=@($taskDoorText -split '\r\n' | Where-Object { $_.Trim() })
if ($taskRecords.Count -ne @($taskRaw | Group-Object id).Count) { throw 'L2OFF door count mismatch' }
$taskIds=@{}
foreach ($taskLine in $taskRecords) {
    if ($taskLine -notmatch '^door_begin \[[A-Za-z0-9_]+\] type=normal_type editor_id=(\d+) open_method=by_npc height=(\d+) hp=\d+ physical_defence=\d+ magical_defence=\d+ pos=.+ range=.+ door_end$') { throw 'Invalid L2OFF door grammar' }
    $taskId=$Matches[1]; $taskHeight=[int]$Matches[2]
    if ($taskIds.ContainsKey($taskId)) { throw 'Repeated L2OFF editor_id' }; $taskIds[$taskId]=$true
    $taskNodes=@($taskRaw | Where-Object id -eq $taskId | Sort-Object { [int]$_.node })
    if ($taskNodes.Count -ne 4 -or $taskHeight -ne [Math]::Ceiling([double]$taskNodes[0].rangeHeight)) { throw 'L2OFF door height mismatch' }
    $taskParsed=[regex]::Match($taskLine,$taskRange)
    if (!$taskParsed.Success -or $taskParsed.Groups['n'].Captures.Count -ne 12) { throw 'Invalid door range syntax' }
    for ($taskI=0;$taskI -lt 4;$taskI++) {
        for ($taskAxis=0;$taskAxis -lt 3;$taskAxis++) {
            $taskField=@('worldX','worldY','worldZ')[$taskAxis]
            $taskExpected=[Math]::Round([double]$taskNodes[$taskI].$taskField,0,[MidpointRounding]::AwayFromZero)
            if ([int]$taskParsed.Groups['n'].Captures[$taskI*3+$taskAxis].Value -ne $taskExpected) { throw 'L2OFF range changed client geometry' }
        }
    }
    $taskPos=[regex]::Match($taskLine,'pos='+$taskPoint)
    for ($taskAxis=0;$taskAxis -lt 3;$taskAxis++) {
        $taskField=@('locationX','locationY','locationZ')[$taskAxis]
        if (!$taskPos.Success -or [int]$taskPos.Groups['n'].Captures[$taskAxis].Value -ne [Math]::Round([double]$taskNodes[0].$taskField,0,[MidpointRounding]::AwayFromZero)) { throw 'L2OFF position mismatch' }
    }
}
$taskWaterStem=[IO.Path]::GetFileNameWithoutExtension($WaterMap)
$taskWaterText=Read-OffText (Join-Path $taskReceipt "water\l2off\$taskWaterStem.txt")
$taskWaterRaw=@(Import-Csv (Join-Path $taskReceipt "water\raw\$taskWaterStem.zones.tsv") -Delimiter "`t")
$taskBoxes=@()
foreach ($taskLine in ($taskWaterText -split '\r\n' | Where-Object { $_.Trim() })) {
    $taskBox=[regex]::Match($taskLine,'^area_begin name=\[(?<name>[A-Za-z0-9_]+)_box_\d+\] map_no=\{\s*(?<mx>\d+);(?<my>\d+)\s*\} type=water water_range=\{Min:\((?<x1>-?\d+) (?<y1>-?\d+) (?<z1>-?\d+)\) Max:\((?<x2>-?\d+) (?<y2>-?\d+) (?<z2>-?\d+)\)\} area_end$')
    if (!$taskBox.Success) { throw 'Invalid L2OFF water grammar' }
    if (($taskBox.Groups['mx'].Value+'_'+$taskBox.Groups['my'].Value) -ne $taskWaterStem) { throw 'L2OFF water map_no mismatch' }
    $taskRow=@{name=$taskBox.Groups['name'].Value}
    foreach ($taskField in @('x1','y1','z1','x2','y2','z2')) { $taskRow[$taskField]=[int]$taskBox.Groups[$taskField].Value }
    if ($taskRow.x1 -ge $taskRow.x2 -or $taskRow.y1 -ge $taskRow.y2 -or $taskRow.z1 -ge $taskRow.z2) { throw 'Empty/reversed L2OFF water box' }
    $taskBoxes+=,$taskRow
}
foreach ($taskGroup in ($taskWaterRaw | Group-Object name)) {
    $taskRows=@($taskGroup.Group | Sort-Object { [int]$_.node }); $taskArea=0.0; $taskCovered=0.0
    for ($taskI=0;$taskI -lt $taskRows.Count;$taskI++) {
        $taskA=$taskRows[$taskI]; $taskB=$taskRows[($taskI+1)%$taskRows.Count]
        $taskArea+=[double]$taskA.x*[double]$taskB.y-[double]$taskB.x*[double]$taskA.y
    }
    $taskName=[regex]::Replace($taskGroup.Name,'[^A-Za-z0-9_]','_')
    $taskMatching=@($taskBoxes | Where-Object { $_.name -eq $taskName })
    if (!$taskMatching.Count) { throw 'Water zone missing from L2OFF output' }
    foreach ($taskBox in $taskMatching) {
        if ($taskBox.z1 -ne [int]$taskRows[0].minZ -or $taskBox.z2 -ne [int]$taskRows[0].maxZ) { throw 'L2OFF water height mismatch' }
        $taskCovered+=([double]$taskBox.x2-$taskBox.x1)*([double]$taskBox.y2-$taskBox.y1)
    }
    if ($taskCovered -ne [Math]::Abs($taskArea)/2) { throw 'L2OFF water area changed' }
}
if ($ReferenceScript) {
    foreach ($taskName in @('doordata.txt','areadata.txt')) {
        $taskPath=Join-Path $ReferenceScript $taskName; $taskHashes[$taskPath]=(Get-FileHash -LiteralPath $taskPath).Hash
    }
    $taskReferenceDoors=Get-Content (Join-Path $ReferenceScript 'doordata.txt') -Encoding Unicode
    $taskShared=0; $taskMaxDelta=0
    foreach ($taskLine in $taskRecords) {
        $taskId=[regex]::Match($taskLine,'editor_id=(\d+)').Groups[1].Value
        $taskOriginal=@($taskReferenceDoors | Where-Object { $_ -notmatch '^\s*//' -and $_ -match "\beditor_id=$taskId\b" })
        if ($taskOriginal.Count -eq 1) {
            $taskA=[regex]::Match($taskLine,$taskRange); $taskB=[regex]::Match($taskOriginal[0],$taskRange)
            if (!$taskB.Success) { throw 'Reference door range format differs' }
            for ($taskI=0;$taskI -lt 12;$taskI++) { $taskMaxDelta=[Math]::Max($taskMaxDelta,[Math]::Abs([int]$taskA.Groups['n'].Captures[$taskI].Value-[int]$taskB.Groups['n'].Captures[$taskI].Value)) }
            $taskShared++
        }
    }
    $taskReferenceWater=@(Get-Content (Join-Path $ReferenceScript 'areadata.txt') -Encoding Unicode | Where-Object { $_ -notmatch '^\s*//' -and $_ -match '\btype=water\b' })
    "Shared door IDs: $taskShared; maximum client/reference range-coordinate difference: $taskMaxDelta; active reference water areas: $($taskReferenceWater.Count). Differences between client releases are diagnostic, not copied over." | Set-Content (Join-Path $taskReceipt 'reference-comparison.txt')
}
foreach ($taskPath in $taskHashes.Keys) { if ((Get-FileHash -LiteralPath $taskPath).Hash -ne $taskHashes[$taskPath]) { throw 'Source/reference file changed' } }
Write-Output "PASS: L2OFF grammar, UTF-16LE/CRLF, IDs, client positions/ranges/heights, exact water area/Z/map number and source hashes. Receipt: $taskReceipt"
