# Build standalone Windows executables (no Python needed on the target).
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_executable.ps1
#   $env:PM_ONEFILE = "1"; powershell ... build_executable.ps1   # single-file
#
# Output: dist\ProcessMonitor\ProcessMonitor.exe, dist\MockPublisher\MockPublisher.exe
# and dist\ProcessMonitor-windows-x64.zip with both folders.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$venv = ".build-venv"
if (-not (Test-Path "$venv\Scripts\python.exe")) {
    python -m venv $venv
}
$py = "$venv\Scripts\python.exe"

& $py -m pip install --upgrade pip | Out-Null
if ($env:WHEELS) {
    & $py -m pip install --no-index --find-links $env:WHEELS -r requirements.txt pyinstaller
} else {
    & $py -m pip install -r requirements.txt pyinstaller
}

if (Test-Path build) { Remove-Item -Recurse -Force build }
if (Test-Path dist) { Remove-Item -Recurse -Force dist }
& $py -m PyInstaller --clean --noconfirm ProcessMonitor.spec
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed with exit code $LASTEXITCODE" }

$zip = "dist\ProcessMonitor-windows-x64.zip"
# PM_ONEFILE builds single .exe files instead of folders.
$onefile = $env:PM_ONEFILE -and $env:PM_ONEFILE -notin @("0", "false", "no")
if ($onefile) {
    Compress-Archive -Path dist\ProcessMonitor.exe, dist\MockPublisher.exe -DestinationPath $zip -Force
    $run = "dist\ProcessMonitor.exe"
} else {
    Compress-Archive -Path dist\ProcessMonitor, dist\MockPublisher -DestinationPath $zip -Force
    $run = "dist\ProcessMonitor\ProcessMonitor.exe"
}
Write-Host ""
Write-Host "Archive: $zip"
Write-Host "Run:     $run --sub tcp://HOST:6667 --dealer tcp://HOST:5557"
