<#
.SYNOPSIS
    模拟 AirMic Node 通过 WebSocket 推裸 PCM 帧，验证 AirType 上屏链路（零第三方依赖）。

.DESCRIPTION
    AirType 输入法被激活后，内部会在 config\airtype.toml 指定的端口起 WebSocket 服务。
    本脚本伪装成 ESP32 麦克风连上去，依次发 START -> AUDIO... -> END，
    再接收 STATUS / TEXT 帧，打印识别结果。用来在没有硬件时验证：
      - 服务是否起来（端口在听）
      - ASR（mock 引擎）是否返回文本
      - TSF 是否把文本写到光标处、悬浮图标是否跟随

    默认发 440Hz 正弦波；mock 引擎不关心音频内容，照样返回 mock_text。
    也可用 -Wav 推一段真实录音（必须 16kHz/16bit/mono）。

.PARAMETER ServerHost / Port / Path
    WebSocket 地址，默认 ws://127.0.0.1:8765/ws，需与安装目录下
    %ProgramFiles%\AirType\config\airtype.toml 的 [server] 一致。

.PARAMETER DurationMs
    模拟按住 PTT 的时长（毫秒），默认 1500。

.PARAMETER Wav
    改为推送真实 WAV 文件（16kHz / 16bit / 单声道）。

.PARAMETER Fast
    不按 20ms 实时节奏 sleep，尽快推完（调试用）。

.PARAMETER Cancel
    结尾发 CANCEL 而不是 END，用于验证取消不落字。

.EXAMPLE
    .\simulate_voice.ps1
    .\simulate_voice.ps1 -Wav .\test.wav
    .\simulate_voice.ps1 -Port 8790 -DurationMs 3000
#>
[CmdletBinding()]
param(
    [string]$ServerHost = '127.0.0.1',
    [int]$Port = 8765,
    [string]$Path = '/ws',
    [string]$Device = 'airmic-mock-ps',
    [int]$DurationMs = 1500,
    [int]$Freq = 440,
    [string]$Wav = '',
    [switch]$Fast,
    [switch]$Cancel,
    [int]$TimeoutSec = 20
)

$ErrorActionPreference = 'Stop'

# 帧协议常量（v1，8 字节头 LE）
$MAGIC      = 0xA1
$VERSION    = 0x01
$KIND_START  = 0x01
$KIND_AUDIO  = 0x02
$KIND_END    = 0x03
$KIND_CANCEL = 0x04
$KIND_PING   = 0x05
$KIND_PONG   = 0x06
$KIND_STATUS = 0x81
$KIND_TEXT   = 0x82
$SR = 16000
$FRAME_MS = 20
$SAMPLES_PER_FRAME = $SR * $FRAME_MS / 1000   # 320

function Log([string]$s, [string]$c = 'Gray') { Write-Host $s -ForegroundColor $c }
function Fail([string]$s) { Write-Host "错误: $s" -ForegroundColor Red; exit 1 }

# 组帧：<BBBBHH magic,version,kind,flags,seq(u16 LE),length(u16 LE)> + payload
function New-AirFrame([byte]$Kind, [int]$Seq, [byte[]]$Payload) {
    $len = if ($Payload) { $Payload.Length } else { 0 }
    $buf = [byte[]]::new(8 + $len)
    $buf[0] = $MAGIC
    $buf[1] = $VERSION
    $buf[2] = $Kind
    $buf[3] = 0
    $buf[4] = [byte]($Seq -band 0xFF)
    $buf[5] = [byte](($Seq -shr 8) -band 0xFF)
    $buf[6] = [byte]($len -band 0xFF)
    $buf[7] = [byte](($len -shr 8) -band 0xFF)
    if ($len -gt 0) { [Array]::Copy($Payload, 0, $buf, 8, $len) }
    return ,$buf
}

# 生成正弦波 PCM 帧（16bit LE mono）
function New-ToneChunks([int]$DurationMs, [int]$Freq) {
    $frames = [math]::Max(1, [int]($DurationMs / $FRAME_MS))
    $phase = 0
    for ($f = 0; $f -lt $frames; $f++) {
        $pcm = [byte[]]::new($SAMPLES_PER_FRAME * 2)
        for ($i = 0; $i -lt $SAMPLES_PER_FRAME; $i++) {
            $v = [int](3000 * [math]::Sin(2 * [math]::PI * $Freq * ($phase + $i) / $SR))
            $pcm[2 * $i]     = [byte]($v -band 0xFF)
            $pcm[2 * $i + 1] = [byte](($v -shr 8) -band 0xFF)
        }
        $phase += $SAMPLES_PER_FRAME
        ,$pcm
    }
}

