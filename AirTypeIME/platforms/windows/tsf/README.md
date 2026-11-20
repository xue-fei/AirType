# Windows TSF 外壳

把 AirType 接入 Windows 文本服务框架（TSF），让识别结果以「输入法」的方式写进任意
输入框。Rust 核心只负责收流和识别，注入由本目录的 C++ 外壳负责。

## 当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| M1 | CMake 工程 + `airtype_tsf_probe` 风险探针 + `FfiBridge` | ✅ 已完成 |
| M2 | `airtype_tsf.dll`：`ITfTextInputProcessorEx` + 编辑会话 + 注册 | ✅ 已完成（2026-11-19 管理员 `regsvr32` 注册成功） |
| M3 | `HiddenWindow` marshal、生命周期、端到端联调 | ✅ marshal + 真实落字均已验证（2026-11-19） |

> **注册状态**：`{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}` 已写入
> `HKLM\SOFTWARE\Classes\CLSID`，语言配置与键盘类别均已注册（2026-11-19）。
> 注册路径固定指向 `build\bin\airtype_tsf.dll`，**重新构建不会失效**；只有移动
> DLL 或换机器才需要重新注册。

## 目录

```
tsf/
├── CMakeLists.txt          MSVC + C++17，Ninja 生成器；两个目标（probe exe + tsf dll）
├── build.ps1               一键构建（手工拼 MSVC 环境，不依赖 vcvars/reg.exe）
├── include/airtype_ffi.h   Rust 核心的 C ABI 契约
├── src/
│   ├── airtype_tsf.def     COM 入口导出表（STDAPI 不含 dllexport，必须 .def）
│   ├── Log.h               OutputDebugString + stderr 日志
│   ├── TsfGuids.h/.cpp     文本服务 CLSID / profile GUID
│   ├── TsfTypes.h          注册窗口消息 + AirTypePayload
│   ├── FfiBridge.h/.cpp    airtype_ffi.dll 的加载与封装
│   ├── ModuleUtil.h/.cpp   本模块目录 / 宿主 exe 目录解析
│   ├── TsfString.h         UTF-8 <-> UTF-16
│   ├── HiddenWindow.h/.cpp MTA 回调 -> STA 线程的 marshal
│   ├── TsfEditSession.h/.cpp  替换式编辑会话（带写入范围缓存）
│   ├── TsfTextService.h/.cpp  ITfTextInputProcessorEx + ITfThreadMgrEventSink
│   ├── dllmain.cpp         DllMain / 类工厂 / 注册注销
│   └── TsfProbe.cpp        M1 探针 + HiddenWindow marshal 验证
└── build/                  构建产物（已 gitignore）
```

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1              # Debug
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release -Clean
powershell -ExecutionPolicy Bypass -File build.ps1 -OutDir D:\side   # 产物另存
```

产物：

| 文件 | 说明 |
|---|---|
| `build/bin/airtype_tsf.dll` | in-proc COM 服务器（TSF 文本服务） |
| `build/bin/airtype_ffi.dll` | 脚本自动从 `target\<config>\` 同步过来的 Rust 核心 |
| `build/bin/airtype_tsf_probe.exe` | 独立探针，不注册、不注入 |

> `build.ps1` 构建完成后会**自动把 `airtype_ffi.dll` 复制进 `build\bin`**。
> 少了它 `--dllselftest` 会报 `airtype_ffi.dll 加载失败: 126`（`ERROR_MOD_NOT_FOUND`），
> 但 COM 激活仍然返回 `S_OK`——判断核心是否起来要看日志里的「核心已启动」，
> 不要只看「测试通过」。

> **DLL 被占用时用 `-OutDir` 旁路构建**：`build\bin` 下的 DLL 正被宿主进程加载时，
> 链接会失败（`LNK1104: 无法打开文件 "bin\airtype_tsf.dll"`）。加 `-OutDir <目录>`
> 把两个目标输出到别处即可完整编译链接，用于验证改动是否能过编译；产物目录可以
> 自己选（不存在会先创建）。注意这只绕过**输出**，`-Clean` 仍会去删 `build\bin`。

## 运行期文件布局

外壳是 in-proc DLL，会被系统注入到任意宿主进程（记事本、Word…），**宿主 exe
目录下不会有 `airtype_ffi.dll`**。因此加载路径按「本模块目录 → 宿主 exe 目录」
两级查找（`src/ModuleUtil.cpp`）：

| 文件 | 查找顺序 | 缺失后果 |
|---|---|---|
| `airtype_ffi.dll` | DLL 同目录 → 宿主 exe 同目录 | 核心起不来，激活仍返回 `S_OK` |
| `config\airtype.toml` | DLL 同目录 → 宿主 exe 同目录 | 回退到 `Config::default()`（mock 引擎、127.0.0.1:8765） |
| `airtype.toml` | 同上 | 同上 |

> 早期版本只查宿主 exe 目录，注入到记事本后必然加载失败。跨目录验证方法：
> 把 `airtype_tsf_probe.exe` 单独复制到空目录，用绝对路径指向 `build\bin` 的
> DLL 跑 `--dllselftest`——修复前报 126，修复后「核心已启动」。

`build.ps1` 不调用 `vcvars64.bat`——它会用 `reg.exe` 查注册表，在受限环境下会失败。
脚本直接拼装 `INCLUDE` / `LIB` / `PATH`，用 VS 自带的 `cmake.exe` + `ninja.exe`。

> **导出表**：`dllmain.cpp` 的四个入口用 `STDAPI` 声明，该宏不含 `dllexport`，
> 必须由 `src/airtype_tsf.def` 显式导出。漏掉 `.def` 时 DLL 能链接成功但导出表为空，
> 系统激活时 `GetProcAddress("DllGetClassObject")` 会失败。可用
> `dumpbin /exports build/bin/airtype_tsf.dll` 复核，应看到 4 个未修饰名。

## 运行链路

```
ESP32-S3 麦克风 --WiFi--> airtype_tsf.dll 内置的 WebSocket 服务（127.0.0.1:8765）
                             │  帧协议 v1（Start / Audio… / End）
                             ▼
                    airtype_ffi.dll（tokio runtime / MTA）
                             │  on_text / on_partial 裸指针回调
                             │  PostMessage 到隐藏窗口（marshal 回 STA）
                             ▼
                    ITfContext::RequestEditSession -> ITfRange::SetText
