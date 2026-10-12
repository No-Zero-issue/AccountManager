package com.accountmanager.steamguard

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// SyncPackage.kt —— 「从电脑同步」的同步包解析
//
// 格式与桌面端 frontend/src/syncPackage.js 一一对应，一页同步包就是一段纯文本：
//
//   AM2FA/1                                    ← 魔数 + 格式版本
//   1/3                                        ← 本页序号 / 总页数
//   名称 \t 类型 \t 密钥 \t 算法 \t 位数 \t 周期 \t 撤销码   ← 一条记录一行，制表符分隔
//
// 密钥统一是 Base32（桌面端会把 Steam 的 Base64 shared_secret 先转成 Base32）；
// 本类负责转成 App 内部使用的 Base64，转换逻辑复用 SecretParser，避免两套实现。
// 刻意不依赖任何 Android API —— 这样可以在纯 JVM 单元测试里验证（见 SyncPackageTest）。
object SyncPackage {

    const val MAGIC = "AM2FA/1"

    /** 一条待导入的记录（密钥已转成 App 内部使用的 Base64） */
    data class Item(
        val name: String,
        val secretBase64: String,
        val type: String,
        val digits: Int,
        val period: Int,
        val algorithm: String,
        val revocation: String?
    )

    /** 一页同步包 */
    data class Page(val index: Int, val total: Int, val items: List<Item>)

    /** 判断一段扫码内容是不是同步包（否则按单个 otpauth / 密钥处理） */
    fun isPage(text: String): Boolean = text.trimStart().startsWith(MAGIC)

    /**
     * 解析一页同步包。内容不合法时抛 IllegalArgumentException，
     * 调用方把消息直接显示给用户即可。
     */
    fun parse(raw: String): Page {
        val text = raw.replace("\r\n", "\n").replace('\r', '\n').trim('\n')
        val lines = text.split('\n')
        require(lines.size >= 2) { "同步包内容不完整" }
        require(lines[0].trim() == MAGIC) { "不是本程序的同步包（缺少 $MAGIC 标记）" }

        val matched = Regex("""^(\d+)/(\d+)$""").find(lines[1].trim())
            ?: throw IllegalArgumentException("同步包页码无法识别：" + lines[1].trim())
        val index = matched.groupValues[1].toInt()
        val total = matched.groupValues[2].toInt()
        require(total in 1..999 && index in 1..total) { "同步包页码越界：$index/$total" }

        val items = ArrayList<Item>()
        for (line in lines.drop(2)) {
            if (line.isBlank()) continue
            val parts = line.split('\t')
            require(parts.size >= 6) { "同步包这条记录字段不足：$line" }

            val name = parts[0].trim().ifEmpty { "未命名" }
            val type = parts[1].trim().lowercase()
            require(type == "steam" || type == "totp") { "同步包里有未知的类型：$type" }
            val secretBase64 = SecretParser.normalizeBase64(SecretParser.base32ToBase64(parts[2].trim()))
            val algorithm = parts[3].trim().uppercase().ifEmpty { "SHA1" }
            val digits = parts[4].trim().toIntOrNull() ?: if (type == "steam") 5 else 6
            val period = parts[5].trim().toIntOrNull() ?: 30
            require(digits in 4..10) { "同步包里的位数不合法：$digits" }
            require(period > 0) { "同步包里的周期不合法：$period" }
            val revocation = parts.getOrNull(6)?.trim()?.ifEmpty { null }

            items.add(Item(name, secretBase64, type, digits, period, algorithm, revocation))
        }
        return Page(index, total, items)
    }
}
