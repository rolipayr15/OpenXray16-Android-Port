[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $SdkRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$versions = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $PSScriptRoot 'toolchain.properties')) {
    $trimmed = $line.Trim()
    if (!$trimmed -or $trimmed.StartsWith('#')) {
        continue
    }
    $parts = $trimmed.Split('=', 2)
    $versions[$parts[0].Trim()] = $parts[1].Trim()
}

$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
$hostTag = if ($IsWindows) { 'windows-x86_64' } elseif ($IsMacOS) { 'darwin-x86_64' } else { 'linux-x86_64' }
$executableSuffix = if ($IsWindows) { '.exe' } else { '' }
$ndkRoot = Join-Path $SdkRoot "ndk\$($versions['android.ndk'])"
$toolchainBin = Join-Path $ndkRoot "toolchains\llvm\prebuilt\$hostTag\bin"
$clang = Join-Path $toolchainBin "clang++$executableSuffix"
$cmake = Join-Path $SdkRoot "cmake\$($versions['android.cmake'])\bin\cmake$executableSuffix"
$sdkManagerSuffix = if ($IsWindows) { '.bat' } else { '' }
$sdkManager = Join-Path $SdkRoot "cmdline-tools\$($versions['android.cmdlineTools.version'])\bin\sdkmanager$sdkManagerSuffix"

foreach ($requiredPath in @($clang, $cmake, $sdkManager)) {
    if (!(Test-Path -LiteralPath $requiredPath)) {
        throw "Required Android tool is missing: $requiredPath"
    }
}

$api = $versions['android.minSdk']
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$platformProbe = Join-Path $PSScriptRoot 'platform-probe.cpp'
$targets = @(
    @{ Abi = $versions['android.primaryAbi']; Triple = "aarch64-linux-android$api" },
    @{ Abi = $versions['android.secondaryAbi']; Triple = "armv7a-linux-androideabi$api" }
)

foreach ($target in $targets) {
    & $clang --target=$($target.Triple) -x c++ -std=c++17 -fsyntax-only `
        -DNDEBUG -DXRAY_STATIC_BUILD -I (Join-Path $repositoryRoot 'src') $platformProbe
    if ($LASTEXITCODE -ne 0) {
        throw "NDK compiler smoke test failed for $($target.Abi)"
    }
    Write-Host "OK: $($target.Abi) ($($target.Triple))"
}

Write-Host "OK: NDK $($versions['android.ndk'])"
Write-Host "OK: CMake $($versions['android.cmake'])"
Write-Host "OK: Android SDK toolchain root $SdkRoot"
