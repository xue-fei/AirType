#ifndef AIRTYPE_FFI_H
#define AIRTYPE_FFI_H

/*
 * AirTypeIME 共享核心的 C ABI。
 *
 * 供各平台原生输入法外壳调用：
 *   Windows  : C++ / TSF in-proc COM DLL
 *   macOS    : Objective-C++ / InputMethodKit
 *   Linux    : C++ / Fcitx5 或 IBus addon
 *
 * 外壳只负责"被系统识别为输入法"和编辑会话，
 * WebSocket 收流、ASR、文本后处理全部在 Rust 核心里。
 *
 * ABI 约定：所有字符串均为 UTF-8、NUL 结尾；返回的指针由库持有，
 * 调用方不得释放，且在下次调用或 airtype_stop() 后可能失效。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(AIRTYPE_FFI_EXPORTS)
#  define AIRTYPE_API __declspec(dllexport)
#elif defined(_WIN32)
#  define AIRTYPE_API __declspec(dllimport)
#else
#  define AIRTYPE_API __attribute__((visibility("default")))
#endif

#define AIRTYPE_OK          0
#define AIRTYPE_ERR_NULL   (-1)
#define AIRTYPE_ERR_CONFIG (-2)
#define AIRTYPE_ERR_STATE  (-3)
#define AIRTYPE_ERR_UTF8   (-4)
#define AIRTYPE_ERR_INTERNAL (-5)

/* 识别完成、完成文本后处理之后的回调。cb 在库内部线程上触发，
 * 外壳需自行 marshal 到 UI/编辑会话线程，不要在此回调里做耗时操作。 */
typedef void (*airtype_text_cb)(const char *utf8_text, void *userdata);

/* 流式识别的中间结果回调（边说边出字）。每次 partial 触发一次。
 *
 *   utf8_text    : 当前累积文本，即「此刻输入框里应该显示什么」
 *   delete_chars : 相对上一次上屏文本需要退格的字符数（Unicode 字符，非字节）
 *   append       : 退格之后需要追加的增量文本
 *
 * 外壳可以只取 utf8_text 做整体替换，也可以按 delete_chars/append 做最小编辑。
 * 与 airtype_text_cb 同在库内部线程触发，同样需要 marshal。 */
typedef void (*airtype_partial_cb)(const char *utf8_text, int32_t delete_chars,
                                   const char *utf8_append, void *userdata);

/* 库版本号，静态字符串。 */
AIRTYPE_API const char *airtype_version(void);

/* 载入 TOML 配置并构建核心。config_path 传 NULL 时使用内置默认配置。
 * 必须在 airtype_start() 之前调用，且进程内只需调用一次。 */
AIRTYPE_API int32_t airtype_init(const char *config_path_utf8);

/* 注册最终文本回调。可在 airtype_start() 之前或之后调用。
 * 传 cb=NULL 表示取消回调。 */
AIRTYPE_API int32_t airtype_set_text_callback(airtype_text_cb cb, void *userdata);

/* 注册流式中间结果回调。语义同 airtype_set_text_callback。
 * 不注册时 partial 只在库内记日志，不影响最终文本。 */
AIRTYPE_API int32_t airtype_set_partial_callback(airtype_partial_cb cb, void *userdata);

/* 启动 WebSocket 服务（非阻塞，内部起 tokio runtime）。 */
AIRTYPE_API int32_t airtype_start(void);

/* 停止服务并释放 runtime。可重复调用。 */
AIRTYPE_API void airtype_stop(void);

/* 最近一次错误描述；无错误时返回空字符串。 */
AIRTYPE_API const char *airtype_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AIRTYPE_FFI_H */
