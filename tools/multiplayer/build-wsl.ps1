param([switch]$Test)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workspace = Split-Path $repo -Parent
$python = Join-Path $workspace '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    throw 'Create the project-local .venv and install meson first (see README.multiplayer.md).'
}
$linuxRepo = & wsl -d Ubuntu --exec wslpath -a $repo.Replace('\', '/')
if ($LASTEXITCODE -ne 0 -or -not $linuxRepo) { throw 'Could not resolve repository path in Ubuntu.' }
$pythonModules = (Join-Path $workspace '.venv\Lib\site-packages').Replace('\', '/')
$linuxPythonPath = & wsl -d Ubuntu --exec wslpath -a $pythonModules
if ($LASTEXITCODE -ne 0 -or -not $linuxPythonPath) { throw 'Could not resolve Python path in Ubuntu.' }
$linuxRepo = $linuxRepo.Trim()
$linuxPythonPath = $linuxPythonPath.Trim()
# Pass paths as arguments, never interpolate them into shell source.
$script = 'cd "$1" && export PYTHONPATH="$2" && python3 tools/multiplayer/build.py --build-dir build-mp --jobs 6'
& wsl -d Ubuntu --exec bash -c $script build-wsl $linuxRepo $linuxPythonPath
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Test) {
    & wsl -d Ubuntu --exec env "PYTHONPATH=$linuxPythonPath" python3 -m mesonbuild.mesonmain test -C "$linuxRepo/build-mp" --print-errorlogs
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
