// airtype_tsf.dll 入口：COM 类工厂 + 注册/注销。
//
// 注册走 Microsoft 推荐路径（ITfInputProcessorProfiles / ITfCategoryMgr），
// 不依赖 .rgs 脚本。注册会写 HKLM，需要管理员权限。

#include <windows.h>

#include <atlbase.h>
#include <msctf.h>
#include <objbase.h>

#include <new>
#include <string>

#include "Log.h"
#include "TsfGuids.h"
#include "TsfTextService.h"

namespace {

HINSTANCE g_instance = nullptr;
LONG g_objects = 0;

// 进程内只有一个文本服务实例，用静态对象简化引用计数。
class TsfClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_INVALIDARG;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        auto* service = new (std::nothrow) airtype::TsfTextService();
        if (service == nullptr) {
            return E_OUTOFMEMORY;
        }
        InterlockedIncrement(&g_objects);
        const HRESULT hr = service->QueryInterface(riid, ppv);
        service->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL /*lock*/) override { return S_OK; }
};

std::wstring ModulePath() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(g_instance, path, MAX_PATH) == 0) {
        return {};
    }
    return path;
}

std::wstring GuidString(REFGUID guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, 64);
    // StringFromGUID2 返回带花括号的形如 {XXXXXXXX-...}
    return buffer;
}

std::wstring ClsidKeyName(REFGUID guid) {
    return L"CLSID\\" + GuidString(guid);
}

bool WriteClsidKeys() {
    const std::wstring module = ModulePath();
    const std::wstring key = ClsidKeyName(CLSID_AIRTYPE_TSF);

    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
        airtype::log("注册 CLSID 失败（需要管理员权限）: %s", module.c_str());
        return false;
    }
    const WCHAR* name = L"AirType 语音输入";
    RegSetValueExW(hk, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(name),
                   static_cast<DWORD>((wcslen(name) + 1) * sizeof(WCHAR)));

    HKEY server = nullptr;
    RegCreateKeyExW(hk, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
                    nullptr, &server, nullptr);
    if (server != nullptr) {
        const std::wstring& m = module;
        RegSetValueExW(server, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(m.c_str()),
                       static_cast<DWORD>((m.size() + 1) * sizeof(WCHAR)));
        const WCHAR* apartment = L"Apartment";
        RegSetValueExW(server, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(apartment),
                       static_cast<DWORD>((wcslen(apartment) + 1) * sizeof(WCHAR)));
        RegCloseKey(server);
    }
    RegCloseKey(hk);
    return true;
}

void DeleteClsidKeys() {
    RegDeleteTreeW(HKEY_CLASSES_ROOT, ClsidKeyName(CLSID_AIRTYPE_TSF).c_str());
}

bool RegisterProfiles() {
    CComPtr<ITfInputProcessorProfiles> profiles;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&profiles));
    if (FAILED(hr)) {
        airtype::log_hr("CoCreateInstance(TF_InputProcessorProfiles)", hr);
        return false;
    }

    hr = profiles->Register(CLSID_AIRTYPE_TSF);
    if (FAILED(hr)) {
        airtype::log_hr("ITfInputProcessorProfiles::Register", hr);
        return false;
    }

    // 只注册 zh-CN 一个语言配置。
    // AirType 做的是中英双语识别，语言判断在 ASR 模型里完成，与键盘布局的
    // LANGID 无关；再注册 en-US 只会在输入法列表里多出一个无用的切换项。
    // 描述串长度按字符数传（不含结尾 NUL）。
    const WCHAR* desc = L"AirType 语音输入";
    const ULONG desc_len = static_cast<ULONG>(wcslen(desc));
    hr = profiles->AddLanguageProfile(CLSID_AIRTYPE_TSF, 0x0804, GUID_AIRTYPE_PROFILE, desc,
                                      desc_len, nullptr, 0, 0);
    if (FAILED(hr)) {
        airtype::log_hr("AddLanguageProfile(zh-CN)", hr);
        return false;
    }
    return true;
}

bool RegisterCategories() {
    CComPtr<ITfCategoryMgr> catmgr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, reinterpret_cast<void**>(&catmgr));
    if (FAILED(hr)) {
        airtype::log_hr("CoCreateInstance(TF_CategoryMgr)", hr);
        return false;
    }
    hr = catmgr->RegisterCategory(CLSID_AIRTYPE_TSF, GUID_TFCAT_TIP_KEYBOARD, CLSID_AIRTYPE_TSF);
    if (FAILED(hr)) {
        airtype::log_hr("ITfCategoryMgr::RegisterCategory", hr);
        return false;
    }
    return true;
}

void UnregisterProfiles() {
    CComPtr<ITfInputProcessorProfiles> profiles;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                                   reinterpret_cast<void**>(&profiles)))) {
        profiles->Unregister(CLSID_AIRTYPE_TSF);
    }
    CComPtr<ITfCategoryMgr> catmgr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, reinterpret_cast<void**>(&catmgr)))) {
        catmgr->UnregisterCategory(CLSID_AIRTYPE_TSF, GUID_TFCAT_TIP_KEYBOARD, CLSID_AIRTYPE_TSF);
    }
}

}  // namespace

// --- DLL 入口与导出 ---

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_instance = instance;
            DisableThreadLibraryCalls(instance);
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (rclsid != CLSID_AIRTYPE_TSF) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    static TsfClassFactory factory;
    return factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    // 外壳被 TSF 注入后生命周期由系统管理，保守起见不自动卸载。
    return g_objects == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    airtype::log("开始注册 AirType 文本服务...");
    if (!WriteClsidKeys()) {
        return E_FAIL;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return E_FAIL;
    }
    bool ok = RegisterProfiles() && RegisterCategories();
    CoUninitialize();
    if (!ok) {
        DllUnregisterServer();
        return E_FAIL;
    }
    airtype::log("注册完成。注意：TSF 会缓存文本服务列表，可能需要重启 explorer.exe 才生效。");
    return S_OK;
}

STDAPI DllUnregisterServer() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return E_FAIL;
    }
    UnregisterProfiles();
    CoUninitialize();
    DeleteClsidKeys();
    airtype::log("已注销 AirType 文本服务");
    return S_OK;
}
