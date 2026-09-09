[CmdletBinding()]
param(
    [string] $Compressor,
    [string] $Output
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$Compressor) {
    $Compressor = Join-Path $repositoryRoot 'bin\x64\Release\utils\xrCompress.exe'
}
if (!$Output) {
    $Output = Join-Path $repositoryRoot 'build\android-fixtures\gamedata.db0'
}

$Compressor = [IO.Path]::GetFullPath($Compressor)
$Output = [IO.Path]::GetFullPath($Output)
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'fixtures\xrarchive\source\gamedata'))
$config = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'fixtures\xrarchive\pack.ltx'))

if (!(Test-Path -LiteralPath $Compressor -PathType Leaf)) {
    throw "xrCompress was not found: $Compressor. Build the desktop Release x64 baseline first."
}
if ($Compressor.Contains(' ') -or $Output.Contains(' ') -or $config.Contains(' ') -or $source.Contains(' ')) {
    throw 'xrCompress command-line parsing does not support spaces in fixture paths.'
}

New-Item -ItemType Directory -Force -Path (Split-Path $Output -Parent) | Out-Null
$outputDirectory = Split-Path $Output -Parent
$compressorExit = -1
Push-Location $outputDirectory
try {
    & $Compressor $source -store -filename $Output -ltx $config
    $compressorExit = $LASTEXITCODE
}
finally {
    Pop-Location
}
if ($compressorExit -ne 0) {
    throw "xrCompress failed with exit code $compressorExit"
}
if (!(Test-Path -LiteralPath $Output -PathType Leaf)) {
    throw "xrCompress did not create the expected archive: $Output"
}

$archive = Get-Item -LiteralPath $Output
$hash = Get-FileHash -LiteralPath $Output -Algorithm SHA256
Write-Host "Generated OpenXRay archive fixture: $($archive.FullName)"
Write-Host "Size: $($archive.Length) bytes"
Write-Host "SHA-256: $($hash.Hash)"
