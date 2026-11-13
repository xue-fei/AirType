//! # airtype-ffi
//!
//! 把 Rust 核心封装成 `cdylib`，供各平台原生输入法外壳加载。
//! 契约见 `platforms/windows/tsf/include/airtype_ffi.h`。
//!
//! 职责边界：**外壳负责注入，核心负责识别**。
//! FFI 启动时强制把内置注入器设为 `none`，最终文本通过
//! `airtype_set_text_callback` 交给外壳，由外壳用 TSF / IMK / Fcitx5
//! 的编辑会话写入当前输入框。

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex, OnceLock};

use airtype_core::Config;
use airtype_daemon::{run_server, Registry, TextCallback};

const AIRTYPE_OK: i32 = 0;
const AIRTYPE_ERR_CONFIG: i32 = -2;
const AIRTYPE_ERR_STATE: i32 = -3;
const AIRTYPE_ERR_UTF8: i32 = -4;
const AIRTYPE_ERR_INTERNAL: i32 = -5;

type TextCb = extern "C" fn(*const c_char, *mut c_void);

struct Global {
    runtime: Option<tokio::runtime::Runtime>,
    config: Option<Config>,
    /// 函数指针 + userdata 的指针值（存 usize 以保持 Global: Send）
    callback: Option<(TextCb, usize)>,
    shutdown: Option<tokio::sync::oneshot::Sender<()>>,
    last_error: CString,
}

impl Default for Global {
    fn default() -> Self {
        Self {
            runtime: None,
            config: None,
            callback: None,
            shutdown: None,
            last_error: CString::new("").unwrap(),
        }
    }
}

fn global() -> &'static Mutex<Global> {
    static G: OnceLock<Mutex<Global>> = OnceLock::new();
    G.get_or_init(|| Mutex::new(Global::default()))
}

fn store_error(g: &mut Global, msg: &str) {
    g.last_error = CString::new(msg)
        .unwrap_or_else(|_| CString::new("unknown error").unwrap());
}

fn set_error(code: i32, msg: &str) -> i32 {
    if let Ok(mut g) = global().lock() {
        store_error(&mut g, msg);
    }
    code
}

#[no_mangle]
pub extern "C" fn airtype_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn airtype_init(config_path_utf8: *const c_char) -> i32 {
    let _ = tracing_subscriber::fmt::try_init();

    let config = if config_path_utf8.is_null() {
        Config::default()
    } else {
        let path = match unsafe { CStr::from_ptr(config_path_utf8) }.to_str() {
            Ok(s) => s,
            Err(e) => {
                return set_error(AIRTYPE_ERR_UTF8, &format!("config path is not UTF-8: {e}"));
            }
        };
        match Config::load(path) {
            Ok(c) => c,
            Err(e) => return set_error(AIRTYPE_ERR_CONFIG, &e.to_string()),
        }
    };

    let runtime = match tokio::runtime::Runtime::new() {
        Ok(r) => r,
        Err(e) => return set_error(AIRTYPE_ERR_INTERNAL, &e.to_string()),
    };

    let mut g = global().lock().unwrap();
    if g.runtime.is_some() {
        store_error(&mut g, "already initialized; call airtype_stop() first");
        return AIRTYPE_ERR_STATE;
    }
    g.runtime = Some(runtime);
    g.config = Some(config);
    g.shutdown = None;
    store_error(&mut g, "");
    AIRTYPE_OK
}

#[no_mangle]
pub extern "C" fn airtype_set_text_callback(
    cb: Option<TextCb>,
    userdata: *mut c_void,
) -> i32 {
    let mut g = global().lock().unwrap();
    g.callback = cb.map(|f| (f, userdata as usize));
    store_error(&mut g, "");
    AIRTYPE_OK
}

#[no_mangle]
pub extern "C" fn airtype_start() -> i32 {
    let mut g = global().lock().unwrap();
    if g.runtime.is_none() || g.config.is_none() {
        store_error(&mut g, "not initialized; call airtype_init() first");
        return AIRTYPE_ERR_STATE;
    }
    if g.shutdown.is_some() {
        store_error(&mut g, "server is already running");
        return AIRTYPE_ERR_STATE;
    }

    let mut config = g.config.clone().unwrap();
    // 外壳接管注入，禁用内置剪贴板注入，避免同一段文字写入两次。
    config.inject.mode = "none".into();

    let on_text: TextCallback = Arc::new(|text: String| {
        // 每次读取当前回调，允许运行期重新注册；绝不持锁调用宿主代码。
        let cb = global().lock().unwrap().callback;
        if let Some((f, userdata)) = cb {
            if let Ok(c) = CString::new(text) {
                f(c.as_ptr(), userdata as *mut c_void);
            }
        }
    });

    let (tx, rx) = tokio::sync::oneshot::channel::<()>();
    g.runtime.as_ref().unwrap().spawn(async move {
        if let Err(e) = run_server(config, Arc::new(Registry::new()), on_text, rx).await {
            tracing::error!("AirTypeIME server stopped: {e:#}");
        }
    });
    g.shutdown = Some(tx);
    store_error(&mut g, "");
    AIRTYPE_OK
}

#[no_mangle]
pub extern "C" fn airtype_stop() {
    let mut g = global().lock().unwrap();
    if let Some(tx) = g.shutdown.take() {
        let _ = tx.send(());
    }
    if let Some(rt) = g.runtime.take() {
        // 非阻塞关闭；外壳进程可能正在卸载 DLL。
        rt.shutdown_background();
    }
    g.config = None;
    g.callback = None;
}

#[no_mangle]
pub extern "C" fn airtype_last_error() -> *const c_char {
    global().lock().unwrap().last_error.as_ptr()
}
