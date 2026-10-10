package com.accountmanager.steamguard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SecretParserTest {

    private val secretBase64 = "cnOgv/KdpLoP6Nbh0GMkXkPXALQ="

    @Test
    fun mafileJsonIsParsed() {
        val json = """{"shared_secret":"$secretBase64","identity_secret":"aWRlbnRpdHk=","revocation_code":"R12345","account_name":"alt_account"}"""
        val parsed = SecretParser.parse(json)
        assertEquals(secretBase64, parsed.secretBase64)
        assertEquals("alt_account", parsed.name)
        assertEquals("R12345", parsed.revocationCode)
    }

    @Test
    fun bareBase64IsAccepted() {
        val parsed = SecretParser.parse(secretBase64)
        assertEquals(secretBase64, parsed.secretBase64)
        assertNull(parsed.name)
    }

    @Test
    fun otpauthBase32EqualsSameSecret() {
        // 同一串字节的 Base32 形式（由 Base64 密钥转出，测试里固定写死以保证独立）
        val base32 = "OJZ2BP7STWSLUD7I23Q5AYZELZB5OAFU"
        val parsed = SecretParser.parse("otpauth://totp/Steam:alt_account?secret=$base32&issuer=Steam")
        assertEquals("alt_account", parsed.name)
        assertEquals(secretBase64, SecretParser.base32ToBase64(base32))
        // 用 otpauth 解析出的密钥算码，应与直接用 Base64 算码完全一致
        assertEquals(
            SteamGuard.codeForTime(secretBase64, 1700000000L),
            SteamGuard.codeForTime(parsed.secretBase64, 1700000000L)
        )
    }

    @Test
    fun invalidInputsThrow() {
        val bad = listOf("", "  ", "{\"foo\":\"bar\"}", "otpauth://totp/Steam:x?issuer=Steam", "!!!not base64!!!")
        for (raw in bad) {
            var threw = false
            try { SecretParser.parse(raw) } catch (e: Exception) { threw = true }
            assertTrue("应当拒绝: " + raw, threw)
        }
    }

    @Test
    fun whitespaceAndMissingPaddingTolerated() {
        val spaced = " cnOgv/KdpLoP6Nbh0GMkXkPXALQ \n"
        assertEquals(secretBase64, SecretParser.normalizeBase64(spaced))
    }
}