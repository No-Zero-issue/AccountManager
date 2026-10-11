// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Vdf.cpp —— VDF（Valve KeyValues）解析 + 从本机 Steam 导入账号
//   读 Steam 安装目录下的 config/loginusers.vdf，取出本地已保存的账号名。

#include "Function.h"

namespace vdf {

void SkipWhitespace(const std::string& text, size_t& pos) {
    // 跳过 UTF-8 BOM：部分 Steam 文件带 BOM，不处理会导致解析直接失败
    if (pos == 0 && text.size() >= 3
        && (unsigned char)text[0] == 0xEF
        && (unsigned char)text[1] == 0xBB
        && (unsigned char)text[2] == 0xBF) {
        pos = 3;
    }

    while (pos < text.size()) {
        char ch = text[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pos++;
            continue;
        }
        // 行注释
        if (ch == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            while (pos < text.size() && text[pos] != '\n') pos++;
            continue;
        }
        break;
    }
}

// 读取一个带引号的 token（KeyValues 文本格式里 key/value 都是引号包裹）
bool ReadToken(const std::string& text, size_t& pos, std::string& out) {
    SkipWhitespace(text, pos);
    if (pos >= text.size() || text[pos] != '"') return false;

    pos++;   // 跳过起始引号
    out.clear();
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos++;
            out.push_back(text[pos++]);
            continue;
        }
        out.push_back(text[pos++]);
    }
    if (pos >= text.size()) return false;
    pos++;   // 跳过结束引号
    return true;
}

// 解析 { ... } 内部
bool ParseObjectBody(const std::string& text, size_t& pos, json& out) {
    out = json::object();
    for (;;) {
        SkipWhitespace(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == '}') {
            pos++;
            return true;
        }

        std::string key;
        if (!ReadToken(text, pos, key)) return false;

        SkipWhitespace(text, pos);
        if (pos >= text.size()) return false;

        if (text[pos] == '{') {
            pos++;
            json child;
            if (!ParseObjectBody(text, pos, child)) return false;
            out[key] = child;
        }
        else {
            std::string value;
            if (!ReadToken(text, pos, value)) return false;
            out[key] = value;
        }
    }
}

// 解析整份文档（顶层可能是 "key" { ... } 或 "key" "value"）
bool ParseDocument(const std::string& text, json& out) {
    out = json::object();
    size_t pos = 0;
    for (;;) {
        SkipWhitespace(text, pos);
        if (pos >= text.size()) break;

        std::string key;
        if (!ReadToken(text, pos, key)) break;

        SkipWhitespace(text, pos);
        if (pos < text.size() && text[pos] == '{') {
            pos++;
            json child;
            if (!ParseObjectBody(text, pos, child)) return false;
            out[key] = child;
        }
        else {
            std::string value;
            if (!ReadToken(text, pos, value)) break;
            out[key] = value;
        }
    }
    return out.is_object();
}

} // namespace vdf

// 读取本机 Steam 已登录账号列表（只读，不修改任何 Steam 数据）
json ReadSteamLoginUsers(std::wstring& error) {
    error.clear();
    json accounts = json::array();

    std::wstring steamExe = GetSteamPath();
    if (steamExe.empty()) {
        error = L"注册表里找不到 Steam 安装路径（请先运行一次 Steam）";
        return accounts;
    }

    std::filesystem::path vdfPath = std::filesystem::path(steamExe).parent_path() / L"config" / L"loginusers.vdf";
    std::error_code ec;
    if (!std::filesystem::exists(vdfPath, ec) || std::filesystem::file_size(vdfPath, ec) == 0) {
        error = L"找不到 config\\loginusers.vdf";
        return accounts;
    }

    try {
        std::ifstream file(vdfPath.wstring(), std::ios::binary);
        if (!file.is_open()) {
            error = L"无法打开 loginusers.vdf";
            return accounts;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        const std::string raw = buffer.str();
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
            error = L"loginusers.vdf 是 UTF-16 编码，暂不支持";
            return accounts;
        }

        json doc;
        if (!vdf::ParseDocument(raw, doc)) {
            error = L"loginusers.vdf 解析失败";
            return accounts;
        }

        if (!doc.contains("users") || !doc["users"].is_object()) {
            error = L"loginusers.vdf 结构不符合预期";
            return accounts;
        }

        for (auto& item : doc["users"].items()) {
            const json& info = item.value();
            if (!info.is_object()) continue;

            json account = json::object();
            account["steamId"] = item.key();
            account["accountName"] = info.value("AccountName", "");
            account["personaName"] = info.value("PersonaName", "");
            account["mostRecent"] = info.value("MostRecent", "0") == "1";
            accounts.push_back(account);
        }
    }
    catch (...) {
        error = L"读取 loginusers.vdf 时出错";
        return json::array();
    }

    return accounts;
}


