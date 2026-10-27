param([Parameter(Mandatory=$true)][string]$Map)
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$taskReceipt = Join-Path $taskRoot ('Tests\MapExtractor\' + [guid]::NewGuid().ToString('N'))
$taskInput = Join-Path $taskReceipt 'input'
$taskExe = Join-Path $PSScriptRoot 'dist\MapExtractor.exe'
New-Item -ItemType Directory -Path $taskInput -Force | Out-Null
& $taskExe --verify-history (Join-Path $taskReceipt 'history')
if ($LASTEXITCODE -ne 0) { throw 'Client history verification failed' }
Copy-Item -LiteralPath $Map -Destination (Join-Path $taskInput 'valid.unr')
Copy-Item -LiteralPath $Map -Destination (Join-Path $taskInput 'valid-second.unr')
& $taskExe scan $taskInput --output (Join-Path $taskReceipt 'single.json') --threads 1
if ($LASTEXITCODE -ne 0) { throw 'Single worker scan failed' }
& $taskExe scan $taskInput --output (Join-Path $taskReceipt 'parallel.json') --threads 4
if ($LASTEXITCODE -ne 0) { throw 'Parallel scan failed' }
if ((Get-FileHash (Join-Path $taskReceipt 'single.json')).Hash -ne (Get-FileHash (Join-Path $taskReceipt 'parallel.json')).Hash) {
    throw 'Worker counts changed manifest contents'
}
$taskBefore = (Get-FileHash (Join-Path $taskReceipt 'single.json')).Hash
$taskUiOutput = Join-Path $taskReceipt 'ui-results'
1..2 | ForEach-Object {
    & $taskExe --verify-ui $taskInput $taskUiOutput
    if ($LASTEXITCODE -ne 0) { throw 'UI workflow failed' }
}
$taskSummaries = @(Get-ChildItem $taskUiOutput -Filter Summary.txt -Recurse)
if ($taskSummaries.Count -ne 2) { throw 'UI runs did not create separate results folders' }
foreach ($taskSummary in $taskSummaries) {
    if ((Get-Content $taskSummary.FullName -Raw) -notmatch 'Finished: 2 maps checked. 0 need attention.') {
        throw 'Readable UI summary missing map counts'
    }
}
& $taskExe scan $taskInput --output (Join-Path $taskReceipt 'single.json')
if ($LASTEXITCODE -ne 1 -or (Get-FileHash (Join-Path $taskReceipt 'single.json')).Hash -ne $taskBefore) {
    throw 'Existing output protection failed'
}
[IO.File]::WriteAllBytes((Join-Path $taskInput 'truncated.unr'), [byte[]](1,2,3))
& $taskExe scan $taskInput --output (Join-Path $taskReceipt 'mixed.json') --threads 4
if ($LASTEXITCODE -ne 2) { throw 'Corrupt map was not reported as partial failure' }
$taskMixed = Get-Content (Join-Path $taskReceipt 'mixed.json') -Raw | ConvertFrom-Json
if ($taskMixed.map_count -ne 3 -or $taskMixed.error_count -ne 1) { throw 'Incorrect partial failure manifest' }
& $taskExe scan $taskInput --output (Join-Path $taskReceipt 'bad.json') --threads 0
if ($LASTEXITCODE -ne 1 -or (Test-Path (Join-Path $taskReceipt 'bad.json'))) { throw 'Invalid worker count accepted' }
Write-Output "PASS: deterministic inventory, overwrite protection, corrupt-map isolation, worker validation, UI workflow, readable summaries, separate run folders. Receipt: $taskReceipt"
exit 0
