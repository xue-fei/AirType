# 把已安装配置里的 [streaming] enabled 升为 true（流式识别，边说边出字）。
#
# 为什么需要单独一步：config\airtype.toml 在 [Files] 里是 onlyifdoesntexist，
# 升级安装不会覆盖用户改过的配置；而老模板里写死了 enabled = false，于是
# 新版默认开了流式、老用户却仍然是"松键后一次性出字"。
#
# 只做一次：改完在文件末尾追加标记行，之后再装不会覆盖用户后来手动关闭的选择。
# 全程用 .NET 的 UTF-8 读写（无 BOM），避免 Inno 的 ANSI 往返把中文注释搞坏。

$ErrorActionPreference = 'Stop'
$cfg = Join-Path $env:ProgramFiles 'AirType\config\airtype.toml'
if (-not (Test-Path -LiteralPath $cfg)) { exit 0 }

$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText($cfg, $utf8)

if ($text -match 'airtype-installer:\s*streaming-upgraded') {
    Write-Host '流式配置已升级过，跳过'
    exit 0
}

$lines = $text -split "`r?`n"
$inStreaming = $false
$changed = $false

for ($i = 0; $i -lt $lines.Count; $i++) {
    $t = $lines[$i].Trim()
    if ($t -eq '[streaming]') { $inStreaming = $true; continue }
    if ($inStreaming -and $t.StartsWith('[')) { $inStreaming = $false }
    if ($inStreaming -and $t -match '^\s*enabled\s*=') {
        if ($t -notmatch '=\s*true') {
            $lines[$i] = 'enabled = true'
            $changed = $true
        }
        $inStreaming = $false
    }
}

if ($changed) {
    $lines += '# airtype-installer: streaming-upgraded（安装器已把 [streaming] 打开；手动改回 false 后重装不会再覆盖）'
    [System.IO.File]::WriteAllText($cfg, ($lines -join "`r`n"), $utf8)
    Write-Host '已把 [streaming] enabled 改为 true'
} else {
    Write-Host '[streaming] 无需修改'
}
