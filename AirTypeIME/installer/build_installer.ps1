<#
.SYNOPSIS
    构建 AirType TSF 并打包成免手动注册的安装包（zip）。

.DESCRIPTION
    1. 调用 tsf/build.ps1 以 Release 构建，产物直接输出到打包暂存目录
    2. 把 DLL / 探针 / 安装脚本 / 配置模板收进包
    3. 压缩为 dist\AirType-Setup-<版本>-x64.zip

    生成的 zip 解压后，管理员运行 install.ps1 即完成注册；
    日后重建 DLL 重跑同一脚本即可更新，无需再 regsvr32。

.PARAMETER SkipBuild
    跳过构建，直接用现有产物打包（调试用）。

.PARAMETER SourceDir
    SkipBuild 时指定产物目录。默认 tsf\build\bin；若该目录 DLL 被已注入进程
    锁住，可指向先前的 .stage 等旁路目录。

.PARAMETER Version
    安装包版本号，仅用于文件名，默认取当天日期 yyyyMMdd。
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$SourceDir = "",
    [string]$Version = (Get-Date -Format 'yyyyMMdd')
)

$ErrorActionPreference = 'Stop'
$InstallerRoot = $PSScriptRoot
$ImeRoot       = Split-Path -Parent $InstallerRoot
$TsfRoot       = Join-Path $ImeRoot 'platforms\windows\tsf'
$Stage         = Join-Path $InstallerRoot '.stage'
$Dist          = Join-Path $InstallerRoot 'dist'
$ZipPath       = Join-Path $Dist "AirType-Setup-$Version-x64.zip"

function Log([string]$s, [string]$c = 'Gray') { Write-Host "[pack] $s" -ForegroundColor $c }
function Fail([string]$s) { Write-Host "[pack] 错误: $s" -ForegroundColor Red; exit 1 }

# ---------- 1. 构建 ----------
if (-not $SkipBuild) {
    $buildScript = Join-Path $TsfRoot 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildScript)) { Fail "找不到构建脚本：$buildScript" }
    Log "构建 Release，产物输出到暂存目录..." 'Cyan'
    if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
    # build.ps1 内部的 CMake 目标仅限 build\bin 与 OutDir，Stage 作为 OutDir 传入
    & powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript -Config Release -OutDir $Stage
    if ($LASTEXITCODE -ne 0) { Fail "构建失败（exit=$LASTEXITCODE）" }
} else {
    if ($SourceDir -eq '') { $SourceDir = Join-Path $TsfRoot 'build\bin' }
    if (-not (Test-Path -LiteralPath $SourceDir)) { Fail "产物目录不存在：$SourceDir" }
    $Stage = (Resolve-Path -LiteralPath $SourceDir).Path
    Log "跳过构建，使用产物目录：$Stage" 'Yellow'
}

# ---------- 2. 核对必需产物 ----------
$required = @('airtype_tsf.dll', 'airtype_ffi.dll', 'airtype_tsf_probe.exe')
foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $Stage $f))) {
        Fail "暂存目录缺少 $f（位于 $Stage）"
    }
}
Log '必需产物齐全。' 'Green'

# ---------- 3. 组装包内容 ----------
$pkg = Join-Path $InstallerRoot '.pkg'
if (Test-Path -LiteralPath $pkg) { Remove-Item -LiteralPath $pkg -Recurse -Force }
New-Item -ItemType Directory -Force -Path $pkg | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'config') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'tools') | Out-Null

Copy-Item -LiteralPath (Join-Path $Stage 'airtype_tsf.dll')   -Destination $pkg -Force
Copy-Item -LiteralPath (Join-Path $Stage 'airtype_ffi.dll')   -Destination $pkg -Force
Copy-Item -LiteralPath (Join-Path $Stage 'airtype_tsf_probe.exe') -Destination $pkg -Force

Copy-Item -LiteralPath (Join-Path $InstallerRoot 'install.ps1') -Destination $pkg -Force
Copy-Item -LiteralPath (Join-Path $InstallerRoot 'config\airtype.toml') -Destination (Join-Path $pkg 'config\airtype.toml') -Force

# 模拟验证工具：装完后无需硬件、无需 Python 即可推音频验证上屏与悬浮图标。
Copy-Item -LiteralPath (Join-Path $InstallerRoot 'tools\simulate_voice.ps1') -Destination (Join-Path $pkg 'tools\simulate_voice.ps1') -Force
Copy-Item -LiteralPath (Join-Path $InstallerRoot 'tools\close_status.ps1') -Destination (Join-Path $pkg 'tools\close_status.ps1') -Force
if (Test-Path -LiteralPath (Join-Path $InstallerRoot 'VERIFY.md')) {
    Copy-Item -LiteralPath (Join-Path $InstallerRoot 'VERIFY.md') -Destination $pkg -Force
}

# 一键安装入口：双击即用管理员权限跑 install.ps1
$bat = @'
@echo off
chcp 65001 >nul
title AirType 安装程序
powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install.ps1" %*
echo.
pause
'@
Set-Content -LiteralPath (Join-Path $pkg 'INSTALL.bat') -Value $bat -Encoding ASCII

$unbat = @'
@echo off
chcp 65001 >nul
title AirType 卸载程序
powershell -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0install.ps1" -Uninstall %*
echo.
pause
'@
Set-Content -LiteralPath (Join-Path $pkg 'UNINSTALL.bat') -Value $unbat -Encoding ASCII

# ---------- 4. 压缩 ----------
# 用 .NET ZipFile 而非 Compress-Archive：后者对刚写入的大 DLL 的瞬时占用
# （Defender 实时扫描）很敏感，会直接 IOException 失败。这里带重试。
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
Log "压缩到 $ZipPath ..." 'Cyan'
$done = $false
for ($i = 1; $i -le 5 -and -not $done; $i++) {
    try {
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $pkg, $ZipPath,
            [System.IO.Compression.CompressionLevel]::Optimal, $false)
        $done = $true
    } catch {
        if ($i -eq 5) { Fail "压缩失败（重试 5 次）：$($_.Exception.Message)" }
        Log "压缩被占用，重试 $i/5 ..." 'DarkYellow'
        Start-Sleep -Milliseconds 1200
    }
}

# ---------- 5. 清理暂存 ----------
Remove-Item -LiteralPath $pkg -Recurse -Force
if (-not $SkipBuild) { Remove-Item -LiteralPath $Stage -Recurse -Force -ErrorAction SilentlyContinue }

$size = [math]::Round((Get-Item -LiteralPath $ZipPath).Length / 1MB, 1)
Log "完成：$ZipPath（$size MB）" 'Green'
Log '包内容：airtype_tsf.dll / airtype_ffi.dll / airtype_tsf_probe.exe / install.ps1 / INSTALL.bat / UNINSTALL.bat / config\airtype.toml / tools\simulate_voice.ps1 / tools\close_status.ps1 / VERIFY.md' 'Gray'