```

> 外壳自身就是服务端：`airtype_start()` 调用 `airtype_daemon::run_server`，
> 不需要另外启动 `airtype-daemon.exe`。配置里的 `[server]` 决定监听地址。

**partial 策略**：不用 TSF composition。外壳记住上次写入的 `ITfRange`，每次
partial / final 到达就用累积文本整体替换该 range；换焦点文档时丢弃旧 range。
partial 与 final 走同一条替换路径，保证「边说边出字」和最终结果一致。

**核心初始化不阻塞 UI 线程**：`ffi_.Init()` 要加载 sherpa 模型（数百毫秒），
`Activate` 若同步等待，输入法激活时目标程序会卡住。因此 `Activate` 只起一个
后台线程跑 `Load → Init → 注册回调 → Start` 后立即返回 `S_OK`；`Deactivate`
先置 `stopping_` 标志、调 `ffi_.Stop()`，再 `join` 回收线程。

**丢字兜底**：TSF 焦点可能晚于文本到达（例如刚点进输入框）。`WriteText` 拿不到
焦点文档时不再直接丢弃，而是缓存本帧文本，等 `OnSetFocus` 到达时补写。

**替换后复位光标**：`ITfRange::SetText` 写完会留下选中态，下一次替换若沿用同一个
range 起始锚点，插入位置会跑偏。因此替换完成后不折叠缓存的 `written_`（折叠会破坏
下一帧替换的区间），而是克隆一个 caret、`Collapse(TF_ANCHOR_END)`，再
`ITfContext::SetSelection(ec, 1, &sel)`（`ase = TF_AE_NONE`）把焦点后的光标位置
显式交回 TSF。这与 M1 探针已验证的写入后处理一致。

**缓存 range 失效回退**：TSF 的 `ITfRange` 在文档被外部改动后会被作废，
`Clone(previous_)` 可能失败。此时不再直接放弃本帧，而是丢弃旧 range、回退到
`GetSelection(TF_DEFAULT_SELECTION)` 取当前选区重写；写入失败也会
`ResetWrittenRange()`，让下一帧改用当前选区，避免连续失败导致整段文本丢失。

## HiddenWindow marshal 验证

```powershell
airtype_tsf_probe.exe --marshal
```

独立 exe 内起两个线程复刻真实链路：STA 线程创建隐藏窗口并跑消息循环，主线程
（模拟 Rust tokio worker）只 `PostMessage` 投递 6 帧。验收三点：

1. handler 在**创建窗口的 STA 线程**上执行（打印两个 tid，不相等）；
2. 6/6 帧按序到达，文本与 `final` 标志逐帧一致；
3. `Destroy()` 后用旧句柄 `PostMessage` 返回 0，窗口类已注销。

实测结果（2026-11-19）：`STA tid=3576 / 主 tid=56176`，6/6 一致，销毁后返回 0，
**通过**。

> 注意：`Destroy()` 会把 `hwnd_` 置空，此时 `Handle()` 返回 `NULL`；而
> `PostMessageW(NULL, ...)` 是**广播**语义会返回成功。断言必须用销毁前保存的旧句柄。
>
> `hwnd_` 是 `std::atomic<HWND>`：`PostText` 在 MTA 线程读、`Destroy` 在 STA 线程写。
> `PostText` 只取一次句柄——分成 `Valid()` + `Handle()` 两次读会形成 TOCTOU，
> 中间窗口被销毁时第二次读到 `NULL`，`PostMessageW(NULL,...)` 就会把 payload
> 指针广播给全系统顶层窗口。`Destroy` 用 `exchange(nullptr)` 原子取走句柄。

## M1 探针

```powershell
# 诊断模式：报告 TSF 链路各步骤的 HRESULT
airtype_tsf_probe.exe

