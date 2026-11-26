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

# ---------- 2.1 检查 sherpa fp32 模型 ----------
# airtype.iss 已把模型打进 [Files]；缺模型也能编译，但装完 engine=sherpa 会报
# 「model_dir 不是目录」，等于白装。这里提前拦住。
$modelDir = Join-Path (Split-Path -Parent $Root) 'models\sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20'
$modelRequired = @(
    'encoder-epoch-99-avg-1.onnx',
    'decoder-epoch-99-avg-1.onnx',
    'joiner-epoch-99-avg-1.onnx',
    'tokens.txt'
)
foreach ($m in $modelRequired) {
    $p = Join-Path $modelDir $m
    if (-not (Test-Path -LiteralPath $p)) {
        Fail "缺少模型文件：$p`n模型未随安装包分发，请先下载/放置到 $modelDir"
    }
}
$modelSize = [math]::Round((Get-ChildItem -LiteralPath $modelDir -File |
            Where-Object { $_.Name -notlike '*.int8.onnx' } |
            Measure-Object Length -Sum).Sum / 1MB, 0)
Log "模型就位：$modelDir（fp32 约 $modelSize MB，安装包将明显变大）" 'Cyan'
Log '源文件齐全。' 'Green'

# ---------- 3. 编译 ----------
# 版本目录名 = 版本号 + 构建戳。必须每次唯一：TSF 外壳是 in-proc DLL，
# 被宿主进程常驻锁定，只有落到全新目录才不会覆盖到在用文件，装完也就
# 不需要重启宿主（老进程用老目录、新进程用新目录）。
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Log "构建戳：$stamp（产物将装到 versions\<版本>-$stamp）" 'Cyan'
Log '编译 Inno Setup 安装包...' 'Cyan'
& $iscc "/DBuildStamp=$stamp" $Iss 2>&1 | ForEach-Object { Write-Host $_ }
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
