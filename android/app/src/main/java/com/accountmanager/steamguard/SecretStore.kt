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
 *
 * 两道保险，避免「一次坏写 / 一次坏解析就把令牌全丢了」：
 *   1) 覆盖前先把旧文件复制成 `entries.bin.bak`，主文件读不出来时自动回退到备份；
 *   2) 写盘先写 `entries.bin.tmp` 再改名（同一个目录内 rename 是原子的），
 *      写到一半被杀也只留下临时文件，不会把正式文件截成半个。
 */
class SecretStore(private val file: File) {

    private val alias = "steamguard_store_v1"
    private val transformation = "AES/GCM/NoPadding"
    private val tagBits = 128

    /** 读取结果：text 为 null 表示确实没有可用数据；fromBackup 表示主文件坏了、用的是上一次的备份 */
    data class Loaded(val text: String?, val fromBackup: Boolean)

    private val backupFile: File get() = File(file.parentFile, file.name + ".bak")
    private val tempFile: File get() = File(file.parentFile, file.name + ".tmp")

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
        val payload = byteArrayOf(iv.size.toByte()) + iv + cipherText

        file.parentFile?.mkdirs()
        // 覆盖前留一份上一版（备份失败不阻塞保存，最多是这次没有回滚点）
        if (file.exists() && file.length() > 0) {
            try {
                file.copyTo(backupFile, overwrite = true)
            } catch (e: Exception) {
                // 忽略：磁盘满 / 权限异常时仍然继续写主文件
            }
        }

        // 先写临时文件再改名，避免写一半被杀留下损坏的主文件
        tempFile.writeBytes(payload)
        if (!tempFile.renameTo(file)) {
            file.writeBytes(payload)   // 极端情况下改名失败（跨分区等），退回直接写
            tempFile.delete()
        }
    }

    /** 先读主文件，读不出来再退回上一次的备份 */
    fun load(): Loaded {
        decrypt(file)?.let { return Loaded(it, false) }
        decrypt(backupFile)?.let { return Loaded(it, true) }
        return Loaded(null, false)
    }

    private fun decrypt(source: File): String? {
        if (!source.exists()) return null
        return try {
            val bytes = source.readBytes()
            if (bytes.size < 2) return null
            val ivLen = bytes[0].toInt()
            if (bytes.size < 1 + ivLen) return null
            val iv = bytes.copyOfRange(1, 1 + ivLen)
            val data = bytes.copyOfRange(1 + ivLen, bytes.size)
            val cipher = Cipher.getInstance(transformation)
            cipher.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(tagBits, iv))
            String(cipher.doFinal(data), Charsets.UTF_8)
        } catch (e: Exception) {
            null   // 密钥失效 / 数据损坏：当作没有数据（不崩溃），交给调用方决定怎么办
        }
    }
}
