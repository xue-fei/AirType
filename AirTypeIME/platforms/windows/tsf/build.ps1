# AirType Windows TSF 外壳构建脚本。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1                 # Debug
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#
# 产物：build/bin/airtype_tsf_probe.exe（后续还会产出 airtype_tsf.dll）
#
# 说明：这里手工拼装 INCLUDE/LIB/PATH，不调用 vcvars64.bat——
# vcvars 内部会用 reg.exe 查注册表，在受限环境下会失败。

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",

    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build"
$BinDir = Join-Path $BuildDir "bin"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "清理 $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# --- 定位 Visual Studio ---
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$InstallPath = $null
if (Test-Path $VsWhere) {
    $InstallPath = & $VsWhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
}
if (-not $InstallPath) {
    $InstallPath = "C:\Program Files\Microsoft Visual Studio\2022\Community"
}
if (-not (Test-Path $InstallPath)) {
    throw "找不到 Visual Studio 2022 安装目录。"
}

# --- 定位 MSVC 工具集（取最新版本号目录） ---
$MsvcRoot = Join-Path $InstallPath "VC\Tools\MSVC"
if (-not (Test-Path $MsvcRoot)) {
    throw "找不到 MSVC 工具集：$MsvcRoot"
}
$Msvc = Join-Path $MsvcRoot (Get-ChildItem $MsvcRoot -Directory |
        Sort-Object Name -Descending |
        Select-Object -First 1).Name

# --- 定位 Windows SDK（取最新版本号目录） ---
$SdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$SdkVer = $null
foreach ($kind in @("Include", "Lib")) {
    $p = Join-Path $SdkRoot $kind
    if (-not (Test-Path $p)) { continue }
    $v = Get-ChildItem $p -Directory | Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $SdkVer) { $SdkVer = $v.Name }
}
if (-not $SdkVer) {
    throw "找不到 Windows SDK。"
}

$Cl = Join-Path $Msvc "bin\Hostx64\x64\cl.exe"
if (-not (Test-Path $Cl)) {
    throw "找不到 cl.exe：$Cl"
}

$CMake = Join-Path $InstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Ninja = Join-Path $InstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $CMake)) { $CMake = "C:\Program Files\CMake\bin\cmake.exe" }
if (-not (Test-Path $Ninja)) { throw "找不到 ninja.exe（VS 自带）。" }

# --- 拼装环境 ---
$env:INCLUDE = (@(
        (Join-Path $Msvc "include"),
        (Join-Path $Msvc "atlmfc\include"),
        (Join-Path $SdkRoot "Include\$SdkVer\ucrt"),
        (Join-Path $SdkRoot "Include\$SdkVer\shared"),
        (Join-Path $SdkRoot "Include\$SdkVer\um"),
        (Join-Path $SdkRoot "Include\$SdkVer\winrt")
    ) -join ';')

$env:LIB = (@(
        (Join-Path $Msvc "lib\x64"),
        (Join-Path $Msvc "atlmfc\lib\x64"),
        (Join-Path $SdkRoot "Lib\$SdkVer\ucrt\x64"),
        (Join-Path $SdkRoot "Lib\$SdkVer\um\x64")
    ) -join ';')

$MsvcBin = Join-Path $Msvc "bin\Hostx64\x64"
$SdkBin = Join-Path $SdkRoot ("bin\" + $SdkVer + "\x64")
$env:PATH = "$MsvcBin;$SdkBin;$env:PATH"

Write-Host "MSVC : $Msvc"
Write-Host "SDK  : $SdkVer"
Write-Host "配置: $Config"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

& $CMake -S $Root -B $BuildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_CXX_COMPILER=$Cl" `
    "-DCMAKE_MAKE_PROGRAM=$Ninja"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

& $CMake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Write-Host ""
Write-Host "构建完成：$BinDir"
Get-ChildItem $BinDir | ForEach-Object { Write-Host "  $($_.Name)" }
