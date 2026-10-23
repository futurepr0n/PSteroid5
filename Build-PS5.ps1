# -LiveServer https://asteroids.futurepr0n.com adds read-only GETs against that server to the tests.
param([string]$LiveServer='')
$ErrorActionPreference='Stop'
$repo=$PSScriptRoot
$linuxRepo=(wsl.exe -d Ubuntu -- wslpath -a $repo.Replace('\','/')).Trim()
$nativePreference=$ErrorActionPreference
try {
    # Windows PowerShell can treat ordinary native stderr (test progress/warnings)
    # as a terminating error under redirection. Trust the process exit status.
    $ErrorActionPreference='Continue'
    wsl.exe -d Ubuntu -- env "PSTEROIDS_LIVE=$LiveServer" bash "$linuxRepo/ps5/build.sh"
    $buildExit=$LASTEXITCODE
} finally { $ErrorActionPreference=$nativePreference }
if($buildExit -ne 0) { throw "PS5 build failed (exit $buildExit)." }
$out=Join-Path $repo 'dist\PPSA99305'
if(Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Path $out -Force | Out-Null
$source='\\wsl.localhost\Ubuntu\home\futurepr0n\work\psteroids5-app\dist\PPSA99305'
Get-ChildItem -LiteralPath $source -Force | Copy-Item -Destination $out -Recurse -Force
python (Join-Path $repo '..\PSPGameCPP\ps5\validate_package.py') $out --generic --title-id PPSA99305 --write-report
if($LASTEXITCODE -ne 0) { throw 'Folder-title validation failed.' }
Copy-Item -LiteralPath (Join-Path $out 'validation.json') -Destination (Join-Path $repo 'ps5\validation.json') -Force
Write-Host "Built: $out"
