# AirType TSF 安装器

一键安装 / 更新 / 卸载 AirType 文本服务，**不需要手工 `regsvr32`**。

## 产物

`dist\AirType-Setup-<版本>-x64.zip`，解压后内容：

```
INSTALL.bat              双击安装（自动弹 UAC 提权）
UNINSTALL.bat            双击卸载
install.ps1              安装/更新/卸载主脚本
airtype_tsf.dll          TSF 文本服务外壳
airtype_ffi.dll          Rust 核心（ASR + WebSocket）
airtype_tsf_probe.exe    诊断探针（不注入、不注册）
config\airtype.toml      默认配置模板
```

## 生成安装包

```powershell
# 完整：Release 构建 + 打包
powershell -ExecutionPolicy Bypass -File build_installer.ps1

# 跳过构建，用 tsf\build\bin 里现有 DLL 直接打包
powershell -ExecutionPolicy Bypass -File build_installer.ps1 -SkipBuild

# 指定版本号（只影响文件名）
powershell -ExecutionPolicy Bypass -File build_installer.ps1 -Version 1.0.0
```

## 安装 / 更新

把 zip 解压到任意目录，双击 **`INSTALL.bat`**，或：

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

脚本做这些事：

1. 申请管理员权限（UAC 弹窗点"是"）
2. 找到当前已注册的 DLL（不管它在 `build\bin` 还是别处），调用其 `DllUnregisterServer` 反注册
3. 复制新 DLL 到固定目录 `%ProgramFiles%\AirType`（注册表 `InprocServer32` 永久指向这里）
4. 调用新 DLL 的 `DllRegisterServer` 注册，顺带清掉旧的 en-US profile（键盘布局里只剩一个中文 AirType）
5. 写"应用和功能"卸载项，重启 `explorer.exe` 让键盘布局刷新

**关键收益**：注册路径固定。日后改了代码重建 DLL，只需重新打包 + 双击 `INSTALL.bat` 覆盖文件即可，不必再手动注册、也不必担心注册路径还指向旧文件。

配置文件 `config\airtype.toml` 已存在时不会被覆盖，改过的麦克风地址/引擎设置会保留。

## 卸载

双击 **`UNINSTALL.bat`**，或：

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
```

会反注册、删除安装目录、移除卸载项。"应用和功能"里也能直接卸载。

## 常见问题

- **"目标文件被占用"**：说明有进程仍加载着旧 TSF DLL（chrome / 微信 / explorer 等）。关掉相关程序，或注销重新登录后重试。
- **安装后键盘布局没刷新**：脚本默认重启 explorer；若用了 `-NoRestartExplorer`，需手动重启 explorer 或重新登录。
- **注册成功但文字不落字**：确认配置项 `inject.mode` 与目标应用一致；用 `airtype_tsf_probe.exe --uia` / `--dllselftest` 诊断（见上层 README）。
- **32 位进程**：当前产物为 x64，仅注册到 64 位视图。32 位程序里不会出现 AirType 输入法。
