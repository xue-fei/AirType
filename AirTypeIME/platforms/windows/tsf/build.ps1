# AirType Windows TSF 外壳构建脚本。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1                 # Debug
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#   powershell -ExecutionPolicy Bypass -File build.ps1 -OutDir D:\side
#
# 产物：<OutDir 或 build\bin>\airtype_tsf_probe.exe + airtype_tsf.dll + airtype_ffi.dll
#
# 说明：这里手工拼装 INCLUDE/LIB/PATH，不调用 vcvars64.bat——
# vcvars 内部会用 reg.exe 查注册表，在受限环境下会失败。

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",

    [switch]$Clean,

    # 只重建探针 exe。目标进程加载着 airtype_tsf.dll / airtype_ffi.dll 时，
    # 链接器写不进去，用这一个选项先更新探针（例如新增 --deactivate）。
    [switch]$ProbeOnly,

    # 产物输出目录，默认 build\bin。正在服役的 DLL 被目标进程锁住、
    # 又不想重启那些进程时，用它把新产物编译到别处验证。
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
if ($OutDir -eq "") {
    $BinDir = Join-Path $Root "build\bin"
} else {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $BinDir = (Resolve-Path -LiteralPath $OutDir).Path
}
# 独立 exe（探针）和 DLL 都要从同一份构建里出，避免两套 CMake 缓存互相覆盖。
$BuildDir = Join-Path $Root "build"

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
    "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DAIRTYPE_TSF_OUT_DIR=$BinDir"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

if ($ProbeOnly) {
    & $CMake --build $BuildDir --target airtype_tsf_probe
    if ($LASTEXITCODE -ne 0) { throw "构建失败" }
    Write-Host "探针已更新：$BinDir\airtype_tsf_probe.exe"
    return
}

& $CMake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

# 外壳是 in-proc DLL，运行期按「自身所在目录」加载 airtype_ffi.dll
# （见 src/ModuleUtil.cpp），所以构建后必须把 Rust 侧的产物同步过来。
# 少了这一步会表现为激活成功但日志报「airtype_ffi.dll 加载失败: 126」。
$ImeRoot = (Resolve-Path (Join-Path $Root "..\..\..")).Path
$CargoProfile = if ($Config -eq "Debug") { "debug" } else { "release" }
$Ffi = Join-Path $ImeRoot "target\$CargoProfile\airtype_ffi.dll"
if (Test-Path $Ffi) {
    try {
        Copy-Item $Ffi $BinDir -Force -ErrorAction Stop
        Write-Host "已同步 airtype_ffi.dll（$CargoProfile）"
    } catch {
        # 目标进程还加载着旧版本时文件被占用，属预期情况，不该让构建失败。
        Write-Host "警告：airtype_ffi.dll 被占用，未能更新（有进程仍加载着文本服务）。"
        Write-Host "      先跑 airtype_tsf_probe.exe --deactivate 再重新构建。"
    }
} else {
    Write-Host "警告：未找到 $Ffi，请先构建 Rust 侧；外壳会因缺少核心而无法启动。"
}

Write-Host ""
Write-Host "构建完成：$BinDir"
Get-ChildItem $BinDir | ForEach-Object { Write-Host "  $($_.Name)" }
