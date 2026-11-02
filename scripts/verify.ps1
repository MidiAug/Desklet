param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (-not $Executable) {
    $Executable = if (Test-Path -LiteralPath (Join-Path $taskRoot 'build/Desklet.0.5.3.exe')) { 'build/Desklet.0.5.3.exe' } else { 'build/Desklet.exe' }
}
$taskExe = Join-Path $taskRoot $Executable
if (-not (Test-Path -LiteralPath $taskExe)) { throw 'Build first with build.ps1.' }
foreach ($taskMode in @('--check', '--render-test', '--resident-check', '--compat-check')) {
    $taskProcess = Start-Process -FilePath $taskExe -ArgumentList $taskMode -WindowStyle Hidden -PassThru -Wait
    if ($taskProcess.ExitCode -ne 0) { throw "$taskMode failed: $($taskProcess.ExitCode)" }
    Write-Output "$taskMode passed"
}
