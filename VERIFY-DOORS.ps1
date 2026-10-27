param(
    [Parameter(Mandatory=$true)][string]$Map,
    [int]$DoorId=24160001,
    [string]$RangeMap,
    [string]$L2jServerSchema,
    [string]$MobiusSchema
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\'+[guid]::NewGuid().ToString('N'))
$taskExe=Join-Path $PSScriptRoot 'dist\MapExtractor.exe'
& $taskExe --verify-core
if ($LASTEXITCODE -ne 0) { throw 'Door core checks failed' }
$taskExport=Join-Path $taskReceipt 'doors'
$taskStem=[IO.Path]::GetFileNameWithoutExtension($Map)
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
& $taskExe doors $Map --output $taskExport
if ($LASTEXITCODE -ne 0) { throw 'Single-map door export failed' }
$taskRaw=@(Import-Csv (Join-Path $taskExport "raw\$taskStem.tsv") -Delimiter "`t" | Where-Object id -eq "$DoorId" | Sort-Object { [int]$_.node })
if ($taskRaw.Count -ne 4) { throw 'Expected four raw nodes' }
foreach ($taskProfile in @('l2jserver','acis','l2scripts','mobius')) {
    [xml]$taskXml=Get-Content (Join-Path $taskExport "$taskProfile\$taskStem.xml") -Raw
    $taskDoor=$taskXml.SelectSingleNode("/list/door[@id='$DoorId']")
    if (!$taskDoor) { throw "Missing $taskProfile door" }
    for ($taskIndex=0; $taskIndex -lt 4; $taskIndex++) {
        $taskX=[double]$taskRaw[$taskIndex].exportX
        $taskY=[double]$taskRaw[$taskIndex].exportY
        switch ($taskProfile) {
            l2jserver { $taskXY=$taskDoor.GetAttribute('node'+($taskIndex+1)).Split(',') }
            acis { $taskXY=@($taskDoor.coordinates.loc[$taskIndex].x,$taskDoor.coordinates.loc[$taskIndex].y) }
            l2scripts { $taskLetter=@('a','b','c','d')[$taskIndex]; $taskXY=@($taskDoor.shape.GetAttribute($taskLetter+'x'),$taskDoor.shape.GetAttribute($taskLetter+'y')) }
            mobius { $taskXY=@($taskDoor.nodes.node[$taskIndex].x,$taskDoor.nodes.node[$taskIndex].y) }
        }
        if ([int]$taskXY[0] -ne $taskX -or [int]$taskXY[1] -ne $taskY) { throw "Geometry changed in $taskProfile node $taskIndex" }
    }
}
foreach ($taskProfile in @('l2jserver','mobius')) {
    $taskSchema=if ($taskProfile -eq 'l2jserver') { $L2jServerSchema } else { $MobiusSchema }
    if (!$taskSchema) { continue }
    $taskErrors=[Collections.Generic.List[string]]::new()
    $taskSettings=[Xml.XmlReaderSettings]::new()
    $taskSettings.ValidationType=[Xml.ValidationType]::Schema
    $taskSettings.Schemas.XmlResolver=[Xml.XmlUrlResolver]::new()
    $null=$taskSettings.Schemas.Add($null,[IO.Path]::GetFullPath($taskSchema))
    $taskSettings.add_ValidationEventHandler({param($sender,$eventArgs) $taskErrors.Add($eventArgs.Message)})
    $taskReader=[Xml.XmlReader]::Create((Join-Path $taskExport "$taskProfile\$taskStem.xml"),$taskSettings)
    try { while ($taskReader.Read()) {} } finally { $taskReader.Dispose() }
    if ($taskErrors.Count) { throw ($taskErrors -join "`n") }
}
$taskClassic=Join-Path $taskReceipt ($taskStem+'_Classic.unr')
Copy-Item -LiteralPath $Map -Destination $taskClassic
$taskClassicOutput=Join-Path $taskReceipt 'classic'
& $taskExe doors $taskClassic --output $taskClassicOutput --classic
if ($LASTEXITCODE -notin @(0,2)) { throw 'Classic fixture export failed' }
foreach ($taskProfile in @('l2jserver','acis','l2scripts','mobius')) {
    if (!(Test-Path (Join-Path $taskClassicOutput "$taskProfile\$taskStem.xml"))) { throw 'Classic output must use the normal region name' }
}
& $taskExe --verify-ui-doors $Map (Join-Path $taskReceipt 'ui')
if ($LASTEXITCODE -ne 0) { throw 'Door UI workflow failed' }
& $taskExe --verify-languages
if ($LASTEXITCODE -ne 0) { throw 'Language verification failed' }
if ($RangeMap) {
    $taskRangeOutput=Join-Path $taskReceipt 'range-regression'
    & $taskExe doors $RangeMap --output $taskRangeOutput
    if ($LASTEXITCODE -notin @(0,2)) { throw 'Range regression extraction failed' }
    $taskRangeStem=[IO.Path]::GetFileNameWithoutExtension($RangeMap)
    $taskRanges=@(Import-Csv (Join-Path $taskRangeOutput "raw\$taskRangeStem.tsv") -Delimiter "`t" | Where-Object actor -eq 'Mover6' | Sort-Object { [int]$_.node })
    $taskExpected=@(@(-3.687500,1.843750,-56.745117),@(-4.031250,-52.265625,-56.745117),@(4.289063,-52.015625,-56.745117),@(4.195313,1.843750,-56.745117))
    if ($taskRanges.Count -ne 4) { throw 'Known Mover6 missing' }
    for ($taskIndex=0; $taskIndex -lt 4; $taskIndex++) {
        $taskActual=@([double]$taskRanges[$taskIndex].deltaX,[double]$taskRanges[$taskIndex].deltaY,[double]$taskRanges[$taskIndex].deltaZ)
        for ($taskAxis=0; $taskAxis -lt 3; $taskAxis++) {
            if ([Math]::Abs($taskActual[$taskAxis]-$taskExpected[$taskIndex][$taskAxis]) -gt 0.00000051) { throw 'Authored RangeDelta regression' }
        }
    }
}
Write-Output "PASS: four profiles preserve client geometry; requested schemas validate; Classic output normalization, single-map UI, languages and optional RangeDelta fixture pass. Receipt: $taskReceipt"
