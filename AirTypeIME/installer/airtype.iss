;----------------------------------------------------------------------
; AirType TSF 输入法 —— Inno Setup 6 安装脚本
;
; 替代原 7-Zip SFX + PowerShell 自提权方案，由正规安装器完成：
;   1. 自动提权（PrivilegesRequired=admin）
;   2. 安装前反注册旧路径的 airtype_tsf.dll（注册表 InprocServer32 指向的位置）
;   3. 安装 DLL / 配置 / 模拟工具到 {app}
;   4. 自动 DllRegisterServer（regserver 标志；Inno 在 STA 线程调用，
;      与 DllRegisterServer 内部的 CoInitializeEx(APARTMENTTHREADED) 兼容）
;   5. 清理旧 en-US 语言 profile（避免键盘布局里出现两个 AirType）
;   6. 卸载项由 Inno 自动生成；卸载时 regserver 自动 DllUnregisterServer
;   7. 安装/卸载后重启 explorer 刷新 TSF 文本服务列表
;
; 构建：powershell -ExecutionPolicy Bypass -File build_inno.ps1
;----------------------------------------------------------------------

#define MyAppName      "AirType 语音输入"
#define MyAppVersion   "0.2.0"
#define MyAppPublisher "AirType"
#define MyCLSID        "{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}"
#define MyProfile      "{0C4CE702-4B69-4BC4-96AF-47EC0DA70E2A}"

