param(
    [Parameter(Mandatory=$true)][string]$RawTable,
    [Parameter(Mandatory=$true)][string]$L2jServer,
    [Parameter(Mandatory=$true)][string]$ACis,
    [Parameter(Mandatory=$true)][string]$L2Scripts,
    [Parameter(Mandatory=$true)][string]$Mobius,
    [Parameter(Mandatory=$true)][string]$Output,
    [int]$DoorId = 24160001
)
$ErrorActionPreference = 'Stop'
$taskCulture = [Globalization.CultureInfo]::InvariantCulture
function Number([string]$Value) { [double]::Parse($Value, $taskCulture) }
function Read-Door([string]$Path) {
    $taskSettings = [Xml.XmlReaderSettings]::new()
    $taskSettings.DtdProcessing = [Xml.DtdProcessing]::Ignore
    $taskSettings.XmlResolver = $null
    $taskReader = [Xml.XmlReader]::Create([IO.Path]::GetFullPath($Path), $taskSettings)
    try {
        $taskDocument = [Xml.XmlDocument]::new()
        $taskDocument.XmlResolver = $null
        $taskDocument.Load($taskReader)
        $taskDoor = $taskDocument.SelectSingleNode("/list/door[@id='$DoorId']")
        if (!$taskDoor) { throw "Door $DoorId missing from supplied reference" }
        return $taskDoor
    } finally { $taskReader.Dispose() }
}
if (Test-Path -LiteralPath $Output) { throw 'Choose a new comparison output folder' }
$taskRaw = @(Import-Csv -LiteralPath $RawTable -Delimiter "`t" | Where-Object { $_.id -eq "$DoorId" } | Sort-Object { [int]$_.node })
if ($taskRaw.Count -ne 4) { throw 'Expected exactly four raw client nodes for the door' }
$taskReferences = [ordered]@{ l2jserver=$L2jServer; acis=$ACis; l2scripts=$L2Scripts; mobius=$Mobius }
$taskRows = @()
$taskHeights = @()
foreach ($taskProfile in $taskReferences.Keys) {
    $taskDoor = Read-Door $taskReferences[$taskProfile]
    $taskNodes = @()
    $taskMin = $null
    $taskMax = $null
    switch ($taskProfile) {
        l2jserver {
            1..4 | ForEach-Object {
                $taskXY = $taskDoor.GetAttribute("node$_").Split(',')
                $taskNodes += ,@((Number $taskXY[0]), (Number $taskXY[1]))
            }
            $taskMin = Number $taskDoor.GetAttribute('nodeZ')
            $taskMax = $taskMin + (Number $taskDoor.GetAttribute('height'))
        }
        acis {
            foreach ($taskNode in $taskDoor.coordinates.loc) {
                $taskNodes += ,@((Number $taskNode.x), (Number $taskNode.y))
            }
            $taskMin = ($taskDoor.coordinates.loc | ForEach-Object { Number $_.z } | Measure-Object -Minimum).Minimum
            $taskMax = $taskMin + (Number $taskDoor.stats.height)
        }
        l2scripts {
            foreach ($taskPair in @(@('ax','ay'),@('bx','by'),@('cx','cy'),@('dx','dy'))) {
                $taskNodes += ,@((Number $taskDoor.shape.GetAttribute($taskPair[0])), (Number $taskDoor.shape.GetAttribute($taskPair[1])))
            }
            $taskMin = Number $taskDoor.shape.minz
            $taskMax = Number $taskDoor.shape.maxz
        }
        mobius {
            foreach ($taskNode in $taskDoor.nodes.node) { $taskNodes += ,@((Number $taskNode.x), (Number $taskNode.y)) }
            $taskMin = Number $taskDoor.nodes.nodeZ
            if ($taskDoor.location.HasAttribute('height')) { $taskMax = $taskMin + (Number $taskDoor.location.height) }
        }
    }
    if ($taskNodes.Count -ne 4) { throw "Expected four $taskProfile reference nodes" }
    for ($taskIndex=0; $taskIndex -lt 4; $taskIndex++) {
        $taskClient = $taskRaw[$taskIndex]
        $taskRows += [pscustomobject]@{
            profile=$taskProfile; id=$DoorId; node=$taskIndex
            deltaX=(Number $taskClient.deltaX); deltaY=(Number $taskClient.deltaY); deltaZ=(Number $taskClient.deltaZ)
            clientX=(Number $taskClient.worldX); clientY=(Number $taskClient.worldY)
            serverX=$taskNodes[$taskIndex][0]; serverY=$taskNodes[$taskIndex][1]
            differenceX=($taskNodes[$taskIndex][0]-(Number $taskClient.worldX))
            differenceY=($taskNodes[$taskIndex][1]-(Number $taskClient.worldY))
        }
    }
    $taskHeights += [pscustomobject]@{ profile=$taskProfile; clientRangeHeight=(Number $taskRaw[0].rangeHeight); clientMinZ=(Number $taskRaw[0].minZ); clientMaxZ=(Number $taskRaw[0].maxZ); serverMinZ=$taskMin; serverMaxZ=$taskMax }
}
New-Item -ItemType Directory -Path $Output | Out-Null
$taskRows | Export-Csv -LiteralPath (Join-Path $Output '911reg-door-nodes.csv') -NoTypeInformation -Encoding utf8
$taskHeights | Export-Csv -LiteralPath (Join-Path $Output '911reg-door-heights.csv') -NoTypeInformation -Encoding utf8
$taskLines = @("# 911reg door comparison: $DoorId",'', 'Differences are server minus client world coordinates. Node order is preserved; no geometry is forced to match a server.', '', '| Server | Largest absolute XY difference | Min Z | Max Z |', '| --- | ---: | ---: | ---: |')
foreach ($taskHeight in $taskHeights) {
    $taskErrors = @($taskRows | Where-Object profile -eq $taskHeight.profile | ForEach-Object { [Math]::Abs($_.differenceX); [Math]::Abs($_.differenceY) })
    $taskLargest = ($taskErrors | Measure-Object -Maximum).Maximum.ToString('0.######',$taskCulture)
    $taskTop = if ($null -eq $taskHeight.serverMaxZ) { 'not specified' } else { $taskHeight.serverMaxZ }
    $taskLines += "| $($taskHeight.profile) | $taskLargest | $($taskHeight.serverMinZ) | $taskTop |"
}
$taskLines += @('',"Client RangeHeight: $($taskRaw[0].rangeHeight). Conservative exported Z bounds: $($taskRaw[0].minZ) to $($taskRaw[0].maxZ).",'', 'A missing server height stays unspecified. Server gameplay stats and manual geometry edits are not client extraction truth.')
[IO.File]::WriteAllLines((Join-Path $Output '911reg-comparison.md'),$taskLines,[Text.UTF8Encoding]::new($false))
Write-Output "Compared door $DoorId across four profiles. Results saved."
