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

; ---- 版本目录隔离 ----
; 为什么每次安装用一个独一无二的目录：TSF 外壳是 in-proc DLL，被注入
; explorer / 微信 / chrome 后常驻，文件句柄一直被持有 —— 原地覆盖必然失败
; （DeleteFile 拿不到），只能改名成 .old 绕过，用户得重启宿主才能用上新版。
; 搜狗/QQ 输入法正是靠「装到新版本目录 + 改注册表指针」做到免重启：
; 老进程继续用老目录的文件（没被碰过），新进程加载新目录，互不干扰。
;
; BuildStamp 由 build_inno.ps1 在编译时用 /DBuildStamp=yyyyMMdd-HHmm 传入，
; 保证同版本号反复构建也能落到不同目录（否则又会退化成原地覆盖）。
#ifndef BuildStamp
  #define BuildStamp "dev"
#endif
; 注意用表达式拼接（MyAppVersion + "-" + BuildStamp），不能写成
; "{#MyAppVersion}-{#BuildStamp}"——ISPP 的 #define 不会递归展开值里的 {#...}，
; 那串会原样残留，被 Inno 当成运行时常量报 "Unknown constant #MyAppVersion"。
#define VerDirName     MyAppVersion + "-" + BuildStamp

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
; 可执行产物装进 versions\<版本>-<构建戳>\：每次都是全新目录，从不覆盖
; 已有文件，所以不会被宿主进程锁住、也不需要让路改名成 .old。
; regserver 的标志仍然有效——DllRegisterServer 用 GetModuleFileName 取自身
; 路径，注册表 InprocServer32 会自动指向新版本目录，老进程不受影响。
Source: "dist\airtype_tsf.dll";       DestDir: "{app}\versions\{#VerDirName}"; Flags: ignoreversion regserver
; ffi 单独放 lib、所有版本共用：它 20MB，若跟着版本目录走，每个删不掉的历史
; 版本都会白占 20MB。外壳按 {app}\lib 优先、同目录兜底查找（见 FfiBridge.cpp）。
;
; 文件名带构建戳 = 热重载的基础：宿主进程按需加载 DLL 后镜像常驻，替换同名文件
; 不会影响它（Windows 不回首已加载的映像）。换文件名后，新进程/重载时能拿到新文件，
; 旧文件继续被老进程持有直到退出。ffi-current.txt 告诉外壳"现在该用哪个"。
; 外壳在输入法激活时比对戳，变化就 Unload + Load —— 升级后切一次输入法即生效。
Source: "dist\airtype_ffi_{#BuildStamp}.dll"; DestDir: "{app}\lib"
Source: "dist\ffi-current.txt";               DestDir: "{app}\lib"; Flags: ignoreversion
Source: "dist\airtype_tsf_probe.exe"; DestDir: "{app}\versions\{#VerDirName}"; Flags: ignoreversion
; 配置与工具不随版本走（用户改过的配置要保留）。
Source: "config\airtype.toml";        DestDir: "{app}\config"; Flags: onlyifdoesntexist
; 热词表：用户会自己编辑，所以只在不存在时投放，升级不覆盖。
Source: "config\hotwords.txt";        DestDir: "{app}\config"; Flags: onlyifdoesntexist
Source: "tools\simulate_voice.ps1";   DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\close_status.ps1";     DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\enable_streaming.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\configure_lan.ps1";   DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\enable_denoise.ps1";   DestDir: "{app}\tools"; Flags: ignoreversion
Source: "tools\enable_hotwords.ps1";  DestDir: "{app}\tools"; Flags: ignoreversion
Source: "VERIFY.md";                  DestDir: "{app}"; Flags: ignoreversion
; 内嵌 sherpa-onnx fp32 模型（encoder/decoder/joiner/tokens.txt/bpe.model，约 458MB）。
; 只打 fp32：resolve_model_files 明确跳过量化文件，int8 版约 209MB 不装。
; 缺模型时 engine = "sherpa" 会在输入法激活时报 model_dir 不是目录，无法识别。
; 降噪模型（9.8MB）+ onnxruntime 动态库（19MB）。
; 运行库放 lib 与 ffi 同级，ort 通过 [denoise] runtime 显式加载。
Source: "..\models\dpdfnet\*"; DestDir: "{app}\models\dpdfnet"; Flags: ignoreversion recursesubdirs createallsubdirs
; 这里**故意不加 ignoreversion**：onnxruntime.dll 只有升级 ORT 时才变，
; 时间戳相同 Inno 就直接跳过，不去动被宿主进程锁着的文件（曾因此整包回滚、
; 连带把 CLSID 注册一起删掉）。真需要更新时由 MoveAsideLockedFiles 让路。
Source: "..\libs\onnxruntime.dll"; DestDir: "{app}\lib"
; onlyifdoesntexist：模型 458MB 且内容不变，已存在就跳过，重复安装省几分钟。
Source: "..\models\sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20\*"; DestDir: "{app}\models\sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20"; Flags: ignoreversion onlyifdoesntexist recursesubdirs createallsubdirs; Excludes: "*.int8.onnx"

