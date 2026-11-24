<#
.SYNOPSIS
    向所有进程广播"关闭 AirType 悬浮图标"消息，并兜底按窗口类名强制关闭残留。

.DESCRIPTION
    TSF 外壳被系统注入到每个进程。卸载只删注册表和文件，已经加载 DLL 的进程
    收不到 Deactivate，图标窗口会变成孤儿留在屏幕上。本脚本做两件事：

      1. 广播注册消息 AirTypeTsf.Shutdown —— 新版 DLL 的 StatusWindow 收到后
         自毁窗口并置位抑制标志（不会复活）。
      2. 兜底：枚举顶层窗口，凡类名以 AirTypeTsfStatus- 开头的，直接发 WM_CLOSE。
         这能清掉旧版 DLL（不认识广播消息）留下的图标。

    两个卸载器（install.ps1 -Uninstall / Inno Setup）都会调用它；也可独立运行做清理。

.PARAMETER Quiet
    不打印逐条结果，只在出错时输出。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File close_status.ps1
    & "$env:ProgramFiles\AirType\tools\close_status.ps1" -Quiet
#>
[CmdletBinding()]
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'

function Log([string]$s) { if (-not $Quiet) { Write-Host "[AirType] $s" } }

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class AirTypeIconCleaner {
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint RegisterWindowMessage(string lpString);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
}
'@ -ErrorAction Stop

# 1) 广播注册消息（新版 DLL 会响应）
$shutdownMsg = [AirTypeIconCleaner]::RegisterWindowMessage('AirTypeTsf.Shutdown')
if ($shutdownMsg -ne 0) {
    [void][AirTypeIconCleaner]::PostMessage([IntPtr]0xffff, $shutdownMsg, [IntPtr]::Zero, [IntPtr]::Zero)
    Log "已广播 AirTypeTsf.Shutdown (msg=0x$('{0:X}' -f $shutdownMsg))"
} else {
    Log 'RegisterWindowMessage 失败，跳过广播'
}

# 2) 兜底：按类名前缀关闭残留窗口（含旧版 DLL）
$WM_CLOSE = 0x0010
$closed = 0
$callback = [AirTypeIconCleaner+EnumWindowsProc]{
    param($hwnd, $lparam)
    $sb = New-Object System.Text.StringBuilder 256
    [void][AirTypeIconCleaner]::GetClassName($hwnd, $sb, 256)
    if ($sb.ToString().StartsWith('AirTypeTsfStatus-')) {
        if ([AirTypeIconCleaner]::PostMessage($hwnd, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)) {
            $script:closed++
        }
    }
    return $true
}
[void][AirTypeIconCleaner]::EnumWindows($callback, [IntPtr]::Zero)
Log "已向 $closed 个残留图标窗口发送 WM_CLOSE"
