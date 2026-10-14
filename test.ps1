# FluentShell test and production-layout gates
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("x64")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutputRoot = Join-Path $Root "build\bin\$Platform\$Configuration"
Set-Location $Root

function Find-VSInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
    $install = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $install) { throw "Visual Studio/MSBuild installation not found" }
    return $install
}

function Find-MSBuild([string]$Install) {
    foreach ($relative in @("MSBuild\Current\Bin\amd64\MSBuild.exe", "MSBuild\Current\Bin\MSBuild.exe")) {
        $candidate = Join-Path $Install $relative
        if (Test-Path $candidate) { return $candidate }
    }
    throw "MSBuild not found"
}

function Find-Dumpbin([string]$Install) {
    $tools = Join-Path $Install "VC\Tools\MSVC"
    $candidate = Get-ChildItem -LiteralPath $tools -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $candidate) { throw "dumpbin.exe not found" }
    return $candidate
}

function Get-MSBuildProperty([string]$MSBuild, [string]$Project, [string]$Name) {
    $output = & $MSBuild $Project /p:Configuration=$Configuration /p:Platform=$Platform "/getProperty:$Name" /v:q
    if ($LASTEXITCODE -ne 0) { throw "Could not query $Name for $Project" }
    return ($output | Where-Object { $_ -and $_ -notmatch '^MSBuild version' } | Select-Object -Last 1).Trim()
}

if (-not (Test-Path -LiteralPath $OutputRoot -PathType Container)) {
    throw "Build output missing. Run .\build.ps1 -Configuration $Configuration first."
}

$vsInstall = Find-VSInstall
$msbuild = Find-MSBuild $vsInstall
$dumpbin = Find-Dumpbin $vsInstall

