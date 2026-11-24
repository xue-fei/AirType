<#
.SYNOPSIS
    用 Inno Setup 6 把 dist 里的产物编译成带正规向导/卸载器的安装 EXE。

.DESCRIPTION
    1. 定位 ISCC.exe（用户级安装 / Program Files 两种路径都找）
    2. 检查 airtype.iss 需要的源文件是否齐全
    3. 调 ISCC 编译，输出 dist\AirType-Setup-<版本>-x64-inno.exe

    与 make_exe.ps1（7-Zip SFX）并存，作为新一代打包方案。
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Dist = Join-Path $Root 'dist'
$Iss  = Join-Path $Root 'airtype.iss'

function Log([string]$s, [string]$c = 'Gray') { Write-Host "[inno] $s" -ForegroundColor $c }
function Fail([string]$s) { Write-Host "[inno] 错误: $s" -ForegroundColor Red; exit 1 }

# ---------- 1. 定位 ISCC ----------
$candidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe',
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
    'C:\Program Files (x86)\Inno Setup 7\ISCC.exe'
)
$iscc = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { Fail '找不到 ISCC.exe。请先安装 Inno Setup 6。' }
Log "ISCC: $iscc" 'Cyan'

# ---------- 2. 检查源文件 ----------
$required = @(
    (Join-Path $Dist 'airtype_tsf.dll'),
    (Join-Path $Dist 'airtype_ffi.dll'),
    (Join-Path $Dist 'airtype_tsf_probe.exe'),
    (Join-Path $Root 'config\airtype.toml'),
    (Join-Path $Root 'tools\simulate_voice.ps1'),
    (Join-Path $Root 'tools\close_status.ps1'),
    (Join-Path $Root 'VERIFY.md')
)
foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath $f)) { Fail "缺少源文件：$f（先跑 build_installer.ps1）" }
}
Log '源文件齐全。' 'Green'

# ---------- 3. 编译 ----------
Log '编译 Inno Setup 安装包...' 'Cyan'
& $iscc $Iss 2>&1 | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { Fail "ISCC 编译失败（exit=$LASTEXITCODE）" }

$exe = Get-ChildItem -LiteralPath $Dist -Filter 'AirType-Setup-*-x64-inno.exe' -File |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) { Fail '编译看似成功但找不到输出 exe。' }
$size = [math]::Round($exe.Length / 1MB, 1)
Log "完成：$($exe.FullName)（$size MB）" 'Green'

# ---------- 4. 数字签名校验（可选，仅提示） ----------
$sig = Get-AuthenticodeSignature -LiteralPath $exe.FullName
if ($sig.Status -eq 'Valid') {
    Log "自带签名有效：$($sig.SignerCertificate.Subject)" 'Green'
} else {
    Log "安装包未签名或签名无效（$($sig.Status)）；如需签名请用 sign.ps1。" 'DarkYellow'
}
