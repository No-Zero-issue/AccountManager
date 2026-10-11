package com.accountmanager.steamguard

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.google.zxing.integration.android.IntentIntegrator
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : Activity() {

    private class Entry(var name: String, var secret: String, var revocation: String? = null)

    // 深色主题配色
    private val colorBg = "#101216"
    private val colorCard = "#1b1f27"
    private val colorCardBorder = "#2a3040"
    private val colorText = "#e8ecf4"
    private val colorDim = "#8b94a7"
    private val colorAccent = "#4ea1ff"
    private val colorWarn = "#e0a030"
    private val colorOnAccent = "#0b1220"
    private val colorDanger = "#e05a4f"

    private val entries = mutableListOf<Entry>()
    private val rows = mutableListOf<Row>()

    /** 一行卡片里需要每帧刷新的控件 */
    private class Row(val code: TextView, val remain: TextView, val bar: ProgressBar, val entry: Entry)
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var store: SecretStore
    private lateinit var listContainer: LinearLayout
    private var sheetDialog: Dialog? = null

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    private fun rounded(color: String, radiusDp: Int, borderColor: String? = null, borderDp: Int = 0): GradientDrawable {
        return GradientDrawable().apply {
            setColor(Color.parseColor(color))
            cornerRadius = dp(radiusDp).toFloat()
            if (borderColor != null && borderDp > 0) setStroke(dp(borderDp), Color.parseColor(borderColor))
        }
    }

    private fun text(value: String, sizeSp: Float, color: String, bold: Boolean = false): TextView {
        return TextView(this).apply {
            text = value
            textSize = sizeSp
            setTextColor(Color.parseColor(color))
            if (bold) typeface = Typeface.DEFAULT_BOLD
        }
    }

    private fun button(label: String, bg: String = colorCard, fg: String = colorText): Button {
        return Button(this).apply {
            text = label
            textSize = 13f
            setTextColor(Color.parseColor(fg))
            background = rounded(bg, 10, colorCardBorder, 1)
            stateListAnimator = null
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = SecretStore(java.io.File(filesDir, "entries.bin"))
        loadEntries()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(28), dp(16), dp(12))
        }
        root.addView(text("Steam 令牌", 22f, colorText, bold = true))

        listContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(12), 0, dp(96))   // 给右下角悬浮按钮留位置
        }
        root.addView(ScrollView(this).apply { addView(listContainer) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        root.addView(text("点卡片复制动态码 · 长按改名/删除", 11f, colorDim).apply {
            gravity = Gravity.CENTER
            setPadding(0, dp(6), 0, 0)
        })

        val fab = TextView(this).apply {
            text = "＋"
            textSize = 30f
            setTextColor(Color.parseColor(colorOnAccent))
            gravity = Gravity.CENTER
            background = rounded(colorAccent, 28)
            setOnClickListener { showActionsSheet() }
        }

        val frame = FrameLayout(this).apply {
            setBackgroundColor(Color.parseColor(colorBg))
            addView(root, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            addView(fab, FrameLayout.LayoutParams(dp(56), dp(56)).apply {
                gravity = Gravity.BOTTOM or Gravity.END
                setMargins(0, 0, dp(20), dp(24))
            })
        }
        setContentView(frame)
        rebuild()
    }

    // ---------------- 底部弹层 ----------------

    /** 通用底部弹层：顶部圆角 + 拖拽条 + 标题 + 自定义内容，从下往上滑入 */
    private fun showSheet(title: String, build: (LinearLayout) -> Unit) {
        val sheet = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(Color.parseColor(colorCard))
                cornerRadii = floatArrayOf(
                    dp(22).toFloat(), dp(22).toFloat(),
                    dp(22).toFloat(), dp(22).toFloat(),
                    0f, 0f, 0f, 0f)
            }
            setPadding(dp(16), dp(10), dp(16), dp(26))
        }
        sheet.addView(View(this).apply { setBackgroundColor(Color.parseColor(colorCardBorder)) },
            LinearLayout.LayoutParams(dp(40), dp(4)).apply {
                gravity = Gravity.CENTER_HORIZONTAL
                bottomMargin = dp(16)
            })
        sheet.addView(text(title, 15f, colorDim).apply { setPadding(dp(4), 0, 0, dp(12)) })
        build(sheet)

        val dialog = Dialog(this)
        dialog.setContentView(sheet)
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setGravity(Gravity.BOTTOM)
            setWindowAnimations(android.R.style.Animation_InputMethod)   // 从下往上滑入
            attributes = attributes.apply {
                dimAmount = 0.6f
                addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            }
        }
        sheetDialog = dialog
        dialog.show()
    }

    private fun fullWidthMargin(bottomDp: Int): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            bottomMargin = dp(bottomDp)
        }

    private fun sheetRow(label: String, onClick: () -> Unit): TextView = TextView(this).apply {
        text = label
        textSize = 16f
        setTextColor(Color.parseColor(colorText))
        background = rounded(colorBg, 12)
        setPadding(dp(18), dp(17), dp(18), dp(17))
        setOnClickListener { onClick() }
    }

    private fun sheetInput(hintText: String, multiline: Boolean = false): EditText = EditText(this).apply {
        hint = hintText
        textSize = 15f
        setTextColor(Color.parseColor(colorText))
        setHintTextColor(Color.parseColor(colorDim))
        background = rounded(colorBg, 12, colorCardBorder, 1)
        setPadding(dp(14), dp(14), dp(14), dp(14))
        if (multiline) {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 3
        } else {
            setSingleLine()
        }
    }

    /** 右下角＋弹出的操作弹层 */
    private fun showActionsSheet() {
        showSheet("添加到令牌列表") { sheet ->
            val options = listOf<Pair<String, () -> Unit>>(
                "✍️   手动添加" to { showAddSheet() },
                "📷   扫描二维码" to { startScan() },
                "🖼️   图片导入" to { pickQrImage() },
                "ℹ️   关于" to { showAboutSheet() }
            )
            for ((label, action) in options) {
                sheet.addView(sheetRow(label) {
                    sheetDialog?.dismiss()
                    action()
                }, fullWidthMargin(8))
            }
        }
    }

    /** 手动添加弹层 */
    /** 关于 */
    private fun showAboutSheet() {
        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName
        } catch (e: Exception) {
            "未知"
        }
        showSheet("关于") { sheet ->
            sheet.addView(text("Steam 令牌生成器", 18f, colorText, bold = true).apply { setPadding(dp(4), 0, 0, dp(4)) }, fullWidthMargin(0))
            sheet.addView(text("版本 " + version + " · GPL-3.0-only", 13f, colorDim).apply { setPadding(dp(4), 0, 0, dp(12)) }, fullWidthMargin(0))
            val body = "本地保存 Steam 令牌密钥并生成 5 位动态码。\n" +
                "密钥用 Android Keystore 加密存放于本机，应用不联网、不上传任何数据。\n" +
                "由 zxing / zxing-android-embedded（Apache-2.0）提供二维码能力。\n" +
                "项目地址：github.com/No-Zero-issue/AccountManager"
            sheet.addView(text(body, 13f, colorDim).apply { setPadding(dp(4), 0, dp(4), dp(14)) }, fullWidthMargin(0))
            val copy = button("复制项目地址", colorAccent, colorOnAccent)
            copy.setOnClickListener {
                (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager)
                    .setPrimaryClip(ClipData.newPlainText("url", "https://github.com/No-Zero-issue/AccountManager"))
                toast("已复制")
            }
            sheet.addView(copy, fullWidthMargin(0))
        }
    }

    private fun showAddSheet() {
        showSheet("手动添加账号") { sheet ->
            val nameInput = sheetInput("名称（可留空）")
            val secretInput = sheetInput("粘贴 shared_secret / .maFile 内容 / otpauth 链接", multiline = true)
            sheet.addView(nameInput, fullWidthMargin(8))
            sheet.addView(secretInput, fullWidthMargin(14))

            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            val confirm = button("导入", colorAccent, colorOnAccent)
            confirm.setOnClickListener {
                val raw = secretInput.text.toString().trim()
                if (raw.isEmpty()) {
                    toast("请先粘贴密钥内容")
                } else {
                    sheetDialog?.dismiss()
                    importFromText(raw, nameInput.text.toString().trim().ifEmpty { "未命名" })
                }
            }
            val cancel = button("取消")
            cancel.setOnClickListener { sheetDialog?.dismiss() }
            row.addView(confirm, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(LinearLayout(this).apply { setPadding(dp(8), 0, 0, 0) })
            row.addView(cancel, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            sheet.addView(row)
        }
    }

    // ---------------- 列表 ----------------

    private fun rebuild() {
        listContainer.removeAllViews()
        rows.clear()
        if (entries.isEmpty()) {
            listContainer.addView(text("还没有账号。点右下角＋添加，或从二维码导入。", 13f, colorDim).apply {
                gravity = Gravity.CENTER
                setPadding(dp(8), dp(40), dp(8), dp(40))
            })
        }
        for (e in entries) {
            listContainer.addView(buildCard(e))
        }
        tick()
    }

    private fun buildCard(e: Entry): View {
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = rounded(colorCard, 14, colorCardBorder, 1)
            setPadding(dp(14), dp(12), dp(14), dp(12))
        }
        card.addView(text(e.name, 15f, colorText, bold = true))

        val codeRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val codeView = text("·····", 34f, colorAccent, bold = true)
        codeView.letterSpacing = 0.12f
        val leftView = text("30s", 13f, colorDim)
        leftView.gravity = Gravity.END
        codeRow.addView(codeView, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        codeRow.addView(leftView)
        card.addView(codeRow)

        val progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = SteamGuard.PERIOD
            this.progress = SteamGuard.PERIOD
        }
        card.addView(progress, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4)).apply {
            topMargin = dp(8)
        })

        card.setOnClickListener {
            val code = SteamGuard.currentCode(e.secret)
            (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("steam", code))
            toast("已复制 " + code)
        }
        card.setOnLongClickListener {
            showRowMenu(e)
            true
        }

        rows.add(Row(codeView, leftView, progress, e))

        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, dp(10))
            addView(card)
        }
    }

    private fun tick() {
        val now = System.currentTimeMillis()
        val left = SteamGuard.secondsRemaining(now)
        val seconds = left.toString() + "s"
        for (row in rows) {
            row.code.text = SteamGuard.currentCode(row.entry.secret, now)
            row.code.setTextColor(Color.parseColor(if (left <= 5) colorWarn else colorAccent))
            row.remain.text = seconds
            row.remain.setTextColor(Color.parseColor(if (left <= 5) colorWarn else colorDim))
            row.bar.progress = left
            row.bar.progressDrawable.setTint(Color.parseColor(if (left <= 5) colorWarn else colorAccent))
        }
        handler.postDelayed({ tick() }, 1000)
    }

    // ---------------- 数据 ----------------

    private fun loadEntries() {
        val plain = store.load() ?: return
        try {
            val arr = JSONArray(plain)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                entries.add(Entry(o.optString("name", "未命名"), o.getString("secret"), o.optString("revocation").ifEmpty { null }))
            }
        } catch (e: Exception) {
            toast("本地数据解析失败，已忽略")
        }
    }

    private fun persist() {
        val arr = JSONArray()
        for (e in entries) {
            arr.put(JSONObject().apply {
                put("name", e.name)
                put("secret", e.secret)
                e.revocation?.let { put("revocation", it) }
            })
        }
        try {
            store.save(arr.toString())
        } catch (e: Exception) {
            toast("保存失败：" + (e.message ?: "未知错误"))
        }
    }

    private fun importFromText(raw: String, fallbackName: String) {
        try {
            val parsed = SecretParser.parse(raw)
            val name = parsed.name ?: fallbackName
            if (entries.any { it.secret == parsed.secretBase64 }) {
                toast("这个密钥已经添加过了")
                return
            }
            entries.add(Entry(name, parsed.secretBase64, parsed.revocationCode))
            persist()
            rebuild()
            toast("已导入：" + name)
        } catch (e: Exception) {
            toast("内容无法识别为密钥：" + (e.message ?: "未知错误"))
        }
    }

    // ---------------- 导入入口 ----------------

    private fun pickQrImage() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "image/*"
        }
        startActivityForResult(intent, REQ_PICK_QR)
    }

    private fun startScan() {
        IntentIntegrator(this)
            .setDesiredBarcodeFormats(IntentIntegrator.QR_CODE)
            .setPrompt("把二维码放进框内")
            .setBeepEnabled(false)
            .setOrientationLocked(false)
            .initiateScan()
    }

    // ---------------- 条目操作 ----------------

    private fun showRowMenu(e: Entry) {
        showSheet(e.name) { sheet ->
            sheet.addView(sheetRow("✏️   重命名") {
                sheetDialog?.dismiss()
                renameEntry(e)
            }, fullWidthMargin(8))
            sheet.addView(sheetRow("🗑️   删除") {
                sheetDialog?.dismiss()
                confirmDelete(e)
            }, fullWidthMargin(8))
            val revocation = e.revocation
            if (revocation != null) {
                sheet.addView(sheetRow("🔑   查看撤销码") {
                    sheetDialog?.dismiss()
                    showText("撤销码", revocation)
                }, fullWidthMargin(8))
            }
        }
    }
    private fun renameEntry(e: Entry) {
        showSheet("重命名账号") { sheet ->
            val input = sheetInput("名称")
            input.setText(e.name)
            sheet.addView(input, fullWidthMargin(14))
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            val save = button("保存", colorAccent, colorOnAccent)
            save.setOnClickListener {
                e.name = input.text.toString().ifBlank { "未命名" }
                persist()
                rebuild()
                sheetDialog?.dismiss()
            }
            val cancel = button("取消")
            cancel.setOnClickListener { sheetDialog?.dismiss() }
            row.addView(save, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(LinearLayout(this).apply { setPadding(dp(8), 0, 0, 0) })
            row.addView(cancel, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            sheet.addView(row)
        }
    }
    private fun confirmDelete(e: Entry) {
        showSheet("删除账号") { sheet ->
            sheet.addView(text("确定删除「" + e.name + "」吗？删除后需要重新导入密钥。", 14f, colorDim).apply {
                setPadding(dp(4), 0, dp(4), dp(14))
            }, fullWidthMargin(0))
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            val del = button("删除", colorDanger, "#ffffff")
            del.setOnClickListener {
                entries.remove(e)
                persist()
                rebuild()
                sheetDialog?.dismiss()
                toast("已删除")
            }
            val cancel = button("取消")
            cancel.setOnClickListener { sheetDialog?.dismiss() }
            row.addView(del, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(LinearLayout(this).apply { setPadding(dp(8), 0, 0, 0) })
            row.addView(cancel, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            sheet.addView(row)
        }
    }
    private fun showText(title: String, body: String) {
        showSheet(title) { sheet ->
            sheet.addView(text(body, 18f, colorAccent, bold = true).apply {
                setPadding(dp(4), 0, dp(4), dp(16))
            }, fullWidthMargin(0))
            val copy = button("复制", colorAccent, colorOnAccent)
            copy.setOnClickListener {
                (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("steam", body))
                toast("已复制")
            }
            sheet.addView(copy, fullWidthMargin(0))
        }
    }
    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    companion object {
        private const val REQ_PICK_QR = 1001
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)

        val scanResult = IntentIntegrator.parseActivityResult(requestCode, resultCode, data)
        if (scanResult != null) {
            val contents = scanResult.contents
            if (contents.isNullOrEmpty()) toast("已取消扫码") else importFromText(contents, "扫码导入")
            return
        }

        if (requestCode != REQ_PICK_QR || resultCode != RESULT_OK) return
        val uri = data?.data
        if (uri == null) {
            toast("没有选择图片")
            return
        }
        val bitmap = try {
            contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
        } catch (e: Exception) {
            null
        }
        if (bitmap == null) {
            toast("图片读取失败")
            return
        }
        val w = bitmap.width
        val h = bitmap.height
        val pixels = IntArray(w * h)
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h)
        val decoded = QrDecoder.decodeArgb(pixels, w, h)
        if (decoded == null) {
            toast("图片里没识别出二维码")
            return
        }
        importFromText(decoded, "图片导入")
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }
}
