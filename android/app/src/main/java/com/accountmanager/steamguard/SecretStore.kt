package com.accountmanager.steamguard

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * 条目持久化：AES-256-GCM，密钥由 Android Keystore 生成并保管（不出 TEE，App 也读不到明文密钥）。
 * 文件格式：[1 字节 IV 长度][IV][密文]。
 */
class SecretStore(private val file: File) {

    private val alias = "steamguard_store_v1"
    private val transformation = "AES/GCM/NoPadding"
    private val tagBits = 128

    private fun secretKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getEntry(alias, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return generator.generateKey()
    }

    fun save(plainText: String) {
        val cipher = Cipher.getInstance(transformation)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey())
        val iv = cipher.iv
        val cipherText = cipher.doFinal(plainText.toByteArray(Charsets.UTF_8))
        file.parentFile?.mkdirs()
        file.writeBytes(byteArrayOf(iv.size.toByte()) + iv + cipherText)
    }

    fun load(): String? {
        if (!file.exists()) return null
        return try {
            val bytes = file.readBytes()
            if (bytes.size < 2) return null
            val ivLen = bytes[0].toInt()
            if (bytes.size < 1 + ivLen) return null
            val iv = bytes.copyOfRange(1, 1 + ivLen)
            val data = bytes.copyOfRange(1 + ivLen, bytes.size)
            val cipher = Cipher.getInstance(transformation)
            cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(tagBits, iv))
            String(cipher.doFinal(data), Charsets.UTF_8)
        } catch (e: Exception) {
            null   // 密钥失效/数据损坏：当作空库（不崩溃）
        }
    }
}