[UninstallDelete]
; 旧版本目录不在 [Files] 里，Inno 不会自动删，必须显式声明。
Type: filesandordirs; Name: "{app}\versions"
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\models"
Type: filesandordirs; Name: "{app}\config"

[Run]
; 老配置里 [streaming] enabled 写死 false，而 config 是 onlyifdoesntexist（不覆盖
; 用户修改），所以升级后仍是"松键后一次性出字"。这一步把它就地升为 true，
; 脚本自身幂等（改完留标记，之后不再覆盖用户手动关闭的选择）。
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\enable_streaming.ps1"""; Flags: runhidden waituntilterminated; StatusMsg: "升级流式识别配置..."

; 老配置缺 [denoise] 段，补上（已有则不动，所以用户手动关掉降噪不会被重新打开）
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\enable_denoise.ps1"""; Flags: runhidden waituntilterminated; StatusMsg: "启用流式降噪..."
; 老配置的 [sherpa] 段缺热词两行，补上（已有则不动）
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\enable_hotwords.ps1"""; Flags: runhidden waituntilterminated; StatusMsg: "配置热词表..."


[Code]

// 生成固定名 airtype_ffi.dll 的兼容副本。
//
// 为什么需要：热重载是新版外壳（160823 起）才有的逻辑。升级时，已经跑着的宿主
// 进程仍持有**旧外壳**，它只会按固定名 airtype_ffi.dll 找 ffi。而安装过程会把这个
// 固定名文件让路删掉，旧外壳一旦 Deactivate→Activate 就会找不到 —— 表现为输入法
// 突然失效。复制一份副本兜住这个过渡窗口。
//
// 用复制而不是 [Files] 双份投放：安装包体积不翻倍。失败也只是少个回退文件，
// 不影响新外壳（它走 ffi-current.txt）。
procedure MakeFfiCompatCopy();
var
  Src, Dst: String;
begin
  Src := ExpandConstant('{app}\lib\airtype_ffi_{#BuildStamp}.dll');
  Dst := ExpandConstant('{app}\lib\airtype_ffi.dll');
  if not FileExists(Src) then
    Exit;
  if FileExists(Dst) then
    Exit;   // 已存在就不动（可能是老版本留下的）
  if CopyFile(Src, Dst, False) then
    Log('已生成兼容副本 airtype_ffi.dll')
  else
    Log('兼容副本生成失败（可能被占用），跳过');
end;


// 删掉 lib 下**不是当前戳**的 airtype_ffi_*.dll。
// 每个 20MB，不清理会随版本堆积（旧文件常被在跑的宿主进程锁着，删不掉就算了，
// 下次安装再试）。当前戳必须保留。
procedure CleanOldFfiFiles();
var
  FindRec: TFindRec;
  Dir, Keep: String;
begin
  Dir := ExpandConstant('{app}\lib\');
  Keep := 'airtype_ffi_' + ExpandConstant('{#BuildStamp}') + '.dll';
  if FindFirst(Dir + 'airtype_ffi_*.dll', FindRec) then
  try
    repeat
      if not SameText(FindRec.Name, Keep) then
      begin
        if DeleteFile(Dir + FindRec.Name) then
          Log('清理旧 ffi：' + FindRec.Name)
        else
          Log('旧 ffi 被占用，跳过：' + FindRec.Name);
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

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
  // 旧「根目录平铺布局」的两个文件（历史残留）
  MoveAsideOne('airtype_tsf.dll');
  MoveAsideOne('airtype_ffi.dll');
  // 共享核心：被任何宿主进程加载着就覆盖不了，先改名让路。
  // 它只有一份，所以最多留一个 .old（下面 PurgeStaleFiles 会扫 lib 目录移走）。
  MoveAsideOne('lib\airtype_ffi.dll');
  // 降噪运行库同理：启用 [denoise] 的宿主进程（chrome/微信…）都会锁住它。
  // 不加这一行时，覆盖失败会让整包回滚，并连带 DllUnregisterServer 清掉 CLSID。
  MoveAsideOne('lib\onnxruntime.dll');
end;

// 从旧版「根目录平铺布局」升级上来时，清掉根目录那三个可执行产物。
// 它们已被 versions\ 取代；tsf/ffi 若仍被老进程占用，MoveAsideLockedFiles
// 已经把改名成 .old、随后的 PurgeStaleFiles 会收拾，这里主要处理探针。
procedure PurgeLegacyRootFiles;
var
  Dir: string;
  Names: array[0..2] of string;
  I: Integer;
begin
  Dir := ExpandConstant('{app}');
  Names[0] := 'airtype_tsf.dll';
  Names[1] := 'airtype_ffi.dll';
  Names[2] := 'airtype_tsf_probe.exe';
  for I := 0 to 2 do
  begin
    if FileExists(Dir + '\' + Names[I]) then
    begin
      if DeleteFile(Dir + '\' + Names[I]) then
        Log(Format('已清理旧布局文件：%s', [Names[I]]))
      else
        Log(Format('旧布局文件暂被占用：%s', [Names[I]]));
    end;
  end;
end;

// 机器级注册（regserver）不等于「对用户启用」：还要把 profile 写进当前用户的
// 输入法列表，任务栏 / Win+空格 才会出现 AirType。probe 走官方接口
// ITfInputProcessorProfileMgr::ActivateProfile / DeactivateProfile。
// 失败只记日志，不阻断安装 / 卸载（顶多任务栏里没有，不影响已装好的服务）。
// 探针现在装在版本目录里。安装阶段直接按本次的版本目录名拼；
// 卸载阶段那个名字可能已经对不上（卸载器不知道自己属于哪个版本目录），
// 所以退一步读注册表 InprocServer32 —— 它永远指向当前生效的那份 DLL。
function ProbePath(): string;
var
  P, DllPath: string;
begin
  P := ExpandConstant('{app}\versions\{#VerDirName}\airtype_tsf_probe.exe');
  if FileExists(P) then
  begin
    Result := P;
    Exit;
  end;
  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
       'SOFTWARE\Classes\CLSID\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}\InprocServer32',
       '', DllPath) then
  begin
    P := ExtractFilePath(DllPath) + 'airtype_tsf_probe.exe';
    if FileExists(P) then
    begin
      Result := P;
      Exit;
    end;
  end;
  Result := '';
end;

procedure RunProbe(const Params: string);
var
  Code: Integer;
  Probe: string;
begin
  Probe := ProbePath();
  if Probe = '' then
  begin
    Log(Format('probe %s -> 跳过（找不到 airtype_tsf_probe.exe）', [Params]));
    Exit;
  end;
  if not Exec(Probe, Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
  begin
    Code := -1;
  end;
  Log(Format('probe %s -> exit %d', [Params, Code]));
end;

// 清理安装目录里的历史残留：*.old*（让路副本）、is-*.tmp（Inno 中断残留）。
//
// ⚠️ 旧实现的根本缺陷：删不掉时就「留在原地 + 登记重启删除」。文件仍在
//    {app} 里，而用户动辄一两周不重启（本机实测连续运行 148 小时），于是
//    .old/.old.N 常年堆在安装目录、每次升级再递增一个编号——用户看到的
//    「一堆 .old」就是这么来的。别的输入法要么走系统更新（重启替换），
//    要么在安装前强制结束宿主，不会把垃圾留在安装目录。
//
// 新策略：删不掉（被已注入的进程占用）就**同卷改名搬到系统临时目录**。
// Windows 允许重命名已加载的映像文件（MoveAsideLockedFiles 本身就靠这个
// 让路），所以这一步几乎总能成功；安装目录当场干净，被占用的副本留在
// {tmp} 由系统或下次重启回收。只有连改名都失败才退回「仅登记重启删除」。
procedure PurgePattern(const Dir, Pattern: string; var nDel, nMoved, nReg: Integer);
var
  FindRec: TFindRec;
  Src, Dst: string;
begin
  if not FindFirst(Dir + '\' + Pattern, FindRec) then
    Exit;
  try
    repeat
      if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
      begin
        Src := Dir + '\' + FindRec.Name;
        if DeleteFile(Src) then
        begin
          Inc(nDel);
        end
        else
        begin
          Dst := ExpandConstant('{tmp}') + '\stale_' + FindRec.Name;
          if FileExists(Dst) then
            DeleteFile(Dst);
          if RenameFile(Src, Dst) then
          begin
            Inc(nMoved);
            // 副本留在临时目录也会被占，登记重启删除兜底。
            MoveFileEx(Dst, '', MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
            Log(Format('  移出安装目录：%s -> {tmp}', [FindRec.Name]));
          end
          else
          begin
            Inc(nReg);
            MoveFileEx(Src, '', MOVEFILE_DELAY_UNTIL_REBOOT or MOVEFILE_REPLACE_EXISTING);
            Log(Format('  改名也失败，仅登记重启删除：%s', [FindRec.Name]));
          end;
        end;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

// 清理孤儿卸载器：Inno 升级安装有时无法复用旧的 unins000.exe（当时被占用），
// 会新建 unins001.exe 并把注册表指向它，旧的那份就永远留成 4.4MB 垃圾。
// 只在「注册表确实指向另一个卸载器」时删旧的，避免误删当前卸载器。
procedure PurgeOrphanUninstallers;
var
  Dir, CurName, CurDat, Uninst, Cand: string;
  FindRec: TFindRec;
  n: Integer;
begin
  Dir := ExpandConstant('{app}');
  CurName := '';
  if RegQueryStringValue(HKEY_LOCAL_MACHINE,
       'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{2A953F76-1683-47D8-BCA4-A4C9B3171A1F}_is1',
       'UninstallString', Uninst) then
  begin
    Uninst := RemoveQuotes(Uninst);
    CurName := ExtractFileName(Uninst);
  end;

  if CurName = '' then
  begin
    Log('孤儿卸载器清理：读不到当前 UninstallString，跳过');
    Exit;
  end;

  // 当前卸载器有 .exe 和 .dat 两个文件，都在保护名单里。
  CurDat := ChangeFileExt(CurName, '.dat');

  n := 0;
  if FindFirst(Dir + '\unins*.*', FindRec) then
  begin
    try
      repeat
        Cand := FindRec.Name;
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          if (CompareText(Cand, CurName) <> 0) and (CompareText(Cand, CurDat) <> 0) then
          begin
            if DeleteFile(Dir + '\' + Cand) then
            begin
              Inc(n);
              Log(Format('  删除孤儿卸载器文件：%s', [Cand]));
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
  Log(Format('孤儿卸载器清理：删除 %d 个（当前 = %s）', [n, CurName]));
end;

procedure PurgeStaleFiles(const Phase: string);
var
  Dir: string;
  nDel, nMoved, nReg: Integer;
begin
  Dir := ExpandConstant('{app}');
  nDel := 0;
  nMoved := 0;
  nReg := 0;
  PurgePattern(Dir, '*.old*', nDel, nMoved, nReg);
  PurgePattern(Dir, 'is-*.tmp', nDel, nMoved, nReg);
  // lib 目录下是 20MB 的共享 ffi，让路产生的 .old 必须一并移走，否则白占空间。
  PurgePattern(ExpandConstant('{app}\lib'), '*.old*', nDel, nMoved, nReg);
  if Phase = '安装后' then
    PurgeOrphanUninstallers;
  // 注意：Format 的参数数组不能换行到行首——.iss 解析器会把行首的 '[' 误判为
  // section 标记（"Invalid section tag"）。必须与 Format 同行。
  Log(Format('[%s] 残留清理：删除 %d，移出安装目录 %d，仅登记重启 %d', [Phase, nDel, nMoved, nReg]));
end;

// 版本目录只留「当前 + 最近一个」共 2 个。
//
// 为什么留一个旧的：升级时老进程（微信/Chrome）还映射着上一个版本的文件，
// 那份必须留着让它继续跑；等它们都退出后，下一次安装再清掉。
//
// 删不掉（仍被占用）就原样保留，下次再试——**绝不登记重启删除**，
// 那正是 .old 堆积的老毛病（排队几个月都执行不了）。
procedure PruneOldVersions;
var
  FindRec: TFindRec;
  Root, CurName, Tmp: string;
  Names: array[0..63] of string;
  Count, I, J, Removed: Integer;
begin
  Root := ExpandConstant('{app}\versions');
  CurName := '{#VerDirName}';
  Count := 0;
  if FindFirst(Root + '\*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if (CompareText(FindRec.Name, CurName) <> 0) and
             (FindRec.Name <> '.') and (FindRec.Name <> '..') then
          begin
            if Count <= High(Names) then
            begin
              Names[Count] := FindRec.Name;
              Count := Count + 1;
            end;
          end;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;

  // 目录名 = 版本号 + 构建时间戳，同一版本内字典序即时间序；冒泡降序。
  for I := 0 to Count - 2 do
    for J := 0 to Count - 2 - I do
      if CompareText(Names[J], Names[J + 1]) < 0 then
      begin
        Tmp := Names[J];
        Names[J] := Names[J + 1];
        Names[J + 1] := Tmp;
      end;

  // Names[0] 是最新的旧版本，留着给未重启的进程用；从索引 1 开始删。
  Removed := 0;
  for I := 1 to Count - 1 do
  begin
    if DelTree(Root + '\' + Names[I], True, True, True) then
    begin
      Inc(Removed);
      Log(Format('已删除旧版本目录：%s', [Names[I]]));
    end
    else
      Log(Format('旧版本目录仍被占用，保留到下次：%s', [Names[I]]));
  end;
  Log(Format('版本目录清理：非当前 %d 个，删除 %d 个，保留 1 个', [Count, Removed]));
end;

// 局域网直连开关：只有安装时显式带 /LAN=1 才把 [server] bind 改成 0.0.0.0。
//
// 为什么做成参数而不是默认：0.0.0.0 意味着同网段任何机器都能连上语音接收端口
// （无鉴权），不该作为默认安装态。AirMic 走 WiFi 直连 PC 时才需要。
//   Setup.exe /SILENT /LAN=1
procedure ApplyLanBind;
var
  Code: Integer;
  Tool: string;
begin
  if ExpandConstant('{param:LAN|0}') <> '1' then
    Exit;
  Tool := ExpandConstant('{app}\tools\configure_lan.ps1');
  if not FileExists(Tool) then
  begin
    Log('局域网配置脚本不存在，跳过');
    Exit;
  end;
  Exec('powershell.exe',
       '-NoProfile -ExecutionPolicy Bypass -File "' + Tool + '"',
       '', SW_HIDE, ewWaitUntilTerminated, Code);
  Log(Format('局域网直连配置 -> exit %d', [Code]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    RemoveLegacyEnProfile;
    // 先清历史残留再让路：否则 MoveAsideOne 会在旧 .old.N 之上继续递增编号，
    // 目录里的垃圾只增不减（这曾经是本机堆出 60MB 旁路文件的直接原因）。
    PurgeStaleFiles('安装前');
    // 版本目录隔离后不再覆盖任何在用文件，这里只用于「从旧版根目录布局
    // 升级上来」时清掉根目录那对旧 DLL —— 新布局下两个文件都不存在，空操作。
    MoveAsideLockedFiles;
    // 让路之后再清根目录残留（probe 从没被加载过，能直接删）。
    PurgeLegacyRootFiles;
  end;
  if CurStep = ssPostInstall then
  begin
    // DllRegisterServer 之后再清一次，防止旧 DLL 的注册残留。
    RemoveLegacyEnProfile;
    // 关键一步：启用到当前用户列表，否则任务栏看不到 AirType。
    RunProbe('--enable');
    RestartExplorer;
    // 清掉本次/历史遗留的旁路副本。
    PurgeStaleFiles('安装后');
    // 保留最近 2 个版本（当前 + 上一个），更老的删掉。
    PruneOldVersions;
    // 给还没更新的旧外壳留个固定名回退副本。
    MakeFfiCompatCopy;
    // lib 下只留当前戳的 ffi，历史版本删掉（被占用就跳过）。
    CleanOldFfiFiles;
    // 只有显式 /LAN=1 才把监听改成 0.0.0.0。
    ApplyLanBind;
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
    // 清理 MoveAsideLockedFiles 留下的旁路副本（.old / .old.N），
    // 删不掉的一律移出安装目录，不在 {app} 里留垃圾。
    PurgeStaleFiles('卸载后');
    RestartExplorer;
  end;
end;
