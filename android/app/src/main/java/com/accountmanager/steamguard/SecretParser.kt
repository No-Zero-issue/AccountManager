package com.accountmanager.steamguard

import java.util.Base64

/**
 * 密钥来源解析：.maFile(JSON 片段) / otpauth:// 链接 / 裸 Base64。
 * 刻意不使用 org.json 或任何 Android API —— 这样可以在纯 JVM 单元测试里验证。
 */
object SecretParser {

    data class Parsed(
        val name: String?,
        val secretBase64: String,
        val revocationCode: String? = null,
        /** "steam" = Steam Guard（5 位字母）；"totp" = 标准 TOTP（默认 6 位数字） */
        val type: String = "steam",
        val digits: Int = 6,
        val period: Int = 30,
        val algorithm: String = "SHA1"
    )

    fun parse(raw: String): Parsed {
        val text = raw.trim()
        require(text.isNotEmpty()) { "内容为空" }

        // 1) .maFile（Steam Desktop Authenticator 导出的 JSON）；用最小解析避免依赖 JSON 库
        if (text.startsWith("{")) {
            val shared = jsonField(text, "shared_secret")
                ?: throw IllegalArgumentException("JSON 里没有 shared_secret 字段")
            val name = jsonField(text, "account_name")
            val revocation = jsonField(text, "revocation_code")
            val secret = normalizeBase64(shared)
            return Parsed(name, secret, revocation)
        }

        // 2) otpauth:// 链接（Steam 官方设置流程给出的格式，secret 是 Base32）
        if (text.startsWith("otpauth://", ignoreCase = true)) {
            val params = text.substringAfter('?', "")
            val base32 = params.split('&')
                .firstOrNull { it.startsWith("secret=", ignoreCase = true) }
                ?.substringAfter('=')
                ?: throw IllegalArgumentException("otpauth 链接缺少 secret 参数")
            fun param(key: String): String? = params.split('&')
                .firstOrNull { it.startsWith(key + "=", ignoreCase = true) }
                ?.substringAfter('=')

            val digits = param("digits")?.toIntOrNull() ?: 6
            val period = param("period")?.toIntOrNull() ?: 30
            val algorithm = param("algorithm")?.uppercase() ?: "SHA1"
            val label = labelFromUri(text)
            val head = text.substringBefore('?')
            val isSteam = text.contains("issuer=Steam", ignoreCase = true) ||
                head.contains("Steam:", ignoreCase = true) || head.endsWith("/Steam", ignoreCase = true)
            return Parsed(
                label, base32ToBase64(base32), null,
                if (isSteam) "steam" else "totp",
                digits, period, algorithm
            )
        }

        // 3) 裸 Base64 shared_secret
        return Parsed(null, normalizeBase64(text))
    }

    /** 校验并规范化 Base64（去掉空白、补 padding） */
    fun normalizeBase64(value: String): String {
        val cleaned = value.filterNot { it.isWhitespace() }
        val padded = when (cleaned.length % 4) {
            2 -> cleaned + "=="
            3 -> cleaned + "="
            0 -> cleaned
            else -> throw IllegalArgumentException("Base64 长度不合法")
        }
        val bytes = try {
            Base64.getDecoder().decode(padded)
        } catch (e: IllegalArgumentException) {
            throw IllegalArgumentException("不是合法的 Base64 密钥")
        }
        require(bytes.size >= 5) { "密钥太短" }
        return Base64.getEncoder().encodeToString(bytes)
    }

    /** 极简 JSON 字符串字段提取：支持 "key" : "value" 与转义引号 */
    fun jsonField(json: String, key: String): String? {
        val regex = Regex("\"" + Regex.escape(key) + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"")
        val m = regex.find(json) ?: return null
        return m.groupValues[1].replace("\\\"", "\"").replace("\\\\", "\\")
    }

    fun labelFromUri(uri: String): String? {
        val path = uri.substringAfter("://", "").substringBefore('?')
        val label = path.substringAfter('/', "").substringAfterLast('/')
        if (label.isEmpty()) return null
        val decoded = try { java.net.URLDecoder.decode(label, "UTF-8") } catch (e: Exception) { label }
        return decoded.substringAfter(':', decoded).trim().ifEmpty { null }
    }

    /** RFC 4648 Base32 解码 → 重新以 Base64 输出（Steam 内部统一用 Base64） */
    fun base32ToBase64(base32: String): String {
        val alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
        val clean = base32.trim().replace("=", "").replace(" ", "").uppercase()
        require(clean.isNotEmpty()) { "Base32 为空" }
        var buffer = 0
        var bits = 0
        val out = java.io.ByteArrayOutputStream()
        for (c in clean) {
            val idx = alphabet.indexOf(c)
            require(idx >= 0) { "Base32 含非法字符: $c" }
            buffer = (buffer shl 5) or idx
            bits += 5
            if (bits >= 8) {
                out.write((buffer shr (bits - 8)) and 0xFF)
                bits -= 8
            }
        }
        return java.util.Base64.getEncoder().encodeToString(out.toByteArray())
    }
}