// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// version.h —— 程序版本的唯一定义处（single source of truth）
//   发版时只改下面那四个数字，其余地方的版本号全部引用本文件：
//     AccountManager.rc        关于框文本 / VERSIONINFO（EXE 文件属性里的版本）
//     Function/Http.cpp        WinHTTP 的 User-Agent
//     frontend/vite.config.js  解析本文件并把版本注入界面（状态栏 / 关于弹窗）
//
//   四个数字各管什么：
//     主 / 次 / 修订（MAJOR.MINOR.PATCH）→ 界面显示的版本、关于框、User-Agent
//     构建 BUILD（第四位）              → 只进 EXE 文件属性的完整版本，界面不显示
//
//   本文件有两条硬性约束，都是 rc.exe 预处理器太弱导致的，改动前请先读：
//     1) 正文只允许 ASCII —— .rc 是 UTF-16，rc.exe 从 #include 的文件里读中文
//        字符串会直接报 “unexpected end of file”；中文只能出现在注释里；
//     2) rc.exe 不支持相邻字符串字面量拼接（"a" "b"）出现在对话框资源里，所以给
//        关于框用的版本号必须整体写成单个字面量（下面的 APP_VERSION_LABEL /
//        APP_VERSION_FULL）；另外 rc.exe 也不支持 #define 的反斜杠续行，
//        每个宏都写成一行。
//   为了避免「数字改了、字符串忘了改」，C++ 侧用 static_assert 把两者绑死：
//   数字与字符串不一致时编译直接失败（见文件末尾）。

#pragma once

// ============================================================ 唯一的四个数字
// 改版本号只改这四行
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 2
#define APP_VERSION_BUILD 0

// ============================================================ 给 .rc 用
// VERSIONINFO 的数值字段（FILEVERSION / PRODUCTVERSION）：逗号分隔的四个数字
#define APP_VERSION_COMMA APP_VERSION_MAJOR,APP_VERSION_MINOR,APP_VERSION_PATCH,APP_VERSION_BUILD
// 界面显示用：主.次.修订，例如 "1.0.2"
#define APP_VERSION_LABEL "1.0.2"
// 完整版本：主.次.修订.构建，例如 "1.0.2.0"，用于 EXE 文件属性
#define APP_VERSION_FULL "1.0.2.0"

// ============================================================ 给 C++ 用
#ifdef __cplusplus

#define AM_VERSION_WIDEN_IMPL(x) L##x
#define AM_VERSION_WIDEN(x) AM_VERSION_WIDEN_IMPL(x)

// 宽字符版，便于和其它宽字符串拼接
#define APP_VERSION_WLABEL AM_VERSION_WIDEN(APP_VERSION_LABEL)
// WinHTTP 请求头里的 User-Agent
#define APP_USER_AGENT L"AccountManager/" APP_VERSION_WLABEL

// ---- 一致性校验：把上面的数字与字符串绑在一起 ----
namespace am_version_detail {

constexpr bool SameText(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// 极简的常量拼接：把数字按十进制追加进缓冲区
struct Dotted {
    char text[16]{};
    int length = 0;

    constexpr void Push(char c) { text[length++] = c; }

    constexpr void PushNumber(unsigned value) {
        char digits[8]{};
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        } while (value != 0);
        while (count > 0) Push(digits[--count]);
    }
};

// 主.次.修订
constexpr Dotted MakeLabel() {
    Dotted dotted;
    dotted.PushNumber(APP_VERSION_MAJOR);
    dotted.Push('.');
    dotted.PushNumber(APP_VERSION_MINOR);
    dotted.Push('.');
    dotted.PushNumber(APP_VERSION_PATCH);
    return dotted;
}

// 主.次.修订.构建
constexpr Dotted MakeFull() {
    Dotted dotted = MakeLabel();
    dotted.Push('.');
    dotted.PushNumber(APP_VERSION_BUILD);
    return dotted;
}

constexpr Dotted kDerivedLabel = MakeLabel();
constexpr Dotted kDerivedFull = MakeFull();

}  // namespace am_version_detail

static_assert(am_version_detail::SameText(am_version_detail::kDerivedLabel.text, APP_VERSION_LABEL),
              "version.h: APP_VERSION_LABEL 与 APP_VERSION_MAJOR / MINOR / PATCH 不一致，请同步修改");
static_assert(am_version_detail::SameText(am_version_detail::kDerivedFull.text, APP_VERSION_FULL),
              "version.h: APP_VERSION_FULL 与四个版本数字不一致，请同步修改");

#endif  // __cplusplus
