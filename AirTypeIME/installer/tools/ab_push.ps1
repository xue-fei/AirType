# 降噪 A/B 批量推流：同一批音频推给指定端口的 daemon，收集最终 TEXT。
# 参数：-Port 目标端口 -Tag 标签（on/off，只用于结果标记）-CsvPath 输出 CSV
#
# 注意：参数名不能叫 -Out：PowerShell 会把它当成公共参数 -OutVariable 的前缀匹配，
# 绑定失败后为 null（第一版就是这样静默丢结果的）。
param(
    [Parameter(Mandatory = $true)][int]$Port,
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$CsvPath
)

$ErrorActionPreference = 'Continue'
$sim = 'C:\Program Files\AirType\tools\simulate_voice.ps1'
$dir = 'C:\Users\XUEFEI\Documents\MyProject\AirType\dpdfnet_test'

$cases = @(
    @{ Name = 'clean'; File = 'clean.wav' },
    @{ Name = 'snr20'; File = 'snr20.wav' },
    @{ Name = 'snr15'; File = 'snr15.wav' },
    @{ Name = 'snr10'; File = 'snr10.wav' },
    @{ Name = 'snr5';  File = 'snr5.wav'  },
    @{ Name = 'snr0';  File = 'snr0.wav'  }
)

$rows = @()
foreach ($c in $cases) {
    $wav = Join-Path $dir $c.File
    if (-not (Test-Path -LiteralPath $wav)) {
        Write-Host "  跳过（缺文件）：$($c.File)" -ForegroundColor Yellow
        continue
    }
    # simulate_voice.ps1 用 Write-Host 输出，走 Information 流（6），
    # 只合并 stderr（2>&1）抓不到 TEXT，必须合并全部流。
    $out = & $sim -Port $Port -Wav $wav *>&1
    # 输出可能带 ANSI 色码、也不能保证行首就是 [TEXT，所以整块拼起来用正则抓。
    $all = ($out | Out-String)
    $ms = [regex]::Matches($all, '\[TEXT[^\]]*\]\s*([^\r\n]+)')
    $text = if ($ms.Count -gt 0) { $ms[$ms.Count - 1].Groups[1].Value.Trim() } else { '' }
    $rows += [pscustomobject]@{ tag = $Tag; case = $c.Name; text = $text }
    Write-Host ("  [{0}] {1}: {2}" -f $Tag, $c.Name, $text)
}

$rows | Export-Csv -LiteralPath $CsvPath -NoTypeInformation -Encoding UTF8
Write-Host "已写出 $CsvPath（$($rows.Count) 条）"
