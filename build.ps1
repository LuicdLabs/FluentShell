# AnyFluent production build
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$RestoreOnly,
    [switch]$CoreOnly
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$RendererProject = Join-Path $Root "src\Renderer\FluentShell.Renderer.csproj"
$OutputRoot = Join-Path $Root "build\bin\$Platform\$Configuration"
$RendererOutput = Join-Path $OutputRoot "Renderer"
Set-Location $Root

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($install) {
            $candidate = Join-Path $install "MSBuild\Current\Bin\amd64\MSBuild.exe"
            if (Test-Path $candidate) { return $candidate }
            $candidate = Join-Path $install "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "MSBuild not found"
}

function Find-NuGet {
    $local = Join-Path $Root "tools\nuget.exe"
    if (Test-Path $local) { return $local }
    New-Item -ItemType Directory -Path (Join-Path $Root "tools") -Force | Out-Null
    Write-Host "Downloading nuget.exe..."
    Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $local -UseBasicParsing
    return $local
}

function Reset-ProductionOutput {
    $resolvedRoot = [IO.Path]::GetFullPath((Join-Path $Root "build\bin"))
    $resolvedOutput = [IO.Path]::GetFullPath($OutputRoot)
    if (-not $resolvedOutput.StartsWith($resolvedRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean output outside build/bin: $resolvedOutput"
    }
    if (Test-Path -LiteralPath $resolvedOutput) {
        # Keep the verified configuration root itself so a developer terminal
        # whose current directory is the production output cannot block an
        # otherwise clean rebuild.  Removing every child preserves the clean
        # payload guarantee without requiring the directory handle to close.
        Get-ChildItem -LiteralPath $resolvedOutput -Force |
            Remove-Item -Recurse -Force
    } else {
        New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
    }
}

$msbuild = Find-MSBuild
$nuget = Find-NuGet
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw ".NET SDK not found"
}
if (-not (Test-Path -LiteralPath $RendererProject)) {
    throw "Renderer project missing: $RendererProject"
}

Write-Host "Restoring native packages..."
& $nuget restore (Join-Path $Root "src\packages.config") `
    -PackagesDirectory (Join-Path $Root "packages") -NonInteractive
if ($LASTEXITCODE -ne 0) { throw "nuget restore failed" }

# IslandDemo remains a manual diagnostic and still needs the runtime headers when built directly.
& $nuget install Microsoft.WindowsAppSDK.Runtime -Version 2.3.1 `
    -OutputDirectory (Join-Path $Root "packages") -NonInteractive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Windows App SDK runtime restore failed" }

Write-Host "Restoring renderer..."
& dotnet restore $RendererProject -r win-x64
if ($LASTEXITCODE -ne 0) { throw "renderer restore failed" }

if ($RestoreOnly) {
    Write-Host "Restore complete."
    exit 0
}

Reset-ProductionOutput

$targets = if ($CoreOnly) {
    @(
        "third_party\detours\detours.vcxproj",
        "src\FluentShell.Core.vcxproj",
        "src\FluentShell.Bridge.vcxproj",
        "src\FluentShell.Injector.vcxproj",
        "src\PoC\LegacyDialogHost\LegacyDialogHost.vcxproj"
    )
} else {
    @("FluentShell.sln")
}

foreach ($target in $targets) {
    Write-Host "Building $target ($Configuration|$Platform)..."
    & $msbuild $target /m /p:Configuration=$Configuration /p:Platform=$Platform /v:m
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
}

Write-Host "Publishing out-of-process renderer..."
& dotnet publish $RendererProject `
    --configuration $Configuration `
    --runtime win-x64 `
    --self-contained true `
    --no-restore `
    --output $RendererOutput `
    -p:PublishSingleFile=false `
    -p:PublishTrimmed=false `
    -p:WindowsAppSDKSelfContained=true
if ($LASTEXITCODE -ne 0) { throw "renderer publish failed" }

$required = @(
    (Join-Path $OutputRoot "FluentShell.Injector.exe"),
    (Join-Path $OutputRoot "FluentShell.Bridge.dll"),
    (Join-Path $OutputRoot "LegacyDialogHost.exe"),
    (Join-Path $RendererOutput "FluentShell.Renderer.exe")
)
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Required production output missing: $file"
    }
}

$forbiddenRootPayload = @(
    "FluentShell.Bootstrap.dll",
    "FluentShell.Renderer.exe",
    "CoreMessagingXP.dll",
    "Microsoft.UI.dll",
    "Microsoft.UI.Xaml.dll",
    "Microsoft.WindowsAppRuntime.dll"
)
foreach ($name in $forbiddenRootPayload) {
    if (Test-Path -LiteralPath (Join-Path $OutputRoot $name)) {
        throw "Renderer/legacy payload escaped its staging directory: $name"
    }
}

Write-Host ""
Write-Host "Build OK. Production outputs:"
Get-ChildItem -LiteralPath $OutputRoot | Format-Table Name, Length, LastWriteTime
Write-Host ""
Write-Host "Run the translation oracle:"
Write-Host "  `$target = (Resolve-Path '$OutputRoot\LegacyDialogHost.exe').Path"
Write-Host "  Start-Process `$target"
Write-Host "  $OutputRoot\FluentShell.Injector.exe inject `$target"
Write-Host ""
Write-Host "IslandDemo is diagnostic-only and must be built explicitly from its project."
