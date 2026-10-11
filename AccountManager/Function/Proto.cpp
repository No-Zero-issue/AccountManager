// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Proto.cpp —— 极简 protobuf 编解码（只实现本项目用到的那几种 wire type）
//   写：varint / uint64 / bytes / fixed64；读：varint / 按字段号查找。
//   Steam 的 IAuthenticationService / ITwoFactorService 走 protobuf，这里避免引入完整 protobuf 依赖。

#include "Function.h"

namespace steampb {

void WriteVarint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back((char)((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back((char)(value & 0x7F));
}

void WriteUInt64Field(std::string& out, uint32_t field, uint64_t value) {
    WriteVarint(out, ((uint64_t)field << 3) | 0);   // wire type 0 = varint
    WriteVarint(out, value);
}

void WriteBytesField(std::string& out, uint32_t field, const std::string& value) {
    WriteVarint(out, ((uint64_t)field << 3) | 2);   // wire type 2 = length-delimited
    WriteVarint(out, value.size());
    out.append(value);
}

bool ReadVarint(const std::string& data, size_t& pos, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (pos < data.size() && shift <= 63) {
        uint8_t byte = (uint8_t)data[pos++];
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

// 遍历字段，取出第一个匹配的 varint 或 length-delimited 字段
bool FindField(const std::string& data, uint32_t wantedField, uint32_t wantedWireType,
               std::string& bytesOut, uint64_t& varintOut) {
    size_t pos = 0;
    while (pos < data.size()) {
        uint64_t tag = 0;
        if (!ReadVarint(data, pos, tag)) return false;

        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wireType = (uint32_t)(tag & 0x07);

        if (wireType == 0) {
            uint64_t value = 0;
            if (!ReadVarint(data, pos, value)) return false;
            if (field == wantedField && wantedWireType == 0) {
                varintOut = value;
                return true;
            }
        }
        else if (wireType == 2) {
            uint64_t length = 0;
            if (!ReadVarint(data, pos, length)) return false;
            if (length > data.size() - pos) return false;
            if (field == wantedField && wantedWireType == 2) {
                bytesOut.assign(data, pos, (size_t)length);
                return true;
            }
            pos += (size_t)length;
        }
        else if (wireType == 5) {
            if (data.size() - pos < 4) return false;
            pos += 4;
        }
        else if (wireType == 1) {
            if (data.size() - pos < 8) return false;
            if (field == wantedField && wantedWireType == 1) {
                uint64_t value = 0;
                for (int i = 0; i < 8; i++) {
                    value |= ((uint64_t)(uint8_t)data[pos + i]) << (8 * i);
                }
                varintOut = value;
                return true;
            }
            pos += 8;
        }
        else {
            return false;   // 不支持的 wire type
        }
    }
    return false;
}

bool FindVarint(const std::string& data, uint32_t field, uint64_t& value) {
    std::string dummy;
    return FindField(data, field, 0, dummy, value);
}

bool FindBytes(const std::string& data, uint32_t field, std::string& value) {
    uint64_t dummy = 0;
    return FindField(data, field, 2, value, dummy);
}

} // namespace steampb
namespace steampb {

// fixed64（wire type 1）：小端 8 字节，用于 steamid / serial_number 这类字段
void WriteFixed64Field(std::string& out, uint32_t field, uint64_t value) {
    WriteVarint(out, ((uint64_t)field << 3) | 1);
    for (int i = 0; i < 8; i++) {
        out.push_back((char)((value >> (8 * i)) & 0xFF));
    }
}

} // namespace steampb
