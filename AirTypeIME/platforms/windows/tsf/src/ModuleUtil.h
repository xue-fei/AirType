#pragma once

#include <string>

namespace airtype {

/// 本模块（探针 exe 或 tsf dll）所在目录，含尾部分隔符；失败返回空串。
std::wstring SelfModuleDir();

/// 宿主进程 exe 所在目录，含尾部分隔符；失败返回空串。
std::wstring HostExeDir();

}  // namespace airtype
