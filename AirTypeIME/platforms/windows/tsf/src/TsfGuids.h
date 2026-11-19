#pragma once

// AirType TSF 文本服务的固定 GUID。
// 定义在 TsfGuids.cpp（那里定义 INITGUID），这里只声明，供多个 TU 使用。
//
//   CLSID_AIRTYPE_TSF    文本服务 COM 类
//   GUID_AIRTYPE_PROFILE 语言配置（中英文共用一个 profile）

extern "C" {
extern const CLSID CLSID_AIRTYPE_TSF;
extern const GUID GUID_AIRTYPE_PROFILE;
}
