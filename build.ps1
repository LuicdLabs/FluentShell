# AnyFluent build script. Loads the VS2026 x64 developer environment and
# compiles the requested target(s) with MSVC. Usage:  .\build.ps1 [all|demo|testtarget|hookdll|injector]
param([string]$Target = "all")

$ErrorActionPreference = "Stop"
$vs   = "C:\Program Files\Microsoft Visual Studio\18\Community"
$root = "C:\Users\User\anyfluent"
$src  = "$root\src"
$out  = "$root\build\bin"

Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "$vs" -SkipAutomaticLocation -DevCmdArguments "-arch=amd64 -host_arch=amd64" | Out-Null
New-Item -ItemType Directory -Force -Path $out | Out-Null
Set-Location $out

$common = "$src\common\FluentCore.cpp"
$cflags = @("/nologo", "/EHsc", "/std:c++17", "/W3", "/O2", "/MD", "/utf-8", "/DUNICODE", "/D_UNICODE", "/I", "$src\common")
$libs   = @("user32.lib","gdi32.lib","dwmapi.lib","d2d1.lib","dwrite.lib","uxtheme.lib",
            "comctl32.lib","gdiplus.lib","shcore.lib","ole32.lib","advapi32.lib")

function Invoke-Native([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe failed (exit $LASTEXITCODE)" }
}

function Build-Demo {
    if (-not (Test-Path "$src\demo\main.cpp")) { return }
    Write-Host "==> demo.exe" -ForegroundColor Cyan
    Invoke-Native "rc" @("/nologo", "/fo", "$out\dialog.res", "$src\demo\dialog.rc")
    $a = $cflags + @("$src\demo\main.cpp", $common, "$out\dialog.res", "/Fe:$out\demo.exe",
                     "/link") + $libs + @("/SUBSYSTEM:WINDOWS")
    Invoke-Native "cl" $a
}

function Build-TestTarget {
    if (-not (Test-Path "$src\testtarget\main.cpp")) { return }
    Write-Host "==> testtarget.exe" -ForegroundColor Cyan
    Invoke-Native "rc" @("/nologo", "/fo", "$out\testtarget.res", "$src\testtarget\testtarget.rc")
    $a = $cflags + @("$src\testtarget\main.cpp", "$out\testtarget.res", "/Fe:$out\testtarget.exe",
                     "/link") + $libs + @("/SUBSYSTEM:WINDOWS")
    Invoke-Native "cl" $a
}

function Build-HookDll {
    if (-not (Test-Path "$src\hookdll\dllmain.cpp")) { return }
    Write-Host "==> anyfluenthook.dll" -ForegroundColor Cyan
    $a = $cflags + @("/LD", "$src\hookdll\dllmain.cpp", $common, "/Fe:$out\anyfluenthook.dll",
                     "/link") + $libs
    Invoke-Native "cl" $a
}

function Build-Injector {
    if (-not (Test-Path "$src\injector\main.cpp")) { return }
    Write-Host "==> injector.exe" -ForegroundColor Cyan
    $a = $cflags + @("$src\injector\main.cpp", "/Fe:$out\injector.exe",
                     "/link") + $libs + @("/SUBSYSTEM:CONSOLE")
    Invoke-Native "cl" $a
}

function Build-Capture {
    if (-not (Test-Path "$src\tools\capture.cpp")) { return }
    Write-Host "==> capture.exe" -ForegroundColor Cyan
    $a = $cflags + @("$src\tools\capture.cpp", "/Fe:$out\capture.exe",
                     "/link") + $libs + @("/SUBSYSTEM:CONSOLE")
    Invoke-Native "cl" $a
}

switch ($Target.ToLower()) {
    "demo"       { Build-Demo }
    "testtarget" { Build-TestTarget }
    "hookdll"    { Build-HookDll }
    "injector"   { Build-Injector }
    "capture"    { Build-Capture }
    default      { Build-Demo; Build-TestTarget; Build-HookDll; Build-Injector; Build-Capture }
}
Write-Host "Build complete -> $out" -ForegroundColor Green
Get-ChildItem $out -Include *.exe,*.dll -Recurse | Select-Object Name, Length | Format-Table -AutoSize