# 自建窗口 + 写入 + 截图
airtype_tsf_probe.exe --window --preset "预设ABC" --write "你好，TSF" --shot out.bmp

# 加载 Rust 核心，验证导出表
airtype_tsf_probe.exe --ffi ..\..\..\..\target\release\airtype_ffi.dll

# 只重建本 exe（目标进程锁住 DLL 时用）
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release -ProbeOnly
```

模式一览：

| 开关 | 作用 |
|---|---|
| （无） | 诊断：报告 TSF 链路各步骤 HRESULT |
| `--write <text>` / `--preset <text>` / `--window` / `--shot <bmp>` | 自建窗口写入对照 + 截图 |
| `--ffi <dll>` | 加载并报告 Rust 核心版本 |
| `--marshal` | 验证 HiddenWindow 跨线程投递与生命周期 |
| `--dllselftest <dll>` | 直接激活文本服务（不注册） |
| `--activate [--click]` | 把 AirType 切成当前桌面活动输入处理器 |
| `--deactivate [--flags <n>]` | 停用 AirType，回退系统默认键盘 |
| `--cursor` / `--uia` | 报告鼠标位置 / 光标下控件类型与内容（只读） |
| `--shotfg <bmp>` | 抓整块屏幕 |

### M1 结论（实测）

1. **COM 编辑会话链路可用**：`CoCreateInstance(ITfThreadMgr)` → `Activate` →
   `GetFocus` → `GetTop` → `RequestEditSession` → `GetSelection` →
   `ITfRange::SetText` → `SetSelection` 全部返回 `S_OK`，写入后从 TSF 侧
   `GetText` 回读与输入完全一致。
2. **跨进程拿不到目标文档**：记事本在前台时，探针（独立进程）的
   `GetFocus` 恒返回 `S_FALSE`。TSF 的焦点是**按线程**跟踪的，写入方必须运行在
   拥有焦点的那个进程里——这正是 in-proc DLL 的设计前提。
3. **自建窗口拿到的是隐式文档**：探针自己的 RichEdit 控件即使持有 Win32 焦点
   （`GetFocus() == 控件 HWND` 已验证），`GetFocus` 返回的文档仍是空的，
   整文档转储为 0 字符；写入只进这个隐式文档，控件内容不变。
   说明标准 EDIT / RichEdit 不会自动注册为 TSF 文本存储。

## 注册与卸载（需管理员）

```powershell
# 注册（写 HKCR\CLSID + TSF profile + 键盘类别）
regsvr32.exe build\bin\airtype_tsf.dll

