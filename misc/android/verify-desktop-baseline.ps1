[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Mixed', 'Release', 'Release Master Gold')]
    [string] $Configuration = 'Release',

    [ValidateSet('x64', 'x86')]
    [string] $Platform = 'x64',

    [ValidatePattern('^v[0-9]+$')]
    [string] $PlatformToolset
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vsWhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio with the Desktop development with C++ workload.'
}

$visualStudioRoot = (& $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (!$visualStudioRoot) {
    throw 'A Visual Studio C++ installation was not found.'
}

$vsDevCmd = Join-Path $visualStudioRoot 'Common7\Tools\VsDevCmd.bat'
$solution = Join-Path $PSScriptRoot '..\..\src\engine.sln'
$architecture = if ($Platform -eq 'x64') { 'x64' } else { 'x86' }
$commonArguments = @(
    '/m',
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    '/v:minimal'
)
if ($PlatformToolset) {
    $commonArguments += "/p:PlatformToolset=$PlatformToolset"
}

function Invoke-DeveloperCommand {
    param([Parameter(Mandatory)][string[]] $Arguments)

    $quotedArguments = $Arguments | ForEach-Object { '"' + $_.Replace('"', '""') + '"' }
    $command = 'call "{0}" -arch={1} -host_arch=x64 && msbuild "{2}" {3}' -f `
        $vsDevCmd, $architecture, $solution, ($quotedArguments -join ' ')
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE"
    }
}

Write-Host 'Restoring legacy packages.config dependencies...'
Invoke-DeveloperCommand -Arguments (@('/t:Restore', '/p:RestorePackagesConfig=true') + $commonArguments)

Write-Host "Building desktop baseline: $Configuration|$Platform"
Invoke-DeveloperCommand -Arguments $commonArguments
