<#
.SYNOPSIS
    AirType TSF 文本服务安装 / 更新 / 卸载脚本（零依赖，自动提权）。

.DESCRIPTION
    取代手工 regsvr32。做四件事：
      1. 自动申请管理员权限（UAC）
      2. 反注册旧 DLL（不管它注册在哪个路径）
      3. 复制新 DLL 到固定安装目录（默认 %ProgramFiles%\AirType）
      4. 调用新 DLL 的 DllRegisterServer 完成注册，并清理旧 en-US profile

    因为注册表 InprocServer32 固定指向安装目录，日后重建 DLL 只需重跑本脚本
    覆盖文件即可，不必再手动 regsvr32，也不必改注册表。

.PARAMETER InstallDir
    安装目录，默认 "$env:ProgramFiles\AirType"。

.PARAMETER Uninstall
    卸载：反注册 + 删除安装目录 + 删除卸载项。

.PARAMETER NoRestartExplorer
    安装/卸载后不自动重启 explorer.exe（TSF 会缓存文本服务列表）。

.PARAMETER NoElevate
    内部参数，提权后的子进程自动带上，避免重复弹 UAC。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1
    powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$InstallDir = "$env:ProgramFiles\AirType",
    [switch]$Uninstall,
    [switch]$NoRestartExplorer,
    [switch]$NoElevate
)

$ErrorActionPreference = 'Stop'
$CLSID     = '{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}'
$PROFILE   = '{0C4CE702-4B69-4BC4-96AF-47EC0DA70E2A}'
$UNINSTKEY = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AirTypeTSF'

function Log([string]$s, [string]$c = 'Gray') { Write-Host "[AirType] $s" -ForegroundColor $c }
function Fail([string]$s) { Write-Host "[AirType] 错误: $s" -ForegroundColor Red; exit 1 }

# ---------- 1. 自动提权 ----------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $NoElevate) {
    $argv = @('-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File',
              ('"{0}"' -f $PSCommandPath))
    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        if ($kv.Key -eq 'NoElevate') { continue }
        $v = $kv.Value
        if ($v -is [System.Management.Automation.SwitchParameter]) {
            if ($v) { $argv += ('-{0}' -f $kv.Key) }
        } else {
            $argv += ('-{0}' -f $kv.Key); $argv += ('"{0}"' -f $v)
        }
    }
    Log '需要管理员权限，正在申请提权（请在 UAC 弹窗点“是”）...' 'Yellow'
    try {
        $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argv -PassThru -ErrorAction Stop
        $p.WaitForExit()
        exit $p.ExitCode
    } catch {
        Fail "提权失败：$($_.Exception.Message)"
    }
}
if (-not $isAdmin) { Fail '未取得管理员权限，无法写 HKLM 注册表。' }

# ---------- 2. P/Invoke：LoadLibrary + GetProcAddress(DllRegisterServer) ----------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class AirTypeRegistrar {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true, ExactSpelling = true)]
    static extern IntPtr GetProcAddress(IntPtr mod, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool FreeLibrary(IntPtr mod);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int RegServerFn();

    // DllRegisterServer 内部 CoInitializeEx(APARTMENTTHREADED)，必须在 STA 线程调用，
    // 否则返回 RPC_E_CHANGED_MODE。用显式 STA 线程，不依赖宿主 powershell 的单元模型。
    public static void Call(string dllPath, string exportName) {
        int hr = unchecked((int)0x80004005); // E_FAIL
        Exception captured = null;
        var t = new Thread(() => {
            IntPtr h = LoadLibraryEx(dllPath, IntPtr.Zero, 0);
            if (h == IntPtr.Zero) {
                captured = new Exception("LoadLibraryEx 失败 Win32=" + Marshal.GetLastWin32Error());
                return;
            }
            try {
                IntPtr p = GetProcAddress(h, exportName);
                if (p == IntPtr.Zero) { captured = new Exception("找不到导出 " + exportName); return; }
                var fn = (RegServerFn)Marshal.GetDelegateForFunctionPointer(p, typeof(RegServerFn));
                hr = fn();
            } finally { FreeLibrary(h); }
        });
        t.SetApartmentState(ApartmentState.STA);
        t.Start();
        t.Join();
        if (captured != null) throw captured;
        if (hr < 0) {
            throw new Exception(string.Format("{0} 返回 HRESULT 0x{1:X8}", exportName, hr));
        }
    }
}
'@ -ErrorAction Stop

function Invoke-Registrar([string]$Dll, [string]$Export) {
    if (-not (Test-Path -LiteralPath $Dll)) { Fail "DLL 不存在：$Dll" }
    Log ("调用 {0}：{1}" -f $Export, $Dll)
    try { [AirTypeRegistrar]::Call($Dll, $Export) }
    catch { Fail ("{0} 失败：$($_.Exception.Message)" -f $Export) }
}

function Get-RegisteredDll {
    $key = "HKLM:\SOFTWARE\Classes\CLSID\$CLSID\InprocServer32"
    if (Test-Path -LiteralPath $key) {
        return (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).'(default)'
    }
    return $null
}