# 卸载
regsvr32.exe /u build\bin\airtype_tsf.dll
```

已于 2026-11-19 用管理员执行注册成功。注册内容：
- `HKLM\SOFTWARE\Classes\CLSID\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}`
  + `InprocServer32`（`ThreadingModel=Apartment`，指向 `build\bin\airtype_tsf.dll`）
- `ITfInputProcessorProfiles::Register` + `AddLanguageProfile`（zh-CN 0x0804 / en-US 0x0409）
- `ITfCategoryMgr::RegisterCategory(GUID_TFCAT_TIP_KEYBOARD)`

## 无硬件端到端测试（AirMicNode 缺席时）

没有 ESP32 也能验证「TSF 落字」这条链路：外壳自己就是 WebSocket 服务端，
用 `tools/mock_airmic.py` 假装有设备在推流即可。默认配置是 mock 引擎，
**识别结果与音频内容无关**，所以用正弦波或任意 16k/16bit/mono WAV 都行。

### 先做无 GUI 冒烟（推荐先跑）

`--dllselftest` 会激活文本服务并泵消息 3 秒，这期间外壳的 WebSocket 服务已经
在监听，可以顺便把协议链路一起验证掉——不需要硬件、不需要切键盘：

```powershell
# 终端 A：启动外壳（会持续 3 秒）
.\build\bin\airtype_tsf_probe.exe --dllselftest .\build\bin\airtype_tsf.dll

# 终端 B：等约 1 秒让核心起来，再推流
python tools/mock_airmic.py --fast
```

终端 A 应看到：

```
[airtype] AirType 核心已启动 version=0.1.0 config=(内置默认)
[airtype] 拿不到焦点文档，缓存本帧文本（23 字符），等焦点到达
[airtype] [最终] 未写入（已缓存，等焦点） 23 字符
```

「未写入」是**正确**的：探针线程没有 TSF 焦点文档，走的是丢字兜底分支。
这一步验证了 `ffi 加载 → 服务监听 → 帧协议 → 引擎 → 回调 → marshal` 全链路。

### 再验真实落字（自动切键盘，已实测）

不用手工点语言栏，`--activate` 直接调 `ITfInputProcessorProfileMgr::ActivateProfile`
把 AirType 切成**当前桌面所有线程**的活动输入处理器（`TF_IPPMF_FORSESSION`），
前台程序那个线程也就跟着切过来了。

```powershell
# 1) 构建（Release 即可，脚本会带上 airtype_ffi.dll）
powershell -ExecutionPolicy Bypass -File build.ps1 -Config Release

# 2) 确认鼠标下的控件确实是输入框（只读，安全）
.\build\bin\airtype_tsf_probe.exe --uia
#    期望 控件类型: Edit，且「已聚焦: 是」

# 3) 切成 AirType（保持鼠标不动，不要点别的地方）
.\build\bin\airtype_tsf_probe.exe --activate
#    期望「AirType 是否已激活: 是」

# 4) 推流（在 AirTypeIME 目录下执行）
python tools/mock_airmic.py --fast

# 5) 读回控件内容，硬证据
.\build\bin\airtype_tsf_probe.exe --uia
#    期望 ValuePattern 内容: '你好，这是一次 AirType 语音输入测试。'

