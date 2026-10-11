// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Utils.cpp —— 字符编码 / Base64 / Hex / 随机数
//   UTF-8 <-> UTF-16 转换（界面与 C++ 之间的 JSON 一律 UTF-8）、Base64、十六进制、随机字节。

#include "Function.h"

// UTF-8 <-> UTF-16 转换（界面与 C++ 之间的 JSON 一律使用 UTF-8）
std::string WStringToString(const std::wstring& text) {
    if (text.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (size <= 0) return std::string();
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size, NULL, NULL);
    return out;
}

std::wstring StringToWString(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (size <= 0) return std::wstring();
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
    return out;
}
bool RandomBytes(std::vector<BYTE>& out, size_t count) {
    out.resize(count);
    NTSTATUS status = BCryptGenRandom(NULL, out.data(), (ULONG)count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
}

const std::string kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        unsigned int n = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8) | (unsigned int)data[i + 2];
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back(kBase64Chars[n & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        bool two = (i + 1 < data.size());
        unsigned int n = ((unsigned int)data[i] << 16) | (two ? ((unsigned int)data[i + 1] << 8) : 0);
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(two ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<BYTE> Base64Decode(const std::string& text) {
    std::vector<BYTE> out;
    int buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        size_t index = kBase64Chars.find(c);
        if (index == std::string::npos) continue;
        buffer = (buffer << 6) | (int)index;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((BYTE)((buffer >> bits) & 0xFF));
        }
    }
    return out;
}
std::vector<BYTE> HexToBytes(const std::string& hex) {    std::vector<BYTE> bytes;
    bytes.reserve(hex.size() / 2);
    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        bytes.push_back((BYTE)((hi << 4) | lo));
    }
    return bytes;
}

std::string BytesToHex(const std::vector<BYTE>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (BYTE byte : bytes) {
        out.push_back(digits[(byte >> 4) & 0x0F]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string BytesToBase64(const std::vector<BYTE>& bytes) {
    return Base64Encode(bytes);
}

std::string Base64ToBytesString(const std::string& base64) {
    std::vector<BYTE> bytes = Base64Decode(base64);
    return std::string(bytes.begin(), bytes.end());
}
