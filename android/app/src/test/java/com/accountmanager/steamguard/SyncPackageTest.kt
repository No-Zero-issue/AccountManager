package com.accountmanager.steamguard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncPackageTest {

    /**
     * 桌面端（frontend/src/syncPackage.js）实际生成的一页同步包，原样搬进来当兼容性样本。
     * 桌面端实现一改，这里就应该跟着红 —— 两个平台的格式必须始终一致。
     */
    private val desktopPage = listOf(
        "AM2FA/1",
        "1/1",
        "GitHub\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30\t",
        "Steam 大号\tsteam\tOJZ2BP7STWSLUD7I23Q5AYZELZB5OAFU\tSHA1\t5\t30\tR12345",
        "谷歌 8 位\ttotp\tJBSWY3DPEHPK3PXPJBSWY3D\tSHA256\t8\t60\t"
    ).joinToString("\n")

    @Test
    fun parsesPageGeneratedByDesktop() {
        val page = SyncPackage.parse(desktopPage)

        assertEquals(1, page.index)
        assertEquals(1, page.total)
        assertEquals(3, page.items.size)

        val github = page.items[0]
        assertEquals("GitHub", github.name)
        assertEquals("totp", github.type)
        assertEquals("SHA1", github.algorithm)
        assertEquals(6, github.digits)
        assertEquals(30, github.period)
        assertNull(github.revocation)
        assertEquals(SecretParser.base32ToBase64("JBSWY3DPEHPK3PXP"), github.secretBase64)

        val steam = page.items[1]
        assertEquals("Steam 大号", steam.name)
        assertEquals("steam", steam.type)
        assertEquals("R12345", steam.revocation)
        assertEquals(5, steam.digits)
        // 桌面端把 Base64 shared_secret 转成 Base32 发过来，这里必须还原成同一串字节
        assertEquals("cnOgv/KdpLoP6Nbh0GMkXkPXALQ=", steam.secretBase64)
        assertEquals(
            SteamGuard.codeForTime("cnOgv/KdpLoP6Nbh0GMkXkPXALQ=", 1700000000L),
            SteamGuard.codeForTime(steam.secretBase64, 1700000000L)
        )

        val google = page.items[2]
        assertEquals("谷歌 8 位", google.name)
        assertEquals("SHA256", google.algorithm)
        assertEquals(8, google.digits)
        assertEquals(60, google.period)
    }

    @Test
    fun isPageOnlyMatchesSyncPackages() {
        assertTrue(SyncPackage.isPage(desktopPage))
        assertTrue(SyncPackage.isPage("  \nAM2FA/1\n1/1\nX\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30\t"))
        assertFalse(SyncPackage.isPage("otpauth://totp/Steam:x?secret=JBSWY3DPEHPK3PXP&issuer=Steam"))
        assertFalse(SyncPackage.isPage("cnOgv/KdpLoP6Nbh0GMkXkPXALQ="))
    }

    @Test
    fun readsPageIndexAndTotal() {
        val page = SyncPackage.parse("AM2FA/1\n2/3\n备用\tsteam\tOJZ2BP7STWSLUD7I23Q5AYZELZB5OAFU\tSHA1\t5\t30")
        assertEquals(2, page.index)
        assertEquals(3, page.total)
        assertEquals(1, page.items.size)
        assertNull(page.items[0].revocation)
    }

    @Test
    fun toleratesCrlfAndTrailingNewlines() {
        val page = SyncPackage.parse(desktopPage.replace("\n", "\r\n") + "\r\n")
        assertEquals(3, page.items.size)
    }

    @Test
    fun blankNameFallsBackToPlaceholder() {
        val page = SyncPackage.parse("AM2FA/1\n1/1\n\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30")
        assertEquals("未命名", page.items[0].name)
    }

    @Test
    fun malformedPackagesAreRejected() {
        val bad = listOf(
            "AM2FA/2\n1/1\nX\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30",        // 版本不认识
            "AM2FA/1",                                                      // 少了页码行
            "AM2FA/1\n第一页",                                               // 页码格式不对
            "AM2FA/1\n3/2\nX\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30",         // 页码越界
            "AM2FA/1\n1/1\nX\ttotp\tJBSWY3DPEHPK3PXP",                      // 字段不足
            "AM2FA/1\n1/1\nX\tsms\tJBSWY3DPEHPK3PXP\tSHA1\t6\t30",          // 类型不认识
            "AM2FA/1\n1/1\nX\ttotp\t!!!!\tSHA1\t6\t30",                     // 密钥不是 Base32
            "AM2FA/1\n1/1\nX\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t99\t30",        // 位数不合法
            "AM2FA/1\n1/1\nX\ttotp\tJBSWY3DPEHPK3PXP\tSHA1\t6\t0"           // 周期不合法
        )
        for (raw in bad) {
            var threw = false
            try {
                SyncPackage.parse(raw)
            } catch (e: Exception) {
                threw = true
            }
            assertTrue("应当拒绝: " + raw.replace("\n", "\\n"), threw)
        }
    }
}
