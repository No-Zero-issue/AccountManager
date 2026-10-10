package com.accountmanager.steamguard

import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Steam Guard 动态码生成。
 *
 * 与 PC 端实现完全一致（PC 端已与 node-steam-totp 做过 200 组差分比对）：
 *   counter = floor(unixSeconds / 30)
 *   mac     = HMAC-SHA1(shared_secret, BE64(counter))
 *   offset  = mac[19] & 0x0F
 *   value   = BE32(mac[offset..offset+4]) & 0x7FFFFFFF
 *   code    = 5 个字符：alphabet[value % 26]; value /= 26
 */
object SteamGuard {

    const val ALPHABET = "23456789BCDFGHJKMNPQRTVWXY"
    const val PERIOD = 30

    fun codeForTime(sharedSecretBase64: String, unixSeconds: Long): String {
        val key = Base64.getDecoder().decode(sharedSecretBase64)
        require(key.isNotEmpty()) { "密钥为空" }

        val counter = unixSeconds / PERIOD
        val message = ByteArray(8)
        for (i in 0 until 8) {
            message[7 - i] = ((counter ushr (8 * i)) and 0xFF).toByte()
        }

        val mac = Mac.getInstance("HmacSHA1")
        mac.init(SecretKeySpec(key, "HmacSHA1"))
        val hash = mac.doFinal(message)

        val offset = hash[19].toInt() and 0x0F
        var value = ((hash[offset].toInt() and 0x7F) shl 24) or
                ((hash[offset + 1].toInt() and 0xFF) shl 16) or
                ((hash[offset + 2].toInt() and 0xFF) shl 8) or
                (hash[offset + 3].toInt() and 0xFF)

        val sb = StringBuilder(5)
        repeat(5) {
            sb.append(ALPHABET[value % 26])
            value /= 26
        }
        return sb.toString()
    }

    fun currentCode(sharedSecretBase64: String, nowMillis: Long = System.currentTimeMillis()): String =
        codeForTime(sharedSecretBase64, nowMillis / 1000)

    /** 当前码剩余有效秒数（1..30） */
    fun secondsRemaining(nowMillis: Long = System.currentTimeMillis()): Int =
        PERIOD - ((nowMillis / 1000) % PERIOD).toInt()
}