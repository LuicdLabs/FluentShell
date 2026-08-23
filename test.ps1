# AnyFluent test and production-layout gates
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
$renderer = Join-Path $OutputRoot "Renderer\FluentShell.Renderer.exe"
foreach ($required in @($bridge, $injector, $renderer)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required production payload missing: $required"
    }
}

$dependencies = (& $dumpbin /nologo /dependents $bridge | Out-String)
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $bridge" }
foreach ($forbidden in @("Microsoft.UI", "Microsoft.WindowsAppRuntime", "CoreMessagingXP")) {
    if ($dependencies -match [regex]::Escape($forbidden)) {
        throw "Bridge has forbidden in-process dependency: $forbidden"
    }
}
Write-Host "Bridge dependency gate passed."

$exports = (& $dumpbin /nologo /exports $bridge | Out-String)
if ($LASTEXITCODE -ne 0 -or $exports -notmatch "FluentShell_Start") {
    throw "Bridge restart export FluentShell_Start is missing"
}
if ($exports -notmatch "FluentShell_IsRendererReady") {
    throw "Bridge readiness export FluentShell_IsRendererReady is missing"
}
Write-Host "Bridge export gate passed."

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
if ($solutionText -match "FluentShell\.Bootstrap" -or $injectorProjectText -match "FluentShell\.Bootstrap") {
    throw "Bootstrap remains in the production solution/dependency graph"
}

$usage = (& $injector 2>&1 | Out-String)
if ($usage -match "inject <pid\|exe>" -or
    $usage -match "system \[" -or
    $usage -match "l0 \[--watch\]" -or
    $usage -match "island-(demo|smoke)") {
    throw "Injector still advertises a removed broad/name-only entry point"
}
Write-Host "Injector CLI cutover gate passed."
Write-Host "All available tests and production gates passed."