# 6) 测完把输入法切回去（否则键盘会被 TIP 接管，无法正常打字）
.\build\bin\airtype_tsf_probe.exe --deactivate
```

> **实测结果（2026-11-19）**：对 WorkBuddy 的一个 Edit 控件走完 2→6，
> 第 5 步 `ValuePattern 内容` 读到了完整识别文本，落字成功。
>
> **注意**
> - `DeactivateProfile` **不接受** `TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE`
>   （会返回 `E_INVALIDARG`），所以 `--deactivate` 默认 `flags=0`；
>   `--flags <n>` 仅供试验。
> - 目标进程加载着 `airtype_ffi.dll` 时重新构建，复制会因文件占用失败。
>   脚本只警告不中断；`airtype_tsf_probe.exe` 可用 `-ProbeOnly` 单独更新。
> - 也可以手工切：任务栏输入指示器 → 「AirType 语音输入」
>   （列表里没有就重启 explorer.exe：TSF 会缓存文本服务列表）。

### 换成真实识别

把 `config\airtype-sherpa.toml` 复制成 DLL 同目录的 `config\airtype.toml`
（模型路径按实际改），再：

```powershell
python tools/mock_airmic.py --wav <16k-mono.wav> --fast
```

`--fast` 只是不等实时节奏，识别质量不受影响。

### 判定要点

| 现象 | 结论 |
|---|---|
| 输入框无反应，日志有「核心已启动」 | 键盘没切到 AirType，或 TSF 未加载新 DLL |
| 输入框无反应，日志报 `加载失败: 126` | `airtype_ffi.dll` 不在 DLL 同目录 |
| `--activate` 报 `E_INVALIDARG` | 漏了 `TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE` |
| 出现文字但位置/内容错乱 | 替换式编辑会话问题，看 `[中间]/[最终] 已写入 N 字符` |
| 日志只有「拿不到焦点文档，缓存本帧文本」 | 焦点不在 TSF 感知控件上 |

> 注意：`--dllselftest` 只验证 COM 激活链路，**核心加载失败时它仍打印「通过」**
> （`ActivateEx` 起后台线程，失败不回传）。务必同时确认日志里的「核心已启动」。

## M3 端到端验收标准

1. 注册后重启 `explorer.exe`（TSF 缓存文本服务列表）。
2. 打开一个 TSF 感知应用（记事本 / WordPad / 浏览器地址栏）。
3. 触发 PTT 说话，确认：
   - 边说边出字（partial 实时替换）；
   - 松手后最终文本与中间态一致，无重复、无残留；
   - 切换输入框 / 窗口后不会写错文档。
4. 日志（DebugView 或 stderr）应出现 `[中间] 已写入 N 字符` 递增，最后一条
   `[最终] 已写入 N 字符`。日志现在会区分「已写入」与「未写入（已缓存，等焦点）」，
   后者说明焦点还不在 TSF 感知控件上，不要当成成功。

## C ABI 契约

头文件：[`include/airtype_ffi.h`](include/airtype_ffi.h)
产物：`cargo build -p airtype-ffi --release` → `target/release/airtype_ffi.dll`

```c
#include "airtype_ffi.h"

static void CALLTYPE on_text(const char* utf8, void* userdata) {
    // 回调在 Rust 的 tokio worker 线程触发，不在 UI 线程。
    // 必须 marshal 回编辑会话线程再写 ITfRange，不要在此做耗时操作。
}

static void CALLTYPE on_partial(const char* text, int32_t delete_chars,
                                const char* append, void* userdata) {
    // 边说边出字：text 是当前累积文本，delete_chars/append 是最小编辑量。
    // TSF 侧只用 text 做整体替换即可。
}

// ITfTextInputProcessor::Activate 中：
airtype_init("C:/path/to/config/airtype.toml");
airtype_set_text_callback(on_text, nullptr);
airtype_set_partial_callback(on_partial, nullptr);
airtype_start();

// Deactivate / DLL_PROCESS_DETACH：
airtype_stop();
```

`airtype_start()` 会**强制把内置注入器设为 `none`**，文本只通过上面两个回调交给
外壳，不会出现「剪贴板粘贴 + TSF 写入」双重输入。

错误码：`AIRTYPE_OK(0)` / `ERR_NULL(-1)` / `ERR_CONFIG(-2)` / `ERR_STATE(-3)` /
`ERR_UTF8(-4)` / `ERR_INTERNAL(-5)`。失败时用 `airtype_last_error()` 取 UTF-8 描述。

## 调试注意事项

- TSF DLL 被注入到**别的进程**，调试器要附加到目标进程（如 notepad.exe）才能下断点。
- 32 位进程需要 32 位 DLL；Rust 侧需另装 `i686-pc-windows-msvc` target。
- 卸载输入法前先 `airtype_stop()`，否则 tokio runtime 随 DLL 卸载可能挂起。
- 注册后 TSF 会缓存文本服务列表，改完 DLL 通常需要重启 explorer.exe 才生效。
- **COM 生命周期**：所有 `CComPtr` 必须先 `.Release()` 再 `CoUninitialize()`。
  反过来的话进程退出时会 segfault（`--activate` 曾踩过，表面现象是「打印完最后一行
  日志后崩」，很难联想到是析构顺序）。
- 目标进程加载着 `airtype_ffi.dll` / `airtype_tsf.dll` 时，构建脚本复制文件会失败
  （只警告不中断），链接也会 `LNK1104`。用 `build.ps1 -OutDir` 旁路输出。
- **`--deactivate` 不足以让已注入的 DLL 卸载**：它只停活动 profile，已经注入到进程
  里的 DLL 实例不会立刻释放。要彻底解锁文件得重启 `explorer.exe`，而 chrome / 微信 /
  WorkBuddy 这类常驻宿主进程只要不退出就一直持有旧 DLL。此时用 `-OutDir` 继续开发。
