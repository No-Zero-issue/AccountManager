package com.accountmanager.steamguard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 标准 TOTP（RFC 6238）测试。
 * 期望值由独立参照实现生成，且该参照实现先用 RFC 6238 附录 B 的官方 SHA1 向量校准通过
 * （SHA1/8 位：94287082 / 07081804 / 14050471 / 89005924 / 69279037 / 65353130）。
 * 密钥在测试里以 Base64 形式给出（ASCII 密钥的 Base64 编码）。
 */
class TotpTest {

    private data class Vector(
        val secretBase64: String,
        val time: Long,
        val digits: Int,
        val period: Int,
        val algorithm: String,
        val expected: String
    )

    private val vectors = listOf(
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 59L, 8, 30, "SHA1", "94287082"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 1111111109L, 8, 30, "SHA1", "07081804"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 1111111111L, 8, 30, "SHA1", "14050471"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 1234567890L, 8, 30, "SHA1", "89005924"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 2000000000L, 8, 30, "SHA1", "69279037"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 20000000000L, 8, 30, "SHA1", "65353130"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 59L, 8, 30, "SHA256", "46119246"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 1111111109L, 8, 30, "SHA256", "68084774"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 1111111111L, 8, 30, "SHA256", "67062674"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 1234567890L, 8, 30, "SHA256", "91819424"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 2000000000L, 8, 30, "SHA256", "90698825"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTI=", 20000000000L, 8, 30, "SHA256", "77737706"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 59L, 8, 30, "SHA512", "90693936"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 1111111109L, 8, 30, "SHA512", "25091201"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 1111111111L, 8, 30, "SHA512", "99943326"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 1234567890L, 8, 30, "SHA512", "93441116"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 2000000000L, 8, 30, "SHA512", "38618901"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNDU2Nzg5MDEyMzQ1Njc4OTAxMjM0NTY3ODkwMTIzNA==", 20000000000L, 8, 30, "SHA512", "47863826"),
    )

    @Test
    fun matchesRfc6238Vectors() {
        for (v in vectors) {
            assertEquals(
                "algo=" + v.algorithm + " time=" + v.time,
                v.expected,
                Totp.codeForTime(v.secretBase64, v.time, v.digits, v.period, v.algorithm)
            )
        }
    }

    @Test
    fun sixDigitTruncationWorks() {
        // 同一密钥同一时刻，8 位与 6 位应满足：6 位 = 8 位后 6 位
        val secret = "MTIzNDU2Nzg5MDEyMzQ1Njc4OTA="   // "12345678901234567890"
        val eight = Totp.codeForTime(secret, 59L, 8)
        val six = Totp.codeForTime(secret, 59L, 6)
        assertEquals(6, six.length)
        assertEquals(eight.takeLast(6), six)
    }

    @Test
    fun stableWithinPeriodAndChangesAcross() {
        val secret = "MTIzNDU2Nzg5MDEyMzQ1Njc4OTA="
        assertEquals(Totp.codeForTime(secret, 60L), Totp.codeForTime(secret, 89L))
        assertTrue(Totp.codeForTime(secret, 60L) != Totp.codeForTime(secret, 90L))
    }

    @Test
    fun remainingSecondsInRange() {
        val left = Totp.secondsRemaining(60000L, 30)
        assertTrue(left in 1..30)
    }
}