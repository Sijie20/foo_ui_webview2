param([switch]$Composition)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath -prerelease
if (-not (Test-Path "$vs\VC\Auxiliary\Build\vcvars64.bat")) { $vs = "$env:USERPROFILE\tools\vs_buildtools" }
$native = Join-Path $root 'packages\Microsoft.Web.WebView2.1.0.3719.77\build\native'
$mode = if ($Composition) { 'composition' } else { '' }
New-Item -ItemType Directory -Force "$root\obj\bounds-unit" | Out-Null
$bat = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++17 /EHsc /utf-8 /I "$native\include" "$root\tests\manual\webview-bounds-unit.cpp" /Fo"$root\obj\bounds-unit\probe.obj" /Fe"$root\obj\bounds-unit\probe.exe" /link "$native\x64\WebView2LoaderStatic.lib" user32.lib ole32.lib shcore.lib version.lib advapi32.lib dcomp.lib
if errorlevel 1 exit /b %errorlevel%
"$root\obj\bounds-unit\probe.exe" "$root\obj\bounds-unit\profile" $mode
"@
$bat | Set-Content -LiteralPath "$root\obj\bounds-unit\run.cmd" -Encoding ascii
& $env:ComSpec /d /c "$root\obj\bounds-unit\run.cmd"
exit $LASTEXITCODE
