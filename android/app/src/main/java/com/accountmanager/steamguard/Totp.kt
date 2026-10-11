package com.accountmanager.steamguard

import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * 标准 TOTP（RFC 6238）—— 用于普通网站/软件（Google、GitHub 等）的 6 位动态码。
 * 与 Steam 那套的区别：这里用十进制 digits 位（默认 6），而不是 Steam 的 26 字母表 5 位。
 */
object Totp {

    fun codeForTime(
        secretBase64: String,
        unixSeconds: Long,
        digits: Int = 6,
        period: Int = 30,
        algorithm: String = "SHA1"
    ): String {
        val key = Base64.getDecoder().decode(secretBase64)
        require(key.isNotEmpty()) { "密钥为空" }
        require(period > 0) { "周期必须为正" }
        require(digits in 6..8) { "只支持 6/7/8 位" }

        val macName = when (algorithm.uppercase()) {
            "SHA1", "SHA-1" -> "HmacSHA1"
            "SHA256", "SHA-256" -> "HmacSHA256"
            "SHA512", "SHA-512" -> "HmacSHA512"
            else -> "HmacSHA1"
        }

        val counter = unixSeconds / period
        val message = ByteArray(8)
        for (i in 0 until 8) {
            message[7 - i] = ((counter ushr (8 * i)) and 0xFF).toByte()
        }

        val mac = Mac.getInstance(macName)
        mac.init(SecretKeySpec(key, macName))
        val hash = mac.doFinal(message)

        val offset = hash[hash.size - 1].toInt() and 0x0F
        val binary = ((hash[offset].toInt() and 0x7F) shl 24) or
                ((hash[offset + 1].toInt() and 0xFF) shl 16) or
                ((hash[offset + 2].toInt() and 0xFF) shl 8) or
                (hash[offset + 3].toInt() and 0xFF)

        val mod = when (digits) {
            6 -> 1000000
            7 -> 10000000
            else -> 100000000
        }
        return (binary % mod).toString().padStart(digits, '0')
    }

    fun currentCode(
        secretBase64: String,
        nowMillis: Long = System.currentTimeMillis(),
        digits: Int = 6,
        period: Int = 30,
        algorithm: String = "SHA1"
    ): String = codeForTime(secretBase64, nowMillis / 1000, digits, period, algorithm)

    fun secondsRemaining(nowMillis: Long = System.currentTimeMillis(), period: Int = 30): Int =
        period - ((nowMillis / 1000) % period).toInt()
}