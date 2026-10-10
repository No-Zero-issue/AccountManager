package com.accountmanager.steamguard

import com.google.zxing.BinaryBitmap
import com.google.zxing.DecodeHintType
import com.google.zxing.MultiFormatReader
import com.google.zxing.RGBLuminanceSource
import com.google.zxing.common.HybridBinarizer

/**
 * 二维码解码。入参是 ARGB 像素数组（与 Android Bitmap.getPixels 的输出一致），
 * 刻意不依赖任何 Android 类型 —— 这样可以在纯 JVM 单元测试里做「编码→解码」往返验证。
 */
object QrDecoder {

    fun decodeArgb(pixels: IntArray, width: Int, height: Int): String? {
        if (width <= 0 || height <= 0 || pixels.size < width * height) return null
        return try {
            val source = RGBLuminanceSource(width, height, pixels)
            val bitmap = BinaryBitmap(HybridBinarizer(source))
            val hints = mapOf(DecodeHintType.TRY_HARDER to true)
            MultiFormatReader().decode(bitmap, hints).text
        } catch (e: Exception) {
            null   // 识别不出就是 null，绝不抛给界面
        }
    }
}