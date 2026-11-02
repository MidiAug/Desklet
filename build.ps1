param([string]$OutputName = 'Desklet.0.5.3.exe')
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskVswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$taskVsRoot = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVsRoot) { throw 'Visual C++ build tools were not found.' }
$taskVcvars = Join-Path $taskVsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$taskBuild = Join-Path $taskRoot 'build'
New-Item -ItemType Directory -Force -Path $taskBuild | Out-Null
$taskCompile = @"
@echo off
call "$taskVcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$taskBuild"
rc /nologo /i "$taskRoot\assets" /fo resources.res "$taskRoot\assets\resources.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN "$taskRoot\src\shell_ui.cpp" /Fe:$OutputName /link resources.res /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /MANIFEST:NO user32.lib gdi32.lib gdiplus.lib dwmapi.lib shell32.lib ole32.lib propsys.lib shlwapi.lib psapi.lib uuid.lib advapi32.lib runtimeobject.lib comctl32.lib version.lib windowscodecs.lib
"@
$taskBuildFile = Join-Path $taskBuild 'compile.cmd'
[System.IO.File]::WriteAllText($taskBuildFile, $taskCompile, [System.Text.Encoding]::Default)
& $env:ComSpec /d /c "`"$taskBuildFile`""
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
