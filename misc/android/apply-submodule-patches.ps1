[CmdletBinding()]
param(
    [string] $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..'))
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$patches = @(
    @{ Submodule = 'Externals\GameSpy'; Patch = 'patches\gamespy-android-threading.patch' },
    @{ Submodule = 'Externals\LuaJIT'; Patch = 'patches\luajit-msvc-cross-arm.patch' },
    @{ Submodule = 'Externals\luabind'; Patch = 'patches\luabind-android-lua-target.patch' },
    @{ Submodule = 'Externals\xrLuaFix'; Patch = 'patches\xrluafix-source-root.patch' }
)

foreach ($entry in $patches) {
    $submodule = Join-Path $RepositoryRoot $entry.Submodule
    $patch = Join-Path $PSScriptRoot $entry.Patch
    if (!(Test-Path -LiteralPath (Join-Path $submodule '.git'))) {
        throw "Submodule is not initialized: $($entry.Submodule). Run git submodule update --init --recursive first."
    }

    & git -C $submodule apply --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) {
        & git -C $submodule apply $patch
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply Android patch to $($entry.Submodule)."
        }
        Write-Host "Applied Android compatibility patch: $($entry.Submodule)"
        continue
    }

    & git -C $submodule apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Android patch does not apply cleanly to $($entry.Submodule)."
    }
    Write-Host "Android compatibility patch already applied: $($entry.Submodule)"
}
