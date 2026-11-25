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
; 内嵌 sherpa-onnx fp32 模型（encoder/decoder/joiner/tokens.txt/bpe.model，约 458MB）。
; 只打 fp32：resolve_model_files 明确跳过量化文件，int8 版约 209MB 不装。
; 缺模型时 engine = "sherpa" 会在输入法激活时报 model_dir 不是目录，无法识别。
Source: "..\models\sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20\*"; DestDir: "{app}\models\sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.int8.onnx"

[Code]
const
  TipKey         = 'SOFTWARE\Microsoft\CTF\TIP\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}\LanguageProfile\0x00000409';
  TipKeyWow      = 'SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}\LanguageProfile\0x00000409';
  ProfileGuid    = '{0C4CE702-4B69-4BC4-96AF-47EC0DA70E2A}';
  MOVEFILE_DELAY_UNTIL_REBOOT = $0004;
  MOVEFILE_REPLACE_EXISTING   = $0001;

// 登记「重启后删除」。用于被注入进程占用、当场删不掉的旁路文件。
function MoveFileEx(lpExistingFileName, lpNewFileName: string; dwFlags: DWORD):
  Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

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

// 重启 Windows shell（任务栏 + 桌面），不打开资源管理器窗口。
//
// 为什么不能直接 Start-Process/ShellExec explorer.exe：
//   1) 安装器以管理员（High IL）运行，直接启动的 explorer 会挂在安装器进程树上，
//      安装器一退出就把它一起回收 -> 表现为「关了任务栏却不回来」；
//   2) `start explorer.exe`（无参数）走 ShellExecute 的「打开主文件夹」语义，
//      explorer 未运行时会额外弹出一个资源管理器窗口。
//
// 所以用 cmd /c start "" explorer.exe：空引号对让 start 把它当窗口标题，命令没有
// 可打开的目标 -> cmd 退化为执行 explorer.exe 本身 = 只启动 shell 进程。
// 经 cmd 中转后 explorer 以普通用户完整性级别脱离提权进程树；explorer 单实例，
// 已运行时本命令不会再起第二个，因此无条件执行一次即幂等安全。
//
// 也不用 FindWindow/tasklist 探测任务栏：安装器是 High IL，UIPI 挡住对低完整性
// 窗口的枚举（FindWindow 恒 0）；tasklist 按 imagename 过滤在无匹配时退出码同样
// 为 0（实测判定不可靠）。探测只会误报，故去掉。
procedure RestartExplorer;
var
  Code: Integer;
begin
  Log('RestartExplorer: 结束 explorer.exe 并刷新 TSF 缓存...');
  Exec('taskkill.exe', '/f /im explorer.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);

  // 给 Windows shell 自身经 DCOM 自动拉起 explorer 的时间（原代码只等 800ms）。
  Sleep(1500);

  Log('RestartExplorer: 恢复 Windows shell（仅任务栏/桌面，不打开文件夹窗口）...');
  Exec(ExpandConstant('{cmd}'), '/c start "" explorer.exe', '', SW_HIDE, ewNoWait, Code);
  Sleep(1500);

  Log('RestartExplorer: 已发出 shell 启动请求（单实例，重复调用安全）。');
end;

// 旧 DLL 常被「已注入的进程」锁住（TSF 外壳一旦注入就常驻），Inno 覆盖时
// DeleteFile 会失败并导致整个安装回滚。Windows 允许重命名已加载的映像，
// 先把它挪走让路，旧进程继续使用内存里的副本，新文件才能写入。
//
// 踩过的坑：早期实现先把已存在的 .old 挪成 .old.<序号> 再让路，但那个旧 .old
// 往往同样被占用 / 挪不动，于是 RenameFile(Base, .old) 因目标已存在而失败
// -> Base 没让开 -> DeleteFile 失败 -> 整个安装回滚。改为不依赖「挪开旧
// .old」，直接把 Base 改名为一个全新的唯一名字（.old.<递增序号>）。
var
  MoveAsideSeq: Integer;

procedure MoveAsideOne(const Name: string);
var
  Base, Target: string;
  i: Integer;
  moved: Boolean;
begin
  Base := ExpandConstant('{app}\\' + Name);
  if not FileExists(Base) then
    Exit;

  moved := False;
  for i := 1 to 50 do
  begin
    MoveAsideSeq := MoveAsideSeq + 1;
    Target := Format('%s.old.%d', [Base, MoveAsideSeq]);
    if FileExists(Target) then
      Continue;
    if RenameFile(Base, Target) then
    begin
      Log(Format('%s 被占用，已改名 %s', [Name, ExtractFileName(Target)]));
      moved := True;
      Break;
    end;
  end;

  if not moved then
    Log(Format('警告：%s 改名失败，本次覆盖可能失败（交由 Inno 处理）', [Name]));
end;

procedure MoveAsideLockedFiles;
begin
  MoveAsideOne('airtype_tsf.dll');
  MoveAsideOne('airtype_ffi.dll');
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

// 旁路文件（MoveAsideLockedFiles 改名出来的 .old / .old.N）会在 install /
// uninstall 反复进行下越积越多（实测单目录 6 个共 60MB）。这里做一次通配清扫。
//
// 为什么在 ssPostInstall 也要清：卸载只删固定名 .old，带序号的 .old.N 从不处理；
// 而安装阶段被注入进程锁住的文件此刻也删不掉。放到 ssPostInstall，此时
// RestartExplorer 刚执行、旧 explorer 已退出，占用概率最低。
procedure CleanupAsideFiles;
var
  FindRec: TFindRec;
  Dir, SelfPath: string;
  n: Integer;
begin
  Dir := ExpandConstant('{app}');
  n := 0;
  if FindFirst(Dir + '\*.old*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          SelfPath := Dir + '\' + FindRec.Name;
          if DeleteFile(SelfPath) then
          begin
            n := n + 1;
            Log(Format('已清理旁路文件：%s', [FindRec.Name]));
          end
          else
          begin
            // 删除失败（多为仍被注入进程占用）：登记到 PendingFileRenameOperations，
            // 下次开机由系统零弹窗清除，不影响当前使用。
            MoveFileEx(SelfPath, '', MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
            Log(Format('旁路文件暂被占用，已登记为重启后删除：%s', [FindRec.Name]));
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  Log(Format('旁路文件清理完成，本次直接删除 %d 个。', [n]));
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
    // 清掉本次及历史遗留的旁路文件（删除失败 = 仍被占用，忽略即可）。
    CleanupAsideFiles;
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
    // 清理 MoveAsideLockedFiles 留下的旁路文件（.old / .old.N），
    // 换用通配匹配，避免只删固定名导致 .old.N 越积越多。
    CleanupAsideFiles;
    RestartExplorer;
  end;
end;
