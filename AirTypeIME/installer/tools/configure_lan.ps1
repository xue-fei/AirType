# 把 [server] bind 改为 0.0.0.0，让局域网内的 AirMic Node 直连本机。
#
# 只由安装器的 /LAN=1 参数触发（见 airtype.iss 的 ApplyLanBind）：
# 默认安装仍监听 127.0.0.1，避免把语音接收端口暴露给整个局域网。
# [web] 段（设置页面）**不动** —— 那个页面能改配置，暴露比音频端口危险得多。
#
# 全程 .NET UTF-8 读写（无 BOM），避免把中文注释的编码搞坏。

$ErrorActionPreference = 'Stop'
$cfg = Join-Path $env:ProgramFiles 'AirType\config\airtype.toml'
if (-not (Test-Path -LiteralPath $cfg)) { Write-Host '配置不存在，跳过'; exit 1 }

$utf8 = New-Object System.Text.UTF8Encoding($false)
$text = [System.IO.File]::ReadAllText($cfg, $utf8)
$lines = $text -split "`r?`n"

$inServer = $false
$changed = $false
for ($i = 0; $i -lt $lines.Count; $i++) {
    $t = $lines[$i].Trim()
    if ($t -eq '[server]') { $inServer = $true; continue }
    if ($inServer -and $t.StartsWith('[')) { $inServer = $false }
    if ($inServer -and $t -match '^\s*bind\s*=') {
        if ($t -notmatch '0\.0\.0\.0') {
            $lines[$i] = 'bind = "0.0.0.0"'
            $changed = $true
        }
        $inServer = $false
    }
}

if ($changed) {
    [System.IO.File]::WriteAllText($cfg, ($lines -join "`r`n"), $utf8)
    Write-Host '[server] bind 已改为 0.0.0.0'
} else {
    Write-Host '[server] bind 已是 0.0.0.0 或未找到，无需修改'
}

# 防火墙放行：Windows 默认阻止入站，光改 bind 是不够的 —— AirMic 会连接超时。
# 只对 Private/Domain 放行（不要放 Public：公共 WiFi 上暴露语音端口不安全）。
$ruleName = 'AirType AirMic (TCP 8765)'
try {
    $exist = Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue
    if (-not $exist) {
        New-NetFirewallRule -DisplayName $ruleName `
            -Description '允许局域网内的 AirMic Node 推送音频到 AirType 输入法' `
            -Direction Inbound -Protocol TCP -LocalPort 8765 `
            -Action Allow -Profile Private,Domain | Out-Null
        Write-Host "已添加防火墙入站规则：$ruleName"
    } else {
        Write-Host "防火墙规则已存在：$ruleName"
    }
} catch {
    Write-Host "添加防火墙规则失败（AirMic 可能连不上）：$($_.Exception.Message)"
}

# 打印本机局域网地址，AirMic Node 配网时填这个
Write-Host '本机局域网地址（AirMic 配网时填 host）：'
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    ForEach-Object { Write-Host ("  {0}  ({1})" -f $_.IPAddress, $_.InterfaceAlias) }