[Setup]
AppId={{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\AirType
DefaultGroupName={#MyAppName}
UninstallDisplayIcon={app}\airtype_tsf.dll
UninstallDisplayName={#MyAppName} {#MyAppVersion}
OutputDir=dist
OutputBaseFilename=AirType-Setup-{#MyAppVersion}-x64-inno
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64os
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern
MinVersion=10.0
DisableDirPage=yes
DisableProgramGroupPage=yes
CloseApplications=no

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; regserver: 安装时 DllRegisterServer，卸载时 DllUnregisterServer
; ignoreversion: 覆盖旧 DLL，不比对版本（源码构建没有版本资源）
Source: "dist\airtype_tsf.dll";       DestDir: "{app}"; Flags: ignoreversion regserver
Source: "dist\airtype_ffi.dll";       DestDir: "{app}"; Flags: ignoreversion
Source: "dist\airtype_tsf_probe.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "config\airtype.toml";        DestDir: "{app}\config"; Flags: onlyifdoesntexist
Source: "tools\simulate_voice.ps1";   DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\close_status.ps1";     DestDir: "{app}\tools"; Flags: ignoreversion
Source: "VERIFY.md";                  DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  TipKey         = 'SOFTWARE\Microsoft\CTF\TIP\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}\LanguageProfile\0x00000409';
  TipKeyWow      = 'SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}\LanguageProfile\0x00000409';
  ProfileGuid    = '{0C4CE702-4B69-4BC4-96AF-47EC0DA70E2A}';

// 说明：不需要在 Code 段手动反注册旧 DLL。
// [Files] 的 regserver 标志会调用新 DLL 的 DllRegisterServer，
// 它把 InprocServer32 重写到 {app} 并重新 Register/AddLanguageProfile，
// 旧路径自然不再被引用；卸载时 regserver 调 DllUnregisterServer 清掉全部 profile。

// 清理 DllUnregisterServer 可能漏掉的 en-US 残留 profile。
procedure RemoveLegacyEnProfile;
var
  Views: array[0..1] of string;
  Target: string;
  I: Integer;
begin
  Views[0] := TipKey;
  Views[1] := TipKeyWow;
  for I := 0 to 1 do
  begin
    Target := Views[I] + '\' + ProfileGuid;
    if RegKeyExists(HKEY_LOCAL_MACHINE, Target) then
    begin
      Log(Format('清理 en-US profile: %s', [Target]));
      RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, Target);
    end;
    if RegKeyExists(HKEY_LOCAL_MACHINE, Views[I]) then
      RegDeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, Views[I]);
  end;
end;

// 卸载时关闭所有进程里的悬浮图标（含旧版 DLL 残留）。
// 用打包进去的 PowerShell 工具：广播注册消息 + 按类名 WM_CLOSE 兜底。
// 必须在 {app} 文件删除之前（usUninstall）调用。
procedure CloseStatusIcons;
var
  Code: Integer;
  Tool: String;
begin
  Tool := ExpandConstant('{app}\tools\close_status.ps1');
  if FileExists(Tool) then
  begin
    Exec('powershell.exe',
         '-NoProfile -ExecutionPolicy Bypass -File "' + Tool + '" -Quiet',
         '', SW_HIDE, ewWaitUntilTerminated, Code);
    Log(Format('关闭悬浮图标工具 -> exit %d', [Code]));
  end;
end;

// TSF 会缓存文本服务列表，必须重启 explorer 才刷新。
//
// 坑：安装器以管理员（高完整性级别）运行。旧代码 taskkill 后只 Sleep(800) 就
// ShellExec('explorer.exe', ewNoWait)，新 explorer 挂在提权安装器的进程树上，
// 安装器一退出就把它带走；只剩 Windows DCOM 自愈兜底，而自愈不保证及时 ——
// 表现就是「explorer 关了却没重启」。实测日志里 explorer 启动与 Setup 退出只差
// 6ms，纯属侥幸；有一次自愈没接管，任务栏就空了。
//
// 修法（与 install.ps1 的 Restart-Explorer 一致）：
//   1) 先等 1.5s，给 Windows shell 自身经 DCOM 自动拉起 explorer 的机会；
//   2) 再经 cmd /c start（ShellExecute）启动一次，让 explorer 以普通完整性级别
//      脱离提权进程树，稳定存活；
//   3) 延时后再补一次，双保险。
//
// 不用 FindWindowByClassName 探测任务栏：安装器是 High IL、explorer 是 Medium IL，
// UIPI 会阻止提权进程枚举低完整性窗口，探测恒为 0（实测误报）。改为幂等策略：
// explorer.exe 是单实例，已运行时 start 不会再起第二个，因此重复调用安全。
procedure RestartExplorer;
var
  Code: Integer;
begin
  Log('RestartExplorer: 结束 explorer.exe 并刷新 TSF 缓存...');
  Exec('taskkill.exe', '/f /im explorer.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);

  // 步骤 1：给 Windows 自愈留时间（原代码只等 800ms）。
  Sleep(1500);

  // 步骤 2：经 cmd /c start 脱离提权进程树启动（幂等，已运行则不会再起）。
  Log('RestartExplorer: 经 cmd /c start 拉起 explorer.exe...');
  Exec(ExpandConstant('{cmd}'), '/c start explorer.exe', '', SW_HIDE, ewNoWait, Code);

  // 步骤 3：延时后补一次，防止首次 start 恰在自愈中途被忽略。
  Sleep(1500);
  Exec(ExpandConstant('{cmd}'), '/c start explorer.exe', '', SW_HIDE, ewNoWait, Code);
  Log('RestartExplorer: 已发出 explorer.exe 启动请求（单实例，重复调用安全）。');
end;

// 旧 DLL 常被「已注入的进程」锁住（TSF 外壳一旦注入就常驻），Inno 覆盖时
// DeleteFile 会失败并导致整个安装回滚。Windows 允许重命名已加载的映像，
// 先把它挪到 .old 让路，旧进程继续使用内存里的副本，新文件才能写入。
procedure MoveAsideLockedFiles;
begin
  if RenameFile(ExpandConstant('{app}\airtype_tsf.dll'),
                ExpandConstant('{app}\airtype_tsf.dll.old')) then
    Log('旧 airtype_tsf.dll 被占用，已改名 airtype_tsf.dll.old');
  if RenameFile(ExpandConstant('{app}\airtype_ffi.dll'),
                ExpandConstant('{app}\airtype_ffi.dll.old')) then
    Log('旧 airtype_ffi.dll 被占用，已改名 airtype_ffi.dll.old');
end;

// 机器级注册（regserver）不等于「对用户启用」：还要把 profile 写进当前用户的
// 输入法列表，任务栏 / Win+空格 才会出现 AirType。probe 走官方接口
// ITfInputProcessorProfileMgr::ActivateProfile / DeactivateProfile。
// 失败只记日志，不阻断安装 / 卸载（顶多任务栏里没有，不影响已装好的服务）。
procedure RunProbe(const Params: string);
var
  Code: Integer;
begin
  if not Exec(ExpandConstant('{app}\airtype_tsf_probe.exe'), Params, '', SW_HIDE,
              ewWaitUntilTerminated, Code) then
  begin
    Code := -1;
  end;
  Log(Format('probe %s -> exit %d', [Params, Code]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    RemoveLegacyEnProfile;
    MoveAsideLockedFiles;
  end;
  if CurStep = ssPostInstall then
  begin
    // DllRegisterServer 之后再清一次，防止旧 DLL 的注册残留。
    RemoveLegacyEnProfile;
    // 关键一步：启用到当前用户列表，否则任务栏看不到 AirType。
    RunProbe('--enable');
    RestartExplorer;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // 必须在 {app} 里的文件被删除之前调用。
  if CurUninstallStep = usUninstall then
  begin
    CloseStatusIcons;
    RunProbe('--disable');
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveLegacyEnProfile;
    // 清理 MoveAsideLockedFiles 留下的旁路文件（可能仍被占用，尽力而为）。
    DeleteFile(ExpandConstant('{app}\airtype_tsf.dll.old'));
    DeleteFile(ExpandConstant('{app}\airtype_ffi.dll.old'));
    RestartExplorer;
  end;
end;
