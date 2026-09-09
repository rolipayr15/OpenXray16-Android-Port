[CmdletBinding()]
param(
    [string] $SdkRoot,
    [switch] $AcceptLicenses,
    [switch] $SkipPackages
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-ToolchainProperties {
    param([Parameter(Mandatory)][string] $Path)

    $result = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trimmed = $line.Trim()
        if (!$trimmed -or $trimmed.StartsWith('#')) {
            continue
        }

        $parts = $trimmed.Split('=', 2)
        if ($parts.Count -ne 2) {
            throw "Invalid toolchain property: $line"
        }
        $result[$parts[0].Trim()] = $parts[1].Trim()
    }
    return $result
}

function Find-JavaHome {
    param([Parameter(Mandatory)][int] $RequiredMajor)

    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($env:JAVA_HOME) {
        $candidates.Add($env:JAVA_HOME)
    }

    $microsoftJdkRoot = Join-Path $env:ProgramFiles 'Microsoft'
    if (Test-Path -LiteralPath $microsoftJdkRoot) {
        Get-ChildItem -LiteralPath $microsoftJdkRoot -Directory -Filter "jdk-$RequiredMajor*" |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add($_.FullName) }
    }

    foreach ($candidate in $candidates) {
        $java = Join-Path $candidate 'bin\java.exe'
        if (!(Test-Path -LiteralPath $java)) {
            continue
        }

        $versionLine = (& $java -version 2>&1 | Select-Object -First 1).ToString()
        $versionPattern = '(?:version |openjdk )"?{0}(?:\.|")' -f $RequiredMajor
        if ($versionLine -match $versionPattern) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    throw "JDK $RequiredMajor was not found. Install it or set JAVA_HOME to a JDK $RequiredMajor directory."
}

function Get-VerifiedArchive {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Url,
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][long] $ExpectedSize,
        [Parameter(Mandatory)][string] $ExpectedSha1
    )

    $currentSize = if (Test-Path -LiteralPath $Path) { (Get-Item -LiteralPath $Path).Length } else { 0 }
    if ($currentSize -gt $ExpectedSize) {
        throw "The cached $Name archive is larger than expected: $Path"
    }

    if ($currentSize -lt $ExpectedSize) {
        $curl = Get-Command curl.exe -ErrorAction Stop
        Write-Host "Downloading $Name ($currentSize/$ExpectedSize bytes)..."
        & $curl.Source --location --fail --retry 5 --retry-all-errors --retry-connrefused `
            --continue-at - --output $Path $Url
        if ($LASTEXITCODE -ne 0) {
            throw "curl failed with exit code $LASTEXITCODE"
        }
    }

    $actualSize = (Get-Item -LiteralPath $Path).Length
    if ($actualSize -ne $ExpectedSize) {
        throw "Unexpected $Name archive size: $actualSize (expected $ExpectedSize)"
    }

    $actualSha1 = (Get-FileHash -LiteralPath $Path -Algorithm SHA1).Hash.ToLowerInvariant()
    if ($actualSha1 -ne $ExpectedSha1.ToLowerInvariant()) {
        throw "$Name checksum mismatch for $Path"
    }
}

$propertiesPath = Join-Path $PSScriptRoot 'toolchain.properties'
$versions = Read-ToolchainProperties -Path $propertiesPath

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$SdkRoot) {
    $workspaceRoot = Split-Path $repositoryRoot -Parent
    $SdkRoot = Join-Path $workspaceRoot '.toolchains\android-sdk'
}
$SdkRoot = [IO.Path]::GetFullPath($SdkRoot)

$javaHome = Find-JavaHome -RequiredMajor ([int]$versions['java.major'])
$env:JAVA_HOME = $javaHome
$env:ANDROID_SDK_ROOT = $SdkRoot
$env:ANDROID_HOME = $SdkRoot

$downloadRoot = Join-Path (Split-Path $SdkRoot -Parent) '.downloads'
New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null

$commandLineToolsVersion = $versions['android.cmdlineTools.version']
$sdkManager = Join-Path $SdkRoot "cmdline-tools\$commandLineToolsVersion\bin\sdkmanager.bat"
if (!(Test-Path -LiteralPath $sdkManager)) {
    New-Item -ItemType Directory -Force -Path (Join-Path $SdkRoot 'cmdline-tools') | Out-Null

    $downloadUrl = $versions['android.cmdlineTools.windows.url']
    $expectedSize = [long]$versions['android.cmdlineTools.windows.size']
    $archiveName = Split-Path ([Uri]$downloadUrl).AbsolutePath -Leaf
    $archivePath = Join-Path $downloadRoot $archiveName
    Get-VerifiedArchive -Name 'Android command-line tools' -Url $downloadUrl -Path $archivePath `
        -ExpectedSize $expectedSize -ExpectedSha1 $versions['android.cmdlineTools.windows.sha1']

    $extractRoot = Join-Path ([IO.Path]::GetTempPath()) ("openxray-android-sdk-" + [guid]::NewGuid().ToString('N'))
    try {
        Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
        $extractedTools = Join-Path $extractRoot 'cmdline-tools'
        if (!(Test-Path -LiteralPath (Join-Path $extractedTools 'bin\sdkmanager.bat'))) {
            throw 'The command-line tools archive has an unexpected layout.'
        }

        $installPath = Join-Path $SdkRoot "cmdline-tools\$commandLineToolsVersion"
        if (Test-Path -LiteralPath $installPath) {
            throw "The incomplete command-line tools directory already exists: $installPath"
        }
        Move-Item -LiteralPath $extractedTools -Destination $installPath
    }
    finally {
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    }
}

