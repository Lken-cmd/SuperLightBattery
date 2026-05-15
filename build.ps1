param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $Root "out"
$AssetsDir = Join-Path $Root "assets"
$IconPath = Join-Path $AssetsDir "SuperLightBattery.ico"
$IconGenerator = Join-Path $AssetsDir "generate-icon.ps1"
$InstallerIconPath = Join-Path $AssetsDir "SuperLightBatteryInstaller.ico"
$InstallerIconGenerator = Join-Path $AssetsDir "generate-installer-icon.ps1"

$AppSource = Join-Path $Root "src\superlightbattery.c"
$AppRc = Join-Path $Root "src\app.rc"
$AppRes = Join-Path $OutDir "app.res"
$AppExe = Join-Path $OutDir "SuperLightBattery.exe"
$AppObj = Join-Path $OutDir "superlightbattery.obj"

$LauncherSource = Join-Path $Root "src\launcher.c"
$LauncherRc = Join-Path $Root "src\launcher.rc"
$LauncherRes = Join-Path $OutDir "launcher.res"
$LauncherExe = Join-Path $OutDir "SuperLightBatteryLauncher.exe"
$LauncherObj = Join-Path $OutDir "launcher.obj"

$InstallerSource = Join-Path $Root "src\installer.c"
$InstallerRc = Join-Path $Root "src\installer.rc"
$InstallerRes = Join-Path $OutDir "installer.res"
$InstallerExe = Join-Path $OutDir "SuperLightBatteryInstaller.exe"
$InstallerObj = Join-Path $OutDir "installer.obj"

function Find-VsDevCmd {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find "Common7\Tools\VsDevCmd.bat" | Select-Object -First 1
        if ($path) {
            return $path
        }
    }

    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Professional\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise\Common7\Tools\VsDevCmd.bat"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

if ($Clean -and (Test-Path $OutDir)) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $IconPath)) {
    if (-not (Test-Path $IconGenerator)) {
        throw "Missing icon and missing generator: $IconGenerator"
    }
    & $IconGenerator -OutputPath $IconPath
}

if (-not (Test-Path $InstallerIconPath)) {
    if (-not (Test-Path $InstallerIconGenerator)) {
        throw "Missing installer icon and missing generator: $InstallerIconGenerator"
    }
    & $InstallerIconGenerator -OutputPath $InstallerIconPath
}

$vsdevcmd = Find-VsDevCmd
if (-not $vsdevcmd) {
    throw "Could not find Visual Studio C++ build tools. Install MSVC x64 tools or run from a Developer PowerShell."
}

function New-RcArgs {
    param(
        [string]$Source,
        [string]$Output
    )
    return "/nologo /fo `"$Output`" `"$Source`""
}

function New-ClArgs {
    param(
        [string]$Source,
        [string]$Exe,
        [string]$Obj,
        [string]$Res,
        [string[]]$Libs,
        [string[]]$CompileArgs = @(),
        [string[]]$LinkArgs = @(),
        [switch]$NoGl,
        [switch]$NoLtcg
    )

    $args = @(
        "/nologo",
        "/std:c11",
        "/W4",
        "/O1",
        "/Gw",
        "/Gy",
        "/MT",
        "/DUNICODE",
        "/D_UNICODE"
    )

    if (-not $NoGl) {
        $args += "/GL"
    }
    $args += $CompileArgs
    $args += @(
        "/Fo:`"$Obj`"",
        "/Fe:`"$Exe`"",
        "`"$Source`"",
        "/link",
        "/nologo",
        "`"$Res`""
    )

    $args += $LinkArgs
    if (-not $NoLtcg) {
        $args += "/LTCG"
    }
    $args += @("/OPT:REF", "/OPT:ICF", "/INCREMENTAL:NO")
    $args += $Libs
    return ($args -join " ")
}

$appRcArgs = New-RcArgs -Source $AppRc -Output $AppRes
$launcherRcArgs = New-RcArgs -Source $LauncherRc -Output $LauncherRes
$installerRcArgs = New-RcArgs -Source $InstallerRc -Output $InstallerRes

$appArgs = New-ClArgs `
    -Source $AppSource `
    -Exe $AppExe `
    -Obj $AppObj `
    -Res $AppRes `
    -LinkArgs @("/SUBSYSTEM:CONSOLE") `
    -Libs @("setupapi.lib", "hid.lib", "shell32.lib", "user32.lib", "gdi32.lib")

$launcherArgs = New-ClArgs `
    -Source $LauncherSource `
    -Exe $LauncherExe `
    -Obj $LauncherObj `
    -Res $LauncherRes `
    -CompileArgs @("/GS-") `
    -LinkArgs @("/SUBSYSTEM:WINDOWS", "/ENTRY:LauncherEntry", "/NODEFAULTLIB") `
    -NoGl `
    -NoLtcg `
    -Libs @("kernel32.lib", "user32.lib")

$installerArgs = New-ClArgs `
    -Source $InstallerSource `
    -Exe $InstallerExe `
    -Obj $InstallerObj `
    -Res $InstallerRes `
    -LinkArgs @("/SUBSYSTEM:WINDOWS") `
    -Libs @("shell32.lib", "ole32.lib", "uuid.lib", "user32.lib", "advapi32.lib", "gdi32.lib", "comctl32.lib")

$cmd = "`"$vsdevcmd`" -arch=x64 -host_arch=x64 >NUL 2>NUL && rc $appRcArgs && rc $launcherRcArgs && rc $installerRcArgs && cl $appArgs && cl $launcherArgs && cl $installerArgs"
cmd.exe /d /s /c $cmd
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Built out\SuperLightBattery.exe"
Write-Host "Built out\SuperLightBatteryLauncher.exe"
Write-Host "Built out\SuperLightBatteryInstaller.exe"
