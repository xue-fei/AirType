# AirType TSF 安装器

一键安装 / 更新 / 卸载 AirType 文本服务，**不需要手工 `regsvr32`**。

## 两种打包方案

| 方案 | 产物 | 说明 |
|---|---|---|
| **Inno Setup（推荐）** | `dist\AirType-Setup-<版本>-x64-inno.exe` | 带正规安装向导、中文界面、自动/静默/卸载参数；DLL 注册由 `regserver` 标志完成 |
| 7-Zip SFX（旧） | `dist\AirType-Setup-<版本>-x64.exe` / `.zip` | 自解压后跑 `INSTALL.bat` → PowerShell 自提权注册，无向导 |

两者装到同一个 `%ProgramFiles%\AirType`，注册表 `InprocServer32` 都固定指向该目录。

## 产物（Inno Setup EXE）

双击 `dist\AirType-Setup-0.2.0-x64-inno.exe` 即可，向导里可改安装目录。安装内容：

```
airtype_tsf.dll          TSF 文本服务外壳（安装时自动 DllRegisterServer）
airtype_ffi.dll          Rust 核心（ASR + WebSocket）
airtype_tsf_probe.exe    诊断探针（不注入、不注册）
config\airtype.toml      默认配置模板（已存在时不覆盖）
tools\simulate_voice.ps1 模拟音频工具（无需硬件/Python 验证上屏）
tools\close_status.ps1   清理残留悬浮图标工具
VERIFY.md                安装后自检指南
```

**验证方法见 `VERIFY.md`**——核心步骤：切到 AirType 输入法 → 打开记事本 →
运行 `& "$env:ProgramFiles\AirType\tools\simulate_voice.ps1"`，观察落字与悬浮图标。

## 生成安装包

```powershell
# 1. 先构建/刷新 dist 里的 DLL（Release）
powershell -ExecutionPolicy Bypass -File build_installer.ps1
#   或跳过构建，仅用现有 DLL 更新包内文件：
#   powershell -ExecutionPolicy Bypass -File build_installer.ps1 -SkipBuild

# 2. 编译 Inno Setup 安装 EXE
powershell -ExecutionPolicy Bypass -File build_inno.ps1
```

`build_inno.ps1` 需要已安装 Inno Setup 6（编译器 `ISCC.exe`）。脚本会自动到
`%LOCALAPPDATA%\Programs\Inno Setup 6` 和 Program Files 下查找。需中文界面时，
把官方 `ChineseSimplified.isl` 放进 Inno Setup 的 `Languages\` 目录（6.7.4 安装包不自带）。

Inno Setup 支持的常用参数：

```
AirType-Setup-0.2.0-x64-inno.exe /SILENT        # 静默安装
AirType-Setup-0.2.0-x64-inno.exe /VERYSILENT     # 完全无界面
AirType-Setup-0.2.0-x64-inno.exe /DIR="D:\AirType" /SILENT
```

## 安装 / 更新

双击 EXE → UAC 点"是" → 下一步即可。安装器做这些事：

1. 申请管理员权限
2. 复制 DLL / 配置 / 工具到 `%ProgramFiles%\AirType`
3. `regserver` 标志调用新 DLL 的 `DllRegisterServer`（STA 线程，与 DLL 内部 `CoInitializeEx(APARTMENTTHREADED)` 兼容），注册表 `InprocServer32` 固定指向安装目录
4. `[Code]` 段清掉旧的 en-US profile（键盘布局里只剩一个中文 AirType）
5. 写"应用和功能"卸载项，重启 `explorer.exe` 让键盘布局刷新

**关键收益**：注册路径固定。日后重建 DLL，重新跑上面两步即可覆盖，不必手动注册。

## 卸载

三种方式等价：

- 控制面板「应用和功能」→ AirType 语音输入 → 卸载
- 运行安装目录下的 `unins000.exe`
- 静默：`AirType-Setup-0.2.0-x64-inno.exe /SILENT /UNINSTALL`

卸载时 `regserver` 自动调 `DllUnregisterServer` 反注册并清 profile。

## 常见问题

- **卸载后悬浮图标还留在屏幕上**：TSF 被系统注入到所有进程，卸载只删注册表/文件，已加载 DLL 的进程不会自行关闭图标。新版安装包在卸载时会广播关闭消息；若仍残留（旧版 DLL 或权限隔离），管理员运行安装目录下的清理工具：
  ```powershell
  & "$env:ProgramFiles\AirType\tools\close_status.ps1"
  ```
  它会广播 `AirTypeTsf.Shutdown` 并按窗口类名兜底关闭。新版 DLL 只在光标定位成功时才显示图标，不会再钉在左上角。
- **"目标文件被占用"**：说明有进程仍加载着旧 TSF DLL（chrome / 微信 / explorer 等）。关掉相关程序，或注销重新登录后重试。Inno Setup 会提示要关闭的进程。
- **安装后键盘布局没刷新**：安装器默认重启 explorer；若没生效，手动重启 explorer 或重新登录。
- **Inno Setup EXE 里没有中文选项**：6.7.4 安装包不自带中文 `.isl`，需把官方 `ChineseSimplified.isl` 放到 Inno Setup 安装目录的 `Languages\` 下再重新编译。
- **注册成功但文字不落字**：确认配置项 `inject.mode` 与目标应用一致；用 `airtype_tsf_probe.exe --uia` / `--dllselftest` 诊断（见上层 README）。
- **32 位进程**：当前产物为 x64，仅注册到 64 位视图。32 位程序里不会出现 AirType 输入法。