$csharpTests = @(Get-ChildItem -Path (Join-Path $Root "tests") -Recurse -Filter "*.csproj" -ErrorAction SilentlyContinue)
foreach ($project in $csharpTests) {
    Write-Host "dotnet test $($project.FullName)"
    & dotnet test $project.FullName --configuration $Configuration `
        -p:FluentShellTestBuild=true
    if ($LASTEXITCODE -ne 0) { throw "C# tests failed: $($project.FullName)" }
}
if ($csharpTests.Count -eq 0) { Write-Host "No C# test project is currently present." }

$nativeTests = @(Get-ChildItem -Path (Join-Path $Root "tests") -Recurse -Filter "*.vcxproj" -ErrorAction SilentlyContinue)
foreach ($project in $nativeTests) {
    Write-Host "Building native test $($project.FullName)"
    & $msbuild $project.FullName /m /p:Configuration=$Configuration /p:Platform=$Platform /v:m
    if ($LASTEXITCODE -ne 0) { throw "Native test build failed: $($project.FullName)" }
    if ((Get-MSBuildProperty $msbuild $project.FullName "ConfigurationType") -ne "Application") {
        Write-Host "Skipping non-executable native test project: $($project.Name)"
        continue
    }
    $targetPath = Get-MSBuildProperty $msbuild $project.FullName "TargetPath"
    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        throw "Native test target was not produced: $targetPath"
    }
    Write-Host "Running $targetPath"
    & $targetPath
    if ($LASTEXITCODE -ne 0) { throw "Native tests failed: $targetPath" }
}
if ($nativeTests.Count -eq 0) { Write-Host "No native test project is currently present." }

$bridge = Join-Path $OutputRoot "FluentShell.Bridge.dll"
$injector = Join-Path $OutputRoot "FluentShell.Injector.exe"
$win32Bridge = Join-Path $OutputRoot "x86\FluentShell.Bridge.dll"
$win32Injector = Join-Path $OutputRoot "x86\FluentShell.Injector.exe"
$renderer = Join-Path $OutputRoot "Renderer\FluentShell.Renderer.exe"
foreach ($required in @($bridge, $injector, $win32Bridge, $win32Injector, $renderer)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required production payload missing: $required"
    }
}

foreach ($dll in @($bridge, $win32Bridge)) {
    $dependencies = (& $dumpbin /nologo /dependents $dll | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $dll" }
    foreach ($forbidden in @("Microsoft.UI", "Microsoft.WindowsAppRuntime", "CoreMessagingXP")) {
        if ($dependencies -match [regex]::Escape($forbidden)) {
            throw "$dll has forbidden in-process dependency: $forbidden"
        }
    }
    # Exact export names: the injector resolves them with GetProcAddress, and a 32-bit
    # DLL without the .def file exports _FluentShell_Start@4, which a substring match accepts.
    $exports = (& $dumpbin /nologo /exports $dll | Out-String)
    if ($LASTEXITCODE -ne 0 -or $exports -notmatch "\s[0-9A-F]{8}\s+FluentShell_Start\s+=") {
        throw "$dll restart export FluentShell_Start is missing or decorated"
    }
    if ($exports -notmatch "\s[0-9A-F]{8}\s+FluentShell_IsRendererReady\s+=") {
        throw "$dll readiness export FluentShell_IsRendererReady is missing or decorated"
    }
}
Write-Host "Bridge dependency and export gates passed (x64 and x86)."

foreach ($forbidden in @(
    "FluentShell.Bootstrap.dll",
    "FluentShell.Renderer.exe",
    "CoreMessagingXP.dll",
    "Microsoft.UI.dll",
    "Microsoft.UI.Xaml.dll",
    "Microsoft.WindowsAppRuntime.dll"
)) {
    if (Test-Path -LiteralPath (Join-Path $OutputRoot $forbidden)) {
        throw "Forbidden payload at production root: $forbidden"
    }
}

$solutionText = Get-Content -Raw (Join-Path $Root "FluentShell.sln")
$injectorProjectText = Get-Content -Raw (Join-Path $Root "src\FluentShell.Injector.vcxproj")
$bridgeProjectText = Get-Content -Raw (Join-Path $Root "src\FluentShell.Bridge.vcxproj")
foreach ($projectText in @($solutionText, $injectorProjectText, $bridgeProjectText)) {
    if ($projectText -match "FluentShell\.(Bootstrap|Core)|Renderer\.Dwm") {
        throw "A removed legacy component remains in the production solution/dependency graph"
    }
}

foreach ($exe in @($injector, $win32Injector)) {
    $usage = (& $exe 2>&1 | Out-String)
    if ($usage -match "inject <pid\|exe>" -or
        $usage -match "system \[" -or
        $usage -match "\bl0\b" -or
        $usage -match "island-(demo|smoke)") {
        throw "$(Split-Path -Leaf $exe) still advertises a removed diagnostic/broad/name-only entry point"
    }
}
Write-Host "Injector CLI cutover gate passed (x64 and x86)."

# Global apply's guardrails.  Each gate names a constraint the design depends on: no elevation, no autostart,
# no authenticated control pipe in this slice, and no global hooks in the watcher.
$guardedSources = @(Get-ChildItem -Path (Join-Path $Root "src") -Recurse -File -Include *.cpp, *.h, *.cs, *.csproj, *.vcxproj)
$guardedInjector = @($guardedSources | Where-Object { $_.FullName -like "*\src\injector\*" })
function Assert-Absent([object[]]$Files, [string]$Pattern, [string]$Reason) {
    foreach ($file in $Files) {
        if (Select-String -LiteralPath $file.FullName -Pattern $Pattern -SimpleMatch -Quiet) {
            throw "$Reason ($($file.FullName))"
        }
    }
}
Assert-Absent $guardedSources "runas" "UAC elevation is forbidden: the 'runas' verb appears"
Assert-Absent $guardedSources "RequireAdministrator" "UAC elevation is forbidden: an elevation manifest appears"
Assert-Absent $guardedSources "UseShellExecute = true" "The GUI must not launch through the shell (elevation path)"
Assert-Absent $guardedSources "CurrentVersion\Run" "Autostart is forbidden: a Run-key entry appears"
Assert-Absent $guardedSources "FluentShell.Control" "The authenticated control pipe is out of scope for this slice"
Assert-Absent $guardedInjector "SetWindowsHookEx" "The watcher must not use global hooks"
Assert-Absent $guardedInjector "SetWinEventHook" "The watcher must not use WinEvent hooks"
Assert-Absent $guardedInjector "AppInit_DLLs" "The watcher must not use AppInit_DLLs"
Assert-Absent $guardedInjector "Image File Execution Options" "The watcher must not use IFEO"
Assert-Absent $guardedInjector "ShellExecute" "The injector must not launch through the shell"
Write-Host "Global apply guardrail gates passed."
Write-Host "All available tests and production gates passed."
# The CLI gate runs the Injector without arguments, which exits 1 by design; every
# failure above has already thrown, so reaching here means success.
exit 0