# 读取标准 PCM WAV 的 data chunk，按 640 字节切帧
function Read-WavChunks([string]$File) {
    if (-not (Test-Path -LiteralPath $File)) { Fail "WAV 文件不存在: $File" }
    $all = [System.IO.File]::ReadAllBytes($File)
    if ($all.Length -lt 44 -or
        [Text.Encoding]::ASCII.GetString($all, 0, 4) -ne 'RIFF' -or
        [Text.Encoding]::ASCII.GetString($all, 8, 4) -ne 'WAVE') {
        Fail '不是有效的 WAV 文件'
    }
    $channels = 0; $rate = 0; $bits = 0; $fmt = 0
    $dataOff = -1; $dataLen = 0
    $pos = 12
    while ($pos + 8 -le $all.Length) {
        $id  = [Text.Encoding]::ASCII.GetString($all, $pos, 4)
        $sz  = [BitConverter]::ToInt32($all, $pos + 4)
        $body = $pos + 8
        if ($id -eq 'fmt ') {
            $fmt      = [BitConverter]::ToUInt16($all, $body)
            $channels = [BitConverter]::ToUInt16($all, $body + 2)
            $rate     = [BitConverter]::ToInt32($all, $body + 4)
            $bits     = [BitConverter]::ToUInt16($all, $body + 14)
        } elseif ($id -eq 'data') {
            $dataOff = $body; $dataLen = $sz
        }
        $pos = $body + $sz + ($sz % 2)
    }
    if ($dataOff -lt 0) { Fail 'WAV 缺少 data chunk' }
    if ($fmt -ne 1 -or $channels -ne 1 -or $rate -ne $SR -or $bits -ne 16) {
        Fail "WAV 格式必须是 PCM 16kHz/16bit/mono（当前 fmt=$fmt ch=$channels rate=$rate bits=$bits）"
    }
    $bytesPerFrame = $SAMPLES_PER_FRAME * 2
    $chunks = @()
    for ($off = $dataOff; $off + $bytesPerFrame -le $dataOff + $dataLen; $off += $bytesPerFrame) {
        $c = [byte[]]::new($bytesPerFrame)
        [Array]::Copy($all, $off, $c, 0, $bytesPerFrame)
        $chunks += ,$c
    }
    return $chunks
}

# ---------- 连接 ----------
$uri = [Uri]::new("ws://${ServerHost}:${Port}${Path}")
Log "连接 ws://${ServerHost}:${Port}${Path} ..." 'Cyan'
$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$cts = [System.Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($TimeoutSec))
try {
    [void]$ws.ConnectAsync($uri, $cts.Token).GetAwaiter().GetResult()
} catch {
    Fail ("连接失败: $($_.Exception.Message)`n" +
          "  1) 先把系统输入法切到中文(AirType)，服务在 DLL 激活后才启动；`n" +
          "  2) 确认端口与 %ProgramFiles%\AirType\config\airtype.toml 的 [server].port 一致。")
}
Log "已连接。" 'Green'

function Send-Binary([byte[]]$Data) {
    $seg = [System.ArraySegment[byte]]::new($Data)
    [void]$ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Binary,
                        $true, $cts.Token).GetAwaiter().GetResult()
}

# ---------- 音频数据 ----------
if ($Wav) {
    $chunks = @(Read-WavChunks $Wav)
    Log "使用 WAV: $Wav（$($chunks.Count) 帧）" 'Yellow'
} else {
    $chunks = @(New-ToneChunks $DurationMs $Freq)
    Log "使用正弦波: ${Freq}Hz / ${DurationMs}ms（$($chunks.Count) 帧）" 'Yellow'
}

# ---------- START ----------
$metaObj = [ordered]@{
    device = $Device
    codec  = 'pcm_s16le'
    sample_rate = $SR
    channels = 1
    bits_per_sample = 16
}
$meta = [Text.Encoding]::UTF8.GetBytes(($metaObj | ConvertTo-Json -Compress))
Send-Binary (New-AirFrame $KIND_START 0 $meta)
Log "START  device=$Device  codec=pcm_s16le  ${SR}Hz/16bit/mono" 'Cyan'

