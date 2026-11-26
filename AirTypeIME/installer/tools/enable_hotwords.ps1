# 把老配置升级到支持热词：在 [sherpa] 段补 hotwords_file / hotwords_score。
#
# 幂等：已经有 hotwords_file 就完全不动（尊重用户自己改过的值）。
# 边界：只往 [sherpa] 段内插，遇到下一个 [section] 就停，不会污染别的段。

$ErrorActionPreference = 'Stop'

$cfg = Join-Path $PSScriptRoot '..\config\airtype.toml'
$cfg = [System.IO.Path]::GetFullPath($cfg)
if (-not (Test-Path -LiteralPath $cfg)) {
    Write-Host "配置不存在，跳过: $cfg"
    exit 0
}

$lines = [System.IO.File]::ReadAllLines($cfg, [System.Text.Encoding]::UTF8)

# 已有 hotwords 配置就别碰
if ($lines | Where-Object { $_ -match '^\s*hotwords_file\s*=' }) {
    Write-Host '已存在 hotwords_file，跳过'
    exit 0
}

# 定位 [sherpa] 段的末尾
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i].Trim() -eq '[sherpa]') { $start = $i; break }
}
if ($start -lt 0) {
    Write-Host '[sherpa] 段不存在，跳过'
    exit 0
}

$insertAt = $lines.Count
for ($i = $start + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i].TrimStart().StartsWith('[')) { $insertAt = $i; break }
}

# 注意必须强类型：PowerShell 5.1 下 @(...) 是 Object[]，
# List[string].AddRange 不接受，会抛 MethodArgumentConversionInvalidCastArgument。
[string[]]$block = @(
    '# 热词表（AirType 自己过滤 # 注释后再交给 sherpa）。留空 = 不启用。',
    'hotwords_file = "hotwords.txt"',
    'hotwords_score = 1.5',
    ''
)

$out = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($i -eq $insertAt) { $out.AddRange($block) }
    $out.Add($lines[$i])
}
if ($insertAt -ge $lines.Count) { $out.AddRange($block) }

[System.IO.File]::WriteAllLines($cfg, $out, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "已为 [sherpa] 段补上热词配置（插入位置行 $insertAt）"
