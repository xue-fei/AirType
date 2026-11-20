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
use std::sync::{Arc, Condvar, Mutex, OnceLock};

use airtype_core::Config;
use airtype_daemon::{run_server, PartialCallback, Registry, TextCallback};

const AIRTYPE_OK: i32 = 0;
const AIRTYPE_ERR_CONFIG: i32 = -2;
const AIRTYPE_ERR_STATE: i32 = -3;
const AIRTYPE_ERR_UTF8: i32 = -4;
const AIRTYPE_ERR_INTERNAL: i32 = -5;

type TextCb = extern "C" fn(*const c_char, *mut c_void);
/// `(累积文本, 需退格字符数, 增量文本, userdata)`。
/// `delete_chars` 用 i32 是为了跨语言对齐；负数按 0 处理。
type PartialCb = extern "C" fn(*const c_char, i32, *const c_char, *mut c_void);

struct Global {
    runtime: Option<tokio::runtime::Runtime>,
    config: Option<Config>,
    /// 函数指针 + userdata 的指针值（存 usize 以保持 Global: Send）
    callback: Option<(TextCb, usize)>,
    partial_callback: Option<(PartialCb, usize)>,
    shutdown: Option<tokio::sync::oneshot::Sender<()>>,
    last_error: CString,
    /// 正在调用宿主回调的数量。`airtype_stop` 必须等它归零才返回，
    /// 否则 C++ 侧可能在回调仍执行时 delete 掉 userdata 指向的对象。
    in_flight_callbacks: usize,
}

impl Default for Global {
    fn default() -> Self {
        Self {
            runtime: None,
            config: None,
            callback: None,
            partial_callback: None,
            shutdown: None,
            last_error: CString::new("").unwrap(),
            in_flight_callbacks: 0,
        }
    }
}

/// 全局锁 + 配套条件变量。回调进出时增减 `in_flight_callbacks`，
/// `airtype_stop` 在 condvar 上等到计数归零。
fn globals() -> &'static (Mutex<Global>, Condvar) {
    static G: OnceLock<(Mutex<Global>, Condvar)> = OnceLock::new();
    G.get_or_init(|| (Mutex::new(Global::default()), Condvar::new()))
}

fn global() -> &'static Mutex<Global> {
    &globals().0
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

/// 在途回调守卫：drop 时递减 `in_flight_callbacks`，归零时唤醒 `airtype_stop`。
struct CallbackGuard(bool);

impl Drop for CallbackGuard {
    fn drop(&mut self) {
        if self.0 {
            let (lock, cv) = globals();
            let mut g = lock.lock().unwrap();
            g.in_flight_callbacks -= 1;
            if g.in_flight_callbacks == 0 {
                cv.notify_all();
            }
        }
    }
}

/// 在持锁状态下读回调并登记在途计数。
/// 「读回调」与「+1」必须同一把锁内完成：否则 `airtype_stop` 可能在两者之间
/// 看到计数 0 就返回，C++ 侧 delete 掉 userdata 指向的对象后，回调才调到宿主 → UAF。
/// `airtype_stop` 注销回调与检查计数也在同一把锁内，二者配合形成闭环。
fn begin_text_callback() -> (Option<(TextCb, usize)>, CallbackGuard) {
    let mut g = global().lock().unwrap();
    let cb = g.callback;
    let entered = cb.is_some();
    if entered {
        g.in_flight_callbacks += 1;
    }
    (cb, CallbackGuard(entered))
}

fn begin_partial_callback() -> (Option<(PartialCb, usize)>, CallbackGuard) {
    let mut g = global().lock().unwrap();
    let cb = g.partial_callback;
    let entered = cb.is_some();
    if entered {
        g.in_flight_callbacks += 1;
    }
    (cb, CallbackGuard(entered))
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
pub extern "C" fn airtype_set_partial_callback(
    cb: Option<PartialCb>,
    userdata: *mut c_void,
) -> i32 {
    let mut g = global().lock().unwrap();
    g.partial_callback = cb.map(|f| (f, userdata as usize));
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
        // 取回调 + 登记在途在同一把锁内；guard 活到本次调用结束。
        // 绝不持锁调用宿主代码（宿主可能重入注册类 API）。
        let (cb, _guard) = begin_text_callback();
        if let Some((f, userdata)) = cb {
            if let Ok(c) = CString::new(text) {
                f(c.as_ptr(), userdata as *mut c_void);
            }
        }
    });

    let on_partial: PartialCallback =
        Arc::new(|text: String, delete_chars: usize, append: String| {
            let (cb, _guard) = begin_partial_callback();
            if let Some((f, userdata)) = cb {
                let delete_i32 = delete_chars.min(i32::MAX as usize) as i32;
                match (CString::new(text), CString::new(append)) {
                    (Ok(t), Ok(a)) => f(t.as_ptr(), delete_i32, a.as_ptr(), userdata as *mut c_void),
                    // 文本含 NUL 属于异常输入，宁可丢这一次 partial 也不传野指针。
                    _ => tracing::warn!("partial text contains NUL; callback skipped"),
                }
            }
        });

    let (tx, rx) = tokio::sync::oneshot::channel::<()>();
    g.runtime.as_ref().unwrap().spawn(async move {
        if let Err(e) = run_server(
            config,
            Arc::new(Registry::new()),
            on_text,
            on_partial,
            rx,
        )
        .await
        {
            tracing::error!("AirTypeIME server stopped: {e:#}");
        }
    });
    g.shutdown = Some(tx);
    store_error(&mut g, "");
    AIRTYPE_OK
}

#[no_mangle]
pub extern "C" fn airtype_stop() {
    let (lock, cv) = globals();
    let rt = {
        let mut g = lock.lock().unwrap();
        // 注销回调：之后进入的 begin_*_callback 读到 None，不再登记新的在途调用。
        g.callback = None;
        g.partial_callback = None;
        if let Some(tx) = g.shutdown.take() {
            let _ = tx.send(());
        }
        // 等所有已登记的在途回调返回。wait_while 会原子释放锁，回调侧
        // 才能拿到锁递减计数并 notify。计数归零后，宿主 userdata 不再被访问，
        // C++ 侧此时 delete service 对象才安全。
        g = cv
            .wait_while(g, |g| g.in_flight_callbacks > 0)
            .unwrap();
        g.config = None;
        g.runtime.take()
    };
    // 关 runtime 放到锁外，避免持锁等待 worker 退出时阻塞回调侧的 guard drop。
    if let Some(rt) = rt {
        rt.shutdown_background();
    }
}

#[no_mangle]
pub extern "C" fn airtype_last_error() -> *const c_char {
    global().lock().unwrap().last_error.as_ptr()
}