function Test-FileLocked([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $fs.Close(); $fs.Dispose(); return $false
    } catch { return $true }
}

function Remove-LegacyEnProfile {
    # DllUnregisterServer 会 Unregister(CLSID) 清掉所有 profile，这里是兜底：
    # 若旧 DLL 已被删除导致没法反注册，仍能手工清掉 en-US 残留。
    $views = @(
        "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$CLSID\LanguageProfile\0x00000409",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\$CLSID\LanguageProfile\0x00000409"
    )
    foreach ($v in $views) {
        $target = Join-Path $v $PROFILE
        if (Test-Path -LiteralPath $target) {
            try {
                Remove-Item -LiteralPath $target -Recurse -Force
                Log "已清除 en-US 残留 profile：$target" 'Yellow'
            } catch { Log "清除 en-US profile 失败（可忽略）：$($_.Exception.Message)" 'DarkYellow' }
        }
        if ((Test-Path -LiteralPath $v) -and -not (Get-ChildItem -LiteralPath $v -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $v -Force -ErrorAction SilentlyContinue
        }
    }
}

function Restart-Explorer {
    Log '重启 explorer.exe 以刷新 TSF 文本服务列表...' 'Yellow'

    # 本脚本以管理员（高完整性级别）运行，有两个坑要避开：
    #
    #   1) 直接 Start-Process explorer.exe：新进程继承提权上下文、挂在 ps 进程树 /
    #      作业对象下，命令结束时容易被一并回收 -> 「任务栏关了却不回来」。
    #   2) start explorer.exe（无参数）：走 ShellExecute 的「打开主文件夹」语义，
    #      explorer 未运行时会额外弹出一个资源管理器窗口（用户反馈的现象）。
    #
    # 所以用 cmd /c start "" explorer.exe：空引号对让 start 把它当窗口标题，命令
    # 没有可打开的目标 -> cmd 退化为执行 explorer.exe 本身 = 只启动 shell 进程
    # （任务栏/桌面），不开文件夹窗口；经 cmd 中转后 explorer 以普通用户完整性
    # 级别脱离提权进程树。explorer 单实例，已运行时本命令不会再起第二个，幂等安全。
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 1500

    Start-Process -FilePath "$env:WINDIR\System32\cmd.exe" `
                  -ArgumentList '/c start "" explorer.exe' `
                  -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500

    if (Get-Process -Name explorer -ErrorAction SilentlyContinue) {
        Log 'explorer.exe 已重启（仅恢复任务栏/桌面）。' 'Green'
    } else {
        Log 'explorer.exe 未自动恢复，请在任务管理器手动运行 explorer.exe（不影响输入法注册）。' 'DarkYellow'
    }
}

function Remove-InstallDirFiles([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    Get-ChildItem -LiteralPath $Dir -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        if (Test-FileLocked $_.FullName) {
            Log "文件被占用，跳过：$($_.FullName)" 'DarkYellow'
        } else {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
        }
    }
    Get-ChildItem -LiteralPath $Dir -Recurse -Directory -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue
        }
    if (-not (Get-ChildItem -LiteralPath $Dir -Force -ErrorAction SilentlyContinue)) {
        Remove-Item -LiteralPath $Dir -Force -ErrorAction SilentlyContinue
    }
}

# ---------- 3. 卸载 ----------
if ($Uninstall) {
    Log '开始卸载 AirType TSF...' 'Cyan'

    # 先广播关闭所有进程里的悬浮图标：TSF 注入到每个进程，反注册/删 DLL
    # 不会通知已加载的进程 Deactivate，图标会变成孤儿留在屏幕上。
    $closeTool = Join-Path $PSScriptRoot 'tools\close_status.ps1'
    if (Test-Path -LiteralPath $closeTool) {
        try { & $closeTool -Quiet; Log '已通知各进程关闭悬浮图标。' 'Green' }
        catch { Log "关闭悬浮图标失败（可忽略）：$($_.Exception.Message)" 'DarkYellow' }
    }

    $registered = Get-RegisteredDll
    $targetDll = if ($registered -and (Test-Path -LiteralPath $registered)) { $registered }
                 else { Join-Path $InstallDir 'airtype_tsf.dll' }
    if (Test-Path -LiteralPath $targetDll) { Invoke-Registrar $targetDll 'DllUnregisterServer' }
    else { Log '未找到已注册的 DLL，跳过反注册。' 'DarkYellow' }
    Remove-LegacyEnProfile
    Remove-InstallDirFiles $InstallDir
    if (Test-Path -LiteralPath $UNINSTKEY) { Remove-Item -LiteralPath $UNINSTKEY -Recurse -Force }
    # 若当初是 Inno 安装包装的，卸载项是 {CLSID}_is1（GUID 键），不是上面的
    # AirTypeTSF。Remove-InstallDirFiles 会删掉 unins000.exe 却不认这个键，
    # 于是「应用和功能」里留下指向空路径的僵尸项。这里一并清掉。
    $innoKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\${CLSID}_is1"
    if (Test-Path -LiteralPath $innoKey) {
        try {
            Remove-Item -LiteralPath $innoKey -Recurse -Force
            Log '已清除 Inno 卸载项（{CLSID}_is1）。' 'Green'
        } catch { Log "清除 Inno 卸载项失败（可忽略）：$($_.Exception.Message)" 'DarkYellow' }
    }
    Log '卸载完成。' 'Green'
    if (-not $NoRestartExplorer) { Restart-Explorer }
    exit 0
}

