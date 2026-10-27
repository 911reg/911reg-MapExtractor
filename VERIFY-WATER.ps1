param(
    [Parameter(Mandatory=$true)][string]$Map,
    [string]$Executable,
    [string]$L2jServerSchema,
    [string]$MobiusSchema,
    [string]$L2ScriptsDtd
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt=Join-Path $taskRoot ('Tests\MapExtractor\'+[guid]::NewGuid().ToString('N'))
$taskExe=if ($Executable) { [IO.Path]::GetFullPath($Executable) } else { Join-Path $PSScriptRoot 'dist\MapExtractor.exe' }
$taskOriginal=(Get-FileHash -LiteralPath $Map).Hash
$taskStem=[IO.Path]::GetFileNameWithoutExtension($Map)
$taskExport=Join-Path $taskReceipt 'single-map'
New-Item -ItemType Directory -Path $taskReceipt | Out-Null
& $taskExe --verify-water-core
if ($LASTEXITCODE -ne 0) { throw 'Water core checks failed' }
& $taskExe water $Map --output $taskExport
if ($LASTEXITCODE -ne 0) { throw 'Expected a map with representable WaterVolumes and no issues' }
$taskRaw=@(Import-Csv (Join-Path $taskExport "raw\$taskStem.zones.tsv") -Delimiter "`t")
if (!$taskRaw.Count) { throw 'No raw water zones found' }
foreach ($taskProfile in @('l2jserver','acis','l2scripts','mobius')) {
    [xml]$taskXml=Get-Content (Join-Path $taskExport "$taskProfile\$taskStem.xml") -Raw
    foreach ($taskGroup in ($taskRaw | Group-Object name)) {
        $taskName=if ($taskProfile -eq 'l2scripts') { '['+$taskGroup.Name+']' } else { $taskGroup.Name }
        $taskZone=@($taskXml.list.zone | Where-Object { $_.name -eq $taskName })
        if ($taskZone.Count -ne 1) { throw "Water zone missing/duplicated in $taskProfile" }
        $taskZone=$taskZone[0]
        $taskRows=@($taskGroup.Group | Sort-Object { [int]$_.node })
        $taskNodes=if ($taskProfile -eq 'l2scripts') { @($taskZone.polygon.coords) } else { @($taskZone.node) }
        if ($taskNodes.Count -ne $taskRows.Count) { throw "Water footprint changed in $taskProfile" }
        for ($taskIndex=0; $taskIndex -lt $taskRows.Count; $taskIndex++) {
            $taskExpected=$taskRows[$taskIndex]
            if ($taskProfile -eq 'l2scripts') {
                $taskValues=$taskNodes[$taskIndex].loc.Split(' ')
                if ([int]$taskValues[2] -ne [int]$taskExpected.minZ -or [int]$taskValues[3] -ne [int]$taskExpected.maxZ) { throw 'L2Scripts water Z mismatch' }
            } else {
                $taskUpper=$taskProfile -ne 'acis'
                $taskValues=@($taskNodes[$taskIndex].GetAttribute($(if ($taskUpper) { 'X' } else { 'x' })), $taskNodes[$taskIndex].GetAttribute($(if ($taskUpper) { 'Y' } else { 'y' })))
                if ([int]$taskZone.minZ -ne [int]$taskExpected.minZ -or [int]$taskZone.maxZ -ne [int]$taskExpected.maxZ) { throw "$taskProfile water Z mismatch" }
            }
            if ([int]$taskValues[0] -ne [int]$taskExpected.x -or [int]$taskValues[1] -ne [int]$taskExpected.y) { throw "$taskProfile water XY mismatch" }
            if ([double]$taskExpected.minZ -lt [double]$taskExpected.sourceMinZ -or [double]$taskExpected.maxZ -gt [double]$taskExpected.sourceMaxZ) { throw 'Water vertical range expanded outside source' }
        }
    }
}
$taskSchemaCount=0
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
    $taskSchemaCount++
}
if ($L2ScriptsDtd) {
    $taskDocument=Get-Content (Join-Path $taskExport "l2scripts\$taskStem.xml") -Raw
    $taskUri=([Uri][IO.Path]::GetFullPath($L2ScriptsDtd)).AbsoluteUri
    $taskDocument=$taskDocument.Replace('?>',("?>`n<!DOCTYPE list SYSTEM `""+$taskUri+'">'))
    $taskErrors=[Collections.Generic.List[string]]::new()
    $taskSettings=[Xml.XmlReaderSettings]::new()
    $taskSettings.DtdProcessing=[Xml.DtdProcessing]::Parse
    $taskSettings.ValidationType=[Xml.ValidationType]::DTD
    $taskSettings.XmlResolver=[Xml.XmlUrlResolver]::new()
    $taskSettings.MaxCharactersFromEntities=1048576
    $taskSettings.add_ValidationEventHandler({param($sender,$eventArgs) $taskErrors.Add($eventArgs.Message)})
    $taskReader=[Xml.XmlReader]::Create([IO.StringReader]::new($taskDocument),$taskSettings)
    try { while ($taskReader.Read()) {} } finally { $taskReader.Dispose() }
    if ($taskErrors.Count) { throw ($taskErrors -join "`n") }
    $taskSchemaCount++
}
# Two copied maps exercise client scope and suffix/XML escaping without touching client files.
$taskInput=Join-Path $taskReceipt 'Maps'
New-Item -ItemType Directory -Path $taskInput | Out-Null
Copy-Item -LiteralPath $Map -Destination (Join-Path $taskInput ($taskStem+'.unr'))
$taskVariant=$taskStem+'_Classic & 911reg'
Copy-Item -LiteralPath $Map -Destination (Join-Path $taskInput ($taskVariant+'.unr'))
foreach ($taskWorkers in @(1,4)) {
    & $taskExe water $taskInput --output (Join-Path $taskReceipt "client-$taskWorkers") --threads $taskWorkers
    if ($LASTEXITCODE -ne 2) { throw 'Expected L2OFF-only omission for nonnumeric escaping fixture' }
    $taskProfileReport=Get-Content (Join-Path $taskReceipt "client-$taskWorkers\911reg-water-report.txt") -Raw
    $taskExpectedOmissions=@($taskRaw | Group-Object name).Count
    if (!$taskProfileReport.Contains("L2OFF-only rejected zones: $taskExpectedOmissions") -or !$taskProfileReport.Contains('Rejected candidates: 0') -or !$taskProfileReport.Contains('Unreadable/failed maps: 0')) { throw 'Unexpected water extraction issues' }
}
foreach ($taskProfile in @('l2jserver','acis','l2scripts','mobius')) {
    foreach ($taskName in @($taskStem,$taskVariant)) {
        $taskRelative="$taskProfile\$taskName.xml"
        $taskSerial=Join-Path $taskReceipt "client-1\$taskRelative"
        $taskParallel=Join-Path $taskReceipt "client-4\$taskRelative"
        if ((Get-FileHash -LiteralPath $taskSerial).Hash -ne (Get-FileHash -LiteralPath $taskParallel).Hash) { throw 'Worker count changed water output' }
        [xml]$taskValidated=Get-Content -LiteralPath $taskParallel -Raw
        if (!$taskValidated.list.zone) { throw 'Missing Classic variant water zone' }
    }
}
& $taskExe --verify-ui-water $Map (Join-Path $taskReceipt 'ui')
if ($LASTEXITCODE -ne 0) { throw 'Water UI workflow failed' }
& $taskExe --verify-languages
if ($LASTEXITCODE -ne 0) { throw 'Water translations failed' }
if ((Get-FileHash -LiteralPath $Map).Hash -ne $taskOriginal) { throw 'Source client map was modified' }
Write-Output "PASS: WaterVolume geometry, four XML layouts, $taskSchemaCount schemas/DTDs, map/client scope, Classic names, escaping, determinism, UI, translations and read-only source. Receipt: $taskReceipt"
exit 0