if ($SkipPackages) {
    Write-Host "Android command-line tools are ready at $SdkRoot"
    exit 0
}

if ($AcceptLicenses) {
    $answers = ((1..100 | ForEach-Object { 'y' }) -join [Environment]::NewLine)
    $answers | & $sdkManager --sdk_root=$SdkRoot --licenses | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "sdkmanager --licenses failed with exit code $LASTEXITCODE"
    }
}

$packages = @(
    'platform-tools',
    "platforms;android-$($versions['android.compileSdk'])",
    "build-tools;$($versions['android.buildTools'])",
    "cmake;$($versions['android.cmake'])"
)

Write-Host "Installing pinned Android SDK packages into $SdkRoot"
& $sdkManager --sdk_root=$SdkRoot --channel=0 @packages
if ($LASTEXITCODE -ne 0) {
    throw "sdkmanager failed with exit code $LASTEXITCODE"
}

$ndkVersion = $versions['android.ndk']
$ndkInstallPath = Join-Path $SdkRoot "ndk\$ndkVersion"
$ndkSourceProperties = Join-Path $ndkInstallPath 'source.properties'
if (!(Test-Path -LiteralPath $ndkSourceProperties)) {
    if (Test-Path -LiteralPath $ndkInstallPath) {
        $incompletePath = "$ndkInstallPath.incomplete-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))-$([guid]::NewGuid().ToString('N'))"
        Move-Item -LiteralPath $ndkInstallPath -Destination $incompletePath
        Write-Warning "Moved an incomplete NDK installation to $incompletePath"
    }

    $ndkUrl = $versions['android.ndk.windows.url']
    $ndkArchiveName = Split-Path ([Uri]$ndkUrl).AbsolutePath -Leaf
    $ndkArchivePath = Join-Path $downloadRoot $ndkArchiveName
    Get-VerifiedArchive -Name "Android NDK $ndkVersion" -Url $ndkUrl -Path $ndkArchivePath `
        -ExpectedSize ([long]$versions['android.ndk.windows.size']) `
        -ExpectedSha1 $versions['android.ndk.windows.sha1']

    $extractRoot = Join-Path ([IO.Path]::GetTempPath()) ("openxray-android-ndk-" + [guid]::NewGuid().ToString('N'))
    try {
        Expand-Archive -LiteralPath $ndkArchivePath -DestinationPath $extractRoot
        $extractedNdk = Join-Path $extractRoot $versions['android.ndk.windows.archiveRoot']
        if (!(Test-Path -LiteralPath (Join-Path $extractedNdk 'source.properties'))) {
            throw 'The Android NDK archive has an unexpected layout.'
        }

        New-Item -ItemType Directory -Force -Path (Split-Path $ndkInstallPath -Parent) | Out-Null
        Move-Item -LiteralPath $extractedNdk -Destination $ndkInstallPath
    }
    finally {
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force
        }
    }
}

& (Join-Path $PSScriptRoot 'verify-toolchain.ps1') -SdkRoot $SdkRoot
