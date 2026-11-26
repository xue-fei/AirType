# 给已安装的配置补上 [denoise] 段（流式降噪）。
#
# 为什么需要：config\airtype.toml 在 [Files] 里是 onlyifdoesntexist，升级不会
# 覆盖用户配置；老配置里没有 [denoise] 段，新装的降噪能力就一直不生效。
#
# 幂等：已经有 [denoise] 段就完全不动（包括用户把 enabled 改成 false 的情况）。
# 全程 .NET UTF-8 读写（无 BOM），避免把中文注释的编码搞坏。

$ErrorActionPreference = 'Stop'
$cfg = Join-Path $env:ProgramFiles 'AirType\config\airtype.toml'
if (-not (Test-Path -LiteralPath $cfg)) { exit 0 }

$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText($cfg, $utf8)

if ($text -match '(?m)^\s*\[denoise\]') {
    Write-Host '[denoise] 段已存在，跳过'
    exit 0
}

$block = @'

# 流式语音降噪（DPDFNet）。嘈杂环境（办公室/车里）建议开；安静环境关掉
# 可以省约 20MB 内存。模型 9.8MB，onnxruntime 走动态加载（见 runtime）。
# 注意：降噪在 ASR 之前，音频电平表也基于降噪后的信号。
[denoise]
enabled = true
model_dir = "../models/dpdfnet"
model = "dpdfnet2.onnx"
runtime = "../lib/onnxruntime.dll"
'@

$text = $text.TrimEnd() + "`r`n" + $block + "`r`n"
[System.IO.File]::WriteAllText($cfg, $text, $utf8)
Write-Host '已给配置追加 [denoise] 段（enabled = true）'
