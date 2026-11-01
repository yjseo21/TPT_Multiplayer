param(
    [ValidateSet('server', 'client')][string]$Mode = 'client',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ProgramArgs
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$linuxRepo = & wsl -d Ubuntu --exec wslpath -a $repo.Replace('\', '/')
if ($LASTEXITCODE -ne 0 -or -not $linuxRepo) { throw 'Could not access Ubuntu.' }
$linuxRepo = $linuxRepo.Trim()
$executable = if ($Mode -eq 'server') { 'tpt-server' } else { 'tpt-remote' }
if (-not (Test-Path -LiteralPath (Join-Path $repo "build-mp\src\multiplayer\$executable"))) {
    throw 'Build first with tools\multiplayer\build-wsl.ps1.'
}
# WSLENV forwards credentials as environment variables, not command-line arguments.
$savedWslEnv = $env:WSLENV
try {
    $entries = @($savedWslEnv -split ':' | Where-Object { $_ })
    foreach ($name in @('TPT_ROOM_TOKEN', 'TPT_ADMIN_TOKEN')) {
        if ($entries -notcontains $name) { $entries += $name }
    }
    $env:WSLENV = $entries -join ':'
    & wsl -d Ubuntu --cd $linuxRepo --exec "./build-mp/src/multiplayer/$executable" @ProgramArgs
    if ($LASTEXITCODE -ne 0) { throw "$executable exited with code $LASTEXITCODE" }
}
finally { $env:WSLENV = $savedWslEnv }
