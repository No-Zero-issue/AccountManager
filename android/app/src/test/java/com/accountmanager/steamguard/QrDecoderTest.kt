package com.accountmanager.steamguard

import com.google.zxing.BarcodeFormat
import com.google.zxing.MultiFormatWriter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class QrDecoderTest {

    private fun encode(text: String, size: Int = 280): Triple<IntArray, Int, Int> {
        val matrix = MultiFormatWriter().encode(text, BarcodeFormat.QR_CODE, size, size)
        val w = matrix.width
        val h = matrix.height
        val pixels = IntArray(w * h)
        for (y in 0 until h) {
            for (x in 0 until w) {
                pixels[y * w + x] = if (matrix.get(x, y)) 0xFF000000.toInt() else 0xFFFFFFFF.toInt()
            }
        }
        return Triple(pixels, w, h)
    }

    @Test
    fun decodesOtpauthQr() {
        val text = "otpauth://totp/Steam:alt_account?secret=OJZ2BP7STWSLUD7I23Q5AYZELZB5OAFU&issuer=Steam"
        val (pixels, w, h) = encode(text)
        assertEquals(text, QrDecoder.decodeArgb(pixels, w, h))
        // 解码结果应能被密钥解析器接受
        val parsed = SecretParser.parse(QrDecoder.decodeArgb(pixels, w, h)!!)
        assertEquals("alt_account", parsed.name)
    }

    @Test
    fun decodesMafileStyleQr() {
        val text = """{"shared_secret":"cnOgv/KdpLoP6Nbh0GMkXkPXALQ=","account_name":"alt"}"""
        val (pixels, w, h) = encode(text)
        assertEquals(text, QrDecoder.decodeArgb(pixels, w, h))
    }

    @Test
    fun blankImageReturnsNullInsteadOfThrowing() {
        val pixels = IntArray(64 * 64) { 0xFFFFFFFF.toInt() }
        assertNull(QrDecoder.decodeArgb(pixels, 64, 64))
    }

    @Test
    fun badArgumentsReturnNull() {
        assertNull(QrDecoder.decodeArgb(IntArray(4), 0, 0))
        assertNull(QrDecoder.decodeArgb(IntArray(4), 10, 10))
    }
}