# ---------- 4. 安装 / 更新 ----------
$Source = $PSScriptRoot
$srcTsf   = Join-Path $Source 'airtype_tsf.dll'
$srcFfi   = Join-Path $Source 'airtype_ffi.dll'
$srcProbe = Join-Path $Source 'airtype_tsf_probe.exe'
$srcCfg   = Join-Path $Source 'config\airtype.toml'
$srcTools = Join-Path $Source 'tools'
$srcVerify = Join-Path $Source 'VERIFY.md'

foreach ($f in @($srcTsf, $srcFfi, $srcProbe)) {
    if (-not (Test-Path -LiteralPath $f)) {
        Fail "安装包不完整，缺少 $(Split-Path -Leaf $f)。请先运行 build_installer.ps1 生成安装包。"
    }
}

$dstTsf   = Join-Path $InstallDir 'airtype_tsf.dll'
$dstFfi   = Join-Path $InstallDir 'airtype_ffi.dll'
$dstProbe = Join-Path $InstallDir 'airtype_tsf_probe.exe'
$dstCfg   = Join-Path $InstallDir 'config\airtype.toml'

Log "安装目录：$InstallDir" 'Cyan'

# 反注册旧 DLL：无论它注册在哪个路径（旧的 build\bin 或旧安装目录）
$registered = Get-RegisteredDll
if ($registered -and (Test-Path -LiteralPath $registered)) {
    if (-not (Test-FileLocked $registered)) {
        Invoke-Registrar $registered 'DllUnregisterServer'
    } else {
        Log "旧 DLL 被占用，改由注册表直接清理：$registered" 'DarkYellow'
        # 占用时无法 LoadLibrary 调反注册，靠 Remove-LegacyEnProfile + 后续 Register 覆盖。
    }
} elseif ($registered) {
    Log "注册路径指向不存在的文件，跳过反注册：$registered" 'DarkYellow'
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir 'config') | Out-Null

# 检查目标文件占用：TSF 已注入的进程会锁住 DLL，此时必须让用户关掉相关程序
foreach ($f in @($dstTsf, $dstFfi)) {
    if (Test-FileLocked $f) {
        Fail ("目标文件被占用：{0}`n请先关闭正在使用 AirType 输入法的程序（或在任务管理器结束相关进程）后重试。" -f $f)
    }
}

Copy-Item -LiteralPath $srcTsf   -Destination $dstTsf   -Force
Copy-Item -LiteralPath $srcFfi   -Destination $dstFfi   -Force
Copy-Item -LiteralPath $srcProbe -Destination $dstProbe -Force
if (Test-Path -LiteralPath $srcCfg) {
    if (-not (Test-Path -LiteralPath $dstCfg)) {
        Copy-Item -LiteralPath $srcCfg -Destination $dstCfg -Force
        Log '已写入默认配置 config\airtype.toml' 'Green'
    } else {
        Log '保留现有 config\airtype.toml（未覆盖）' 'DarkYellow'
    }
}
# 模拟验证工具 + 验证说明也装进去，用户装完无需硬件即可自测
if (Test-Path -LiteralPath $srcTools) {
    Copy-Item -LiteralPath $srcTools -Destination (Join-Path $InstallDir 'tools') -Recurse -Force
}
if (Test-Path -LiteralPath $srcVerify) {
    Copy-Item -LiteralPath $srcVerify -Destination (Join-Path $InstallDir 'VERIFY.md') -Force
}
# 把安装器自身也放进安装目录，供日后更新 / 从“应用和功能”卸载
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $InstallDir 'install.ps1') -Force

Invoke-Registrar $dstTsf 'DllRegisterServer'
Remove-LegacyEnProfile

# 卸载项：出现在 Windows“应用和功能”里
New-Item -Path $UNINSTKEY -Force | Out-Null
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'DisplayName'    -Value 'AirType 语音输入 (TSF)'
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'DisplayVersion' -Value '0.1.0'
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'Publisher'      -Value 'AirType'
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'InstallLocation'-Value $InstallDir
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'NoModify'       -Value 1 -Type DWord
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'NoRepair'       -Value 1 -Type DWord
$uninstallCmd = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}\install.ps1" -Uninstall' -f $InstallDir
Set-ItemProperty -LiteralPath $UNINSTKEY -Name 'UninstallString' -Value $uninstallCmd

Log '安装完成：' 'Green'
Log ("  注册路径  : " + (Get-RegisteredDll)) 'Green'
Log ("  配置文件  : " + $dstCfg) 'Green'
Log '  如需改麦克风地址/引擎，编辑上面的 config\airtype.toml' 'Gray'

if (-not $NoRestartExplorer) { Restart-Explorer }
else { Log '已跳过 explorer 重启；键盘布局可能要重启 explorer 或重新登录才刷新。' 'DarkYellow' }