# ---------- AUDIO ----------
$seq = 1
$sendWatch = [Diagnostics.Stopwatch]::StartNew()
foreach ($chunk in $chunks) {
    Send-Binary (New-AirFrame $KIND_AUDIO $seq $chunk)
    $seq = ($seq + 1) -band 0xFFFF
    if (-not $Fast) {
        $target = ($seq - 1) * $FRAME_MS
        $wait = $target - $sendWatch.ElapsedMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
    }
}
$totalSamples = ($chunks | ForEach-Object { $_.Length / 2 } | Measure-Object -Sum).Sum
Log "AUDIO  $($chunks.Count) 帧 / $totalSamples samples" 'Cyan'

# ---------- END / CANCEL ----------
Send-Binary (New-AirFrame $(if ($Cancel) { $KIND_CANCEL } else { $KIND_END }) $seq $null)
Log "$(if ($Cancel) { 'CANCEL' } else { 'END' })" 'Cyan'

# ---------- 接收 STATUS / TEXT ----------
$recvBuf = [byte[]]::new(65536)
$pending = [System.Collections.Generic.List[byte]]::new()
$sawDone = $false
$gotText = $false
$firstPartialMs = $null
$recvWatch = [Diagnostics.Stopwatch]::StartNew()

while (-not $cts.IsCancellationRequested -and -not $gotText) {
    $rx = $ws.ReceiveAsync([System.ArraySegment[byte]]::new($recvBuf), $cts.Token).GetAwaiter().GetResult()
    if ($rx.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { break }
    for ($i = 0; $i -lt $rx.Count; $i++) { $pending.Add($recvBuf[$i]) }

    # 从累积缓冲里逐帧解析（处理 TCP 粘包/半包）
    while ($pending.Count -ge 8) {
        if ($pending[0] -ne $MAGIC -or $pending[1] -ne $VERSION) {
            $pending.RemoveAt(0)   # 失步，逐字节重新同步
            continue
        }
        $len = $pending[6] -bor ($pending[7] -shl 8)
        if ($pending.Count -lt 8 + $len) { break }
        $kind = $pending[2]
        $body = [byte[]]::new($len)
        for ($i = 0; $i -lt $len; $i++) { $body[$i] = $pending[8 + $i] }
        $pending.RemoveRange(0, 8 + $len)

        switch ($kind) {
            $KIND_STATUS {
                $json = [Text.Encoding]::UTF8.GetString($body)
                try { $st = $json | ConvertFrom-Json } catch { continue }
                $state = [string]$st.state
                $ms = [int]$recvWatch.ElapsedMilliseconds
                if ($state -eq 'partial') {
                    if ($null -eq $firstPartialMs) { $firstPartialMs = $ms }
                    Log "[PARTIAL ${ms}ms] $($st.text)" 'DarkGray'
                } elseif ($state -eq 'done') {
                    $sawDone = $true
                    Log "[STATUS done ${ms}ms] $json" 'DarkGray'
                    if ($st.message -eq 'empty utterance') { $gotText = $true }
                } elseif ($state -in @('cancelled', 'error')) {
                    Log "[STATUS $state ${ms}ms] $json" 'DarkYellow'
                    $gotText = $true
                } else {
                    Log "[STATUS ${ms}ms] $json" 'DarkGray'
                }
            }
            $KIND_TEXT {
                $text = [Text.Encoding]::UTF8.GetString($body)
                $ms = [int]$recvWatch.ElapsedMilliseconds
                Log "[TEXT ${ms}ms] $text" 'Green'
                $gotText = $true
            }
            $KIND_PONG { Log '[PONG]' 'DarkGray' }
        }
    }
}

$first = if ($null -ne $firstPartialMs) { "${firstPartialMs}ms" } else { 'n/a' }
Log "首字=$first  总耗时=$([int]$recvWatch.ElapsedMilliseconds)ms" 'Yellow'
if ($gotText) {
    Log '验证完成：识别文本已返回，查看输入框是否落字、悬浮图标是否贴在光标右侧。' 'Green'
} else {
    Log '未收到最终文本（超时或服务无响应）。检查 DLL 是否已激活、注入模式是否为 clipboard_paste。' 'DarkYellow'
}

try { $ws.Dispose() } catch {}
try { $cts.Dispose() } catch {}
