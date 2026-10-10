package com.accountmanager.steamguard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 差分向量来自 PC 端已验证的实现（PC 端 JS 版本与 node-steam-totp 做过 200 组随机比对，全部一致）。
 * 因此这些固定向量的期望值是可信的：安卓实现必须与它们完全相等。
 */
class SteamGuardTest {

    private data class Vector(val secret: String, val time: Long, val expected: String)

    private val vectors = listOf(
        Vector("cnOgv/KdpLoP6Nbh0GMkXkPXALQ=", 1700000000L, "X45RP"),
        Vector("cnOgv/KdpLoP6Nbh0GMkXkPXALQ=", 1700000015L, "YWH3Q"),
        Vector("dGVzdHNlY3JldDEyMzQ1Njc4OTA=", 1767225600L, "Q7473"),
        Vector("AAAAAAAAAAAAAAAAAAAAAAAAAAA=", 1790000000L, "RBQ9K"),
        Vector("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=", 1600000000L, "J8D72"),
        Vector("YWJjZGVmZ2hpamtsbW5vcHFyc3Q=", 1850000000L, "9GW3H"),
        Vector("c2VjcmV0LWtleS1mb3ItdGVzdC1vbmx5", 1234567890L, "W8MN9")
    )

    @Test
    fun matchesVerifiedVectors() {
        for (v in vectors) {
            assertEquals("secret=" + v.secret + " time=" + v.time,
                v.expected, SteamGuard.codeForTime(v.secret, v.time))
        }
    }

    @Test
    fun codeHasFiveCharsFromAlphabet() {
        val code = SteamGuard.codeForTime("cnOgv/KdpLoP6Nbh0GMkXkPXALQ=", 1700000000L)
        assertEquals(5, code.length)
        assertTrue(code.all { SteamGuard.ALPHABET.contains(it) })
        assertTrue(code.none { it in "01AEILOSUZ" })
    }

    @Test
    fun stableWithinWindowAndChangesAcrossWindow() {
        val s = "cnOgv/KdpLoP6Nbh0GMkXkPXALQ="
        // 1699999980 是 30 的整数倍（时间窗起点）
        assertEquals(SteamGuard.codeForTime(s, 1699999980L), SteamGuard.codeForTime(s, 1699999980L + 29))
        assertTrue(SteamGuard.codeForTime(s, 1699999980L) != SteamGuard.codeForTime(s, 1699999980L + 30))
    }

    @Test
    fun periodAndRemaining() {
        assertEquals(30, SteamGuard.PERIOD)
        val left = SteamGuard.secondsRemaining(1699999980000L)
        assertTrue(left in 1..30)
    }
}