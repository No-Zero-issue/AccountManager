// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// DataStore.cpp —— 配置与单文件数据存储
//   所有状态集中在一个 AccountManager.json：{ version, vault, hello, config }
//   写入一律「读整份 -> 只替换自己那一节 -> 原子写回」，避免互相覆盖；
//   首次运行会把旧的 vault.json / hello.json / config.json 合并进来（旧文件保留不删）。

#include "Function.h"

std::wstring GetConfigFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path p(path);
    return (p.parent_path() / L"config.json").wstring();
}

// 读取配置
// 读取配置（来自 AccountManager.json 的 config 节）
json ReadConfigJson() {
    try {
        json ReadDataSection(const std::string& name);   // 定义在下方
        json section = ReadDataSection("config");
        return section.is_object() ? section : json::object();
    }
    catch (...) {
        return json::object();
    }
}

// 保存配置
// 保存配置（写入 AccountManager.json 的 config 节，其它节原样保留）
void SaveConfigJson(const json& j) {
    try {
        bool WriteDataSection(const std::string& name, const json& value);   // 定义在下方
        if (!WriteDataSection("config", j)) {
            throw std::runtime_error("cannot write config section");
        }
    }
    catch (const std::exception& e) {
        std::wstring err = L"Config Save Error: " + StringToWString(e.what());
        MessageBoxW(NULL, err.c_str(), L"Error", MB_OK);
    }
}
std::wstring GetDataFilePath() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return L"";
    std::filesystem::path p(path);
    return (p.parent_path() / L"AccountManager.json").wstring();
}

// 读取整份数据文件；不存在或解析失败时返回空对象
json ReadDataRoot() {
    // 首次运行：新文件不存在而旧三文件存在时，先合并（旧文件保留不删）
    MigrateLegacyDataFiles();
    const std::wstring file = GetDataFilePath();
    if (file.empty()) return json::object();
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    if (!in) return json::object();
    try {
        std::stringstream ss;
        ss << in.rdbuf();
        json root = json::parse(ss.str());
        return root.is_object() ? root : json::object();
    } catch (...) {
        return json::object();
    }
}

// 原子写入整份数据文件（先写 .tmp 再替换）
bool WriteDataRoot(const json& root) {
    const std::wstring file = GetDataFilePath();
    if (file.empty()) return false;
    const std::filesystem::path target(file);
    const std::filesystem::path tmp(target.wstring() + L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << root.dump(2);
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(tmp, target, ec);
    }
    return !ec;
}

// 读某一节；不存在返回 null
json ReadDataSection(const std::string& name) {
    json root = ReadDataRoot();
    if (root.is_object() && root.contains(name)) return root[name];
    return json();
}

// 读-改-写：只更新指定那一节，其它节原样保留
bool WriteDataSection(const std::string& name, const json& value) {
    json root = ReadDataRoot();
    if (!root.is_object()) root = json::object();
    root["version"] = 1;
    root[name] = value;
    return WriteDataRoot(root);
}

// 读-改-写：删除某一节（其它节原样保留）
bool RemoveDataSection(const std::string& name) {
    json root = ReadDataRoot();
    if (!root.is_object() || !root.contains(name)) return true;
    root.erase(name);
    return WriteDataRoot(root);
}

// 首次启动迁移：把旧的三个独立文件合并进 AccountManager.json（旧文件保留不删）
void MigrateLegacyDataFiles() {
    const std::wstring dataFile = GetDataFilePath();
    if (dataFile.empty()) return;
    if (std::filesystem::exists(std::filesystem::path(dataFile))) return;   // 已有新文件

    json root = json::object();
    root["version"] = 1;
    bool any = false;
    const std::pair<const char*, std::wstring> items[] = {
        { "vault",  GetVaultFilePath() },
        { "hello",  GetHelloFilePath() },
        { "config", GetConfigFilePath() },
    };
    for (const auto& item : items) {
        if (item.second.empty()) continue;
        const std::filesystem::path legacy(item.second);
        if (!std::filesystem::exists(legacy)) continue;
        std::ifstream in(legacy, std::ios::binary);
        if (!in) continue;
        try {
            std::stringstream ss;
            ss << in.rdbuf();
            root[item.first] = json::parse(ss.str());
            any = true;
        } catch (...) {
            // 旧文件损坏：跳过这一节，不阻断启动
        }
    }
    if (any) WriteDataRoot(root);
}
