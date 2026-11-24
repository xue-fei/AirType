# AirType 安装后自检（无需硬件 / 无需 Python）

安装包已附带模拟工具 `tools\simulate_voice.ps1`，它会伪装成 ESP32 麦克风，
通过 Wi-Fi 同样的 WebSocket 协议把一段音频推给 AirType，用来验证整条链路：

```
模拟音频 → WebSocket(8765) → mock ASR → TSF 写文本到光标 → 悬浮图标跟随光标
```

## 一、安装

1. 双击 `INSTALL.bat`（自动弹 UAC，点“是”）。安装到 `%ProgramFiles%\AirType`。
2. 脚本会重启 explorer 刷新输入法列表。若没自动重启，手动重启或注销重登。
3. 确认键盘布局里出现一个中文输入法 **AirType**（只有一个）。

## 二、启动 AirType 服务

AirType 的 WebSocket 服务是**输入法激活时才启动**的（不是装完就在听）。
所以必须：

1. 打开一个能输入的窗口，推荐 **记事本**（`Win+R` → `notepad`）。
2. 把输入法切换到中文 **AirType**（`Win+空格` 循环切换）。
3. 光标点进记事本正文区，保持闪烁。

此时 DLL 被激活，服务在 `127.0.0.1:8765` 启动。可以验证端口：

```powershell
netstat -ano | findstr 8765
```

应看到 `TCP 127.0.0.1:8765  LISTENING`。没看到就说明输入法没真正激活，
回到第 2 步确认切换成功（任务栏输入法指示器显示 AirType）。

## 三、模拟发音频

打开 PowerShell（`Win+X` → Windows PowerShell），运行：

```powershell
& "$env:ProgramFiles\AirType\tools\simulate_voice.ps1"
```

脚本默认推 440Hz 正弦波 1.5 秒。看到这样的输出即表示链路通：

```
连接 ws://127.0.0.1:8765/ws ...
已连接。
使用正弦波: 440Hz / 1500ms（75 帧）
START  device=airmic-mock-ps  codec=pcm_s16le  16000Hz/16bit/mono
AUDIO  75 帧 / 24000 samples
END
[PARTIAL ...] 你好
[STATUS done ...] {"state": "done", ...}
[TEXT ...] 你好，这是一次 AirType 语音输入测试。
首字=...ms  总耗时=...ms
验证完成：识别文本已返回，查看输入框是否落字、悬浮图标是否贴在光标右侧。
```

## 四、确认三件事

| 检查项 | 预期 |
|---|---|
| **落字** | 记事本出现 `你好，这是一次 AirType 语音输入测试。`（mock 引擎固定返回这段） |
| **悬浮图标** | 光标右侧有一个蓝色圆角胶囊，白色麦克风图形 + `AirType` 文字，鼠标穿透、不挡输入 |
| **图标跟随** | 在记事本里换一行/换位置点击，图标跟到新的光标右侧 |

## 常见问题

- **提示 `simulate_voice.ps1` 找不到**：说明还没安装到默认目录，或安装器没跑完。
  先双击 `AirType-Setup-0.2.0-x64.exe` 完成安装，再用下面的命令确认文件已就位：

  ```powershell
  Test-Path "$env:ProgramFiles\AirType\tools\simulate_voice.ps1"
  ```

  应返回 `True`。临时想先用源码验证也可以：

  ```powershell
  & "C:\Users\XUEFEI\Documents\MyProject\AirType\AirTypeIME\installer\tools\simulate_voice.ps1"
  ```

  但此时注册表可能仍指向源码 `build\bin` 里的旧 DLL，新加的状态图标等修复未必生效。

- **`[TEXT]` 正常但记事本没落字**：说明服务通了但没写到当前光标。确认切的是 AirType 输入法、
  光标在可编辑区（普通 `Edit` 控件没有 TSF 文档，用记事本或浏览器地址栏等 RichEdit/WPF 输入框）。
- **连接失败 / 8765 没在听**：AirType 没激活。切到中文 AirType 并点进输入框后重试。
- **端口被占用**：编辑 `%ProgramFiles%\AirType\config\airtype.toml` 的 `[server].port`，
  重启输入法后用 `-Port <新端口>` 传参运行脚本。
- **想验证真实录音**：`& "...\simulate_voice.ps1" -Wav .\test.wav`，
  WAV 必须是 16kHz / 16bit / 单声道。
- **想验证取消不落字**：加 `-Cancel`。

## 附：深入排障

TSF DLL 被注入后没有控制台，日志走 `OutputDebugString`（可用 Sysinternals DebugView 抓取），
独立探针则输出到终端。关键行：`[最终] 已写入 N 字符`、`状态图标已创建 132x36`。

也可用安装目录自带的诊断探针（不注入、不注册）：

```powershell
& "$env:ProgramFiles\AirType\airtype_tsf_probe.exe" --dllselftest "$env:ProgramFiles\AirType\airtype_tsf.dll"
```
