<#
.SYNOPSIS
    把 dist 里的 zip 安装包转成自解压 EXE（用系统已装的 7-Zip，双击即自动安装）。

.DESCRIPTION
    1. 把 zip 解压到临时目录（保留 config\ 和 tools\ 子目录）
    2. 用 7z 把它压成 .7z
    3. 生成 SFX 配置（RunProgram=INSTALL.bat）
    4. 按 7z.sfx + 配置 + 7z 数据 的顺序用字节拼接成自解压 exe

    生成的 exe 双击后：解压到临时目录 -> 自动运行 INSTALL.bat（弹 UAC）-> 安装。

.PARAMETER Zip
    源 zip 路径，默认取 dist 里最新的 AirType-Setup-*-x64.zip。
#>
[CmdletBinding()]
param([string]$Zip = '')

$ErrorActionPreference = 'Stop'
$InstallerRoot = $PSScriptRoot
$Dist = Join-Path $InstallerRoot 'dist'

function Log([string]$s, [string]$c = 'Gray') { Write-Host "[exe] $s" -ForegroundColor $c }
function Fail([string]$s) { Write-Host "[exe] 错误: $s" -ForegroundColor Red; exit 1 }

# ---------- 1. 找 7-Zip ----------
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
$sfxMod   = 'C:\Program Files\7-Zip\7z.sfx'
if (-not (Test-Path -LiteralPath $sevenZip)) { $sevenZip = 'C:\Program Files (x86)\7-Zip\7z.exe'; $sfxMod = 'C:\Program Files (x86)\7-Zip\7z.sfx' }
if (-not (Test-Path -LiteralPath $sevenZip)) { Fail '未找到 7-Zip（C:\Program Files\7-Zip\7z.exe）' }
if (-not (Test-Path -LiteralPath $sfxMod))   { Fail "未找到 SFX 模块 $sfxMod" }

# ---------- 2. 找 zip ----------
if ($Zip -eq '') {
    $zipFile = Get-ChildItem -LiteralPath $Dist -Filter 'AirType-Setup-*-x64.zip' -File -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $zipFile) { Fail "dist 下没有 AirType-Setup-*-x64.zip，请先运行 build_installer.ps1" }
    $Zip = $zipFile.FullName
}
if (-not (Test-Path -LiteralPath $Zip)) { Fail "zip 不存在: $Zip" }
$base = [regex]::Match((Split-Path -Leaf $Zip), '(AirType-Setup-.+-x64)\.zip').Groups[1].Value
if (-not $base) { $base = 'AirType-Setup-0.1.0-x64' }
$ExePath = Join-Path $Dist "$base.exe"
Log "源 zip: $Zip" 'Cyan'
Log "输出 exe: $ExePath" 'Cyan'

# ---------- 3. 解压 zip 到临时目录 ----------
$work = Join-Path $InstallerRoot '.exework'
if (Test-Path -LiteralPath $work) { [System.IO.Directory]::Delete($work, $true) }
New-Item -ItemType Directory -Force -Path $work | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $work)
if (-not (Test-Path -LiteralPath (Join-Path $work 'INSTALL.bat'))) { Fail 'zip 里没有 INSTALL.bat' }
Log '已解压 zip（含子目录）。' 'Green'

# ---------- 4. 压成 .7z ----------
$arc = Join-Path $InstallerRoot '.payload.7z'
if (Test-Path -LiteralPath $arc) { Remove-Item -LiteralPath $arc -Force }
& $sevenZip a -t7z -mx=9 $arc "$work\*" 2>&1 | Out-Null
if (-not (Test-Path -LiteralPath $arc)) { Fail '7z 压缩失败' }
Log '已生成 7z 数据段。' 'Green'

# ---------- 5. SFX 配置 ----------
# InstallDirectory 留空 -> 解压到临时目录；RunProgram 相对该目录，INSTALL.bat 用 %~dp0 定位同目录文件。
$cfg = Join-Path $InstallerRoot '.sfxconfig.txt'
$cfgText = @"
;!@Install@!UTF-8!
Title="AirType Setup"
RunProgram="INSTALL.bat"
;!@InstallEnd@!
"@
[System.IO.File]::WriteAllText($cfg, $cfgText, [System.Text.UTF8Encoding]::new($false))

# ---------- 6. 字节拼接 exe ----------
if (Test-Path -LiteralPath $ExePath) { Remove-Item -LiteralPath $ExePath -Force }
$out = [System.IO.File]::Create($ExePath)
try {
    foreach ($part in @($sfxMod, $cfg, $arc)) {
        $bytes = [System.IO.File]::ReadAllBytes($part)
        $out.Write($bytes, 0, $bytes.Length)
    }
} finally { $out.Dispose() }
Log '已拼接自解压 exe。' 'Green'

# ---------- 7. 清理 ----------
if (Test-Path -LiteralPath $work) { [System.IO.Directory]::Delete($work, $true) }
if (Test-Path -LiteralPath $arc)  { [System.IO.File]::Delete($arc) }
if (Test-Path -LiteralPath $cfg)  { [System.IO.File]::Delete($cfg) }

$size = [math]::Round((Get-Item -LiteralPath $ExePath).Length / 1MB, 1)
Log "完成: $ExePath ($size MB)" 'Green'
Log '双击 exe -> 自动解压并运行 INSTALL.bat（弹 UAC）。验证见包内 VERIFY.md。' 'Gray'
