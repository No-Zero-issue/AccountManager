package com.accountmanager.steamguard

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.net.Uri
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Point
import android.graphics.Rect
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.DragEvent
import android.view.MotionEvent
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.google.zxing.integration.android.IntentIntegrator
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : Activity() {

    private class Entry(
        var name: String,
        var secret: String,
        var revocation: String? = null,
        var type: String = "steam",
        var digits: Int = 6,
        var period: Int = 30,
        var algorithm: String = "SHA1"
    )

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
    /** 唯一的每秒刷新循环：避免每次重建界面都新起一个循环 */
    private val ticker = object : Runnable {
        override fun run() {
            tick()
            handler.postDelayed(this, 1000)
        }
    }
    private lateinit var store: SecretStore
    private lateinit var listContainer: LinearLayout
    private var sheetDialog: Dialog? = null

    // ---- 「从电脑同步」：收集到的分页（页码 -> 该页内容），凑齐后一次性合并 ----
    private val syncPages = sortedMapOf<Int, SyncPackage.Page>()
    private var syncTotal = 0

    // ---- 拖动排序状态（方案 B：跟手浮层 + 原位空槽 + 实时挤压） ----
    /** 正在被拖动的条目 */
    private var dragEntry: Entry? = null
    /** 被拖动条目的槽位：保持挂载，只把高度收成 0（移除视图会让系统取消拖动） */
    private var dragSlot: View? = null
    /** 目标位置的空槽：它在列表里实时移动，其它卡片被挤开 */
    private var dragGap: View? = null
    /** 空槽当前代表的下标（不含被拖卡片本身） */
    private var dragIndex = -1
    private var dragMoved = false
    /** 拖动结束后刷新界面（顺序有变化时才调用） */
    private var dragOnDone: (() -> Unit)? = null

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
        // 顶栏：标题 + 右上角圆形「i」关于按钮
        val topBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        topBar.addView(text("2FA", 22f, colorText, bold = true),
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        // 右上角三条杆按钮（画出来的横线，不依赖字体字形）→ 菜单：编辑卡片 / 关于
        val menuButton = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            background = rounded(colorCard, 10, colorCardBorder, 1)
            setPadding(dp(9), dp(7), dp(9), dp(7))
        }
        menuButton.setOnClickListener { showDropdownMenu(menuButton) }
        repeat(3) {
            menuButton.addView(View(this).apply { setBackgroundColor(Color.parseColor(colorText)) },
                LinearLayout.LayoutParams(dp(18), dp(2)).apply { bottomMargin = dp(3) })
        }
        topBar.addView(menuButton, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(38)))
        root.addView(topBar)

        listContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(12), 0, dp(96))   // 给右下角悬浮按钮留位置
        }
        enableDragSort(listContainer)
        root.addView(ScrollView(this).apply { addView(listContainer) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        root.addView(text("点卡片复制动态码 · 长按改名/删除 · 按住右侧手柄拖动排序", 11f, colorDim).apply {
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
        handler.post(ticker)
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

    /** 菜单行：扁平样式（无圆角块、无边框），按下时整行高亮 —— 类似桌面右键菜单 */
    private fun sheetRow(label: String, onClick: () -> Unit): TextView = TextView(this).apply {
        text = label
        textSize = 15.5f
        setTextColor(Color.parseColor(colorText))
        setPadding(dp(16), dp(15), dp(16), dp(15))
        isClickable = true
        background = android.graphics.drawable.StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed),
                android.graphics.drawable.ColorDrawable(Color.parseColor("#2c3343")))
            addState(intArrayOf(), android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
        }
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
                "手动添加" to { showAddSheet() },
                "扫描二维码" to { startScan() },
                "图片导入" to { pickQrImage() },
            )
            for ((label, action) in options) {
                sheet.addView(sheetRow(label) {
                    sheetDialog?.dismiss()
                    action()
                }, fullWidthMargin(8))
            }
        }
    }

    /** 右上角三条杆的下拉面板（圆角、行间分隔线） */
    private fun showDropdownMenu(anchor: View) {
        var popup: PopupWindow? = null
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(Color.parseColor(colorCard))
                cornerRadius = dp(10).toFloat()
                setStroke(dp(1), Color.parseColor(colorCardBorder))
            }
            setPadding(0, dp(6), 0, dp(6))
        }
        val items = mutableListOf<Pair<String, () -> Unit>>(
            "编辑卡片" to { showEditSheet() },
            "从电脑同步" to { startSync() }
        )
        if (syncPages.isNotEmpty()) {
            items.add("取消未完成的同步（${syncPages.size}/$syncTotal）" to { cancelSync() })
        }
        items.add("关于" to { showAboutSheet() })
        items.forEachIndexed { index, item ->
            val label = item.first
            val action = item.second
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(12), dp(12), dp(14), dp(12))
                isClickable = true
                background = android.graphics.drawable.StateListDrawable().apply {
                    addState(intArrayOf(android.R.attr.state_pressed),
                        ColorDrawable(Color.parseColor("#2c3343")))
                    addState(intArrayOf(), ColorDrawable(Color.TRANSPARENT))
                }
                setOnClickListener {
                    popup?.dismiss()
                    action()
                }
            }
            row.addView(text(label, 15f, colorText),
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            panel.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            if (index != items.lastIndex) {
                panel.addView(View(this).apply { setBackgroundColor(Color.parseColor(colorCardBorder)) },
                    LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)).apply {
                        leftMargin = dp(14)
                        rightMargin = dp(14)
                    })
            }
        }
        popup = PopupWindow(panel, ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, true).apply {
            isOutsideTouchable = true
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            elevation = dp(8).toFloat()
        }
        // 让面板右边缘离屏幕右侧 12dp（左边缘也不越界）
        panel.measure(
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED)
        )
        val panelWidth = panel.measuredWidth
        val anchorPos = IntArray(2)
        anchor.getLocationOnScreen(anchorPos)
        val screenWidth = resources.displayMetrics.widthPixels
        val margin = dp(12)
        var targetLeft = screenWidth - margin - panelWidth
        if (targetLeft < margin) targetLeft = margin
        popup.showAsDropDown(anchor, targetLeft - anchorPos[0], dp(6))
    }

    /** 编辑卡片：左侧手柄按住拖动排序（跟手浮层 + 空槽预览，松手即保存）；点文字进入改名/删除菜单 */
    private fun showEditSheet() {
        showSheet("编辑卡片（按住左侧手柄拖动排序）") { sheet ->
            val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            enableDragSort(box)
            if (entries.isEmpty()) {
                box.addView(text("还没有卡片，先用右下角＋添加。", 14f, colorDim).apply {
                    setPadding(dp(4), 0, dp(4), dp(12))
                }, fullWidthMargin(0))
            } else {
                entries.forEach { e ->
                    val kind = if (e.type == "totp") "6 位数字" else "Steam 5 位"

                    val handle = dragHandle()
                    val label = text(e.name + "    " + kind, 15.5f, colorText).apply {
                        setPadding(dp(4), dp(14), dp(14), dp(14))
                        isClickable = true
                        setOnClickListener {
                            sheetDialog?.dismiss()
                            showRowMenu(e)
                        }
                    }

                    val row = LinearLayout(this).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER_VERTICAL
                        addView(handle, LinearLayout.LayoutParams(dp(46), dp(44)))
                        addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                    }
                    handle.setOnTouchListener { _, ev ->
                        if (ev.actionMasked == MotionEvent.ACTION_DOWN) {
                            beginSlotDrag(box, row, e, ev) {
                                sheetDialog?.dismiss()
                                showEditSheet()
                            }
                        } else {
                            false
                        }
                    }
                    box.addView(row, fullWidthMargin(6))
                }
            }
            sheet.addView(box, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
    }

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
            val open = button("打开项目主页", colorAccent, colorOnAccent)
            open.setOnClickListener {
                try {
                    startActivity(Intent(Intent.ACTION_VIEW,
                        Uri.parse("https://github.com/No-Zero-issue/AccountManager")))
                } catch (e: Exception) {
                    toast("没有可用的浏览器")
                }
            }
            sheet.addView(open, fullWidthMargin(0))
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

    // ---------------- 拖动排序（方案 B） ----------------

    /** 方案 B 的跟手浮层：先把卡片画成位图，再放大一点点绘制，形成「浮起来」的预览 */
    private inner class FloatingShadow(
        private val shot: Bitmap,
        private val touchX: Float,
        private val touchY: Float
    ) : View.DragShadowBuilder() {

        override fun onProvideShadowMetrics(outShadowSize: Point, outShadowTouchPoint: Point) {
            val w = (shot.width * DRAG_SCALE).toInt()
            val h = (shot.height * DRAG_SCALE).toInt()
            outShadowSize.set(w, h)
            outShadowTouchPoint.set(
                (touchX * DRAG_SCALE).toInt().coerceIn(0, w),
                (touchY * DRAG_SCALE).toInt().coerceIn(0, h)
            )
        }

        override fun onDrawShadow(canvas: Canvas) {
            val w = (shot.width * DRAG_SCALE).toInt()
            val h = (shot.height * DRAG_SCALE).toInt()
            val paint = Paint(Paint.FILTER_BITMAP_FLAG)
            paint.alpha = 246
            canvas.drawBitmap(shot, null, Rect(0, 0, w, h), paint)
        }
    }

    /** 自绘拖动手柄：两条横线，不依赖字体字形（固定尺寸 + 自己垂直居中，避免子视图被压扁） */
    private fun dragHandle(): View {
        val barW = dp(18)
        val barH = dp(2)
        val gap = dp(5)
        return object : View(this) {
            private val paint = Paint().apply { color = Color.parseColor(colorDim) }

            override fun onDraw(canvas: Canvas) {
                val left = ((width - barW) / 2).toFloat()
                val top = ((height - (barH * 2 + gap)) / 2).toFloat()
                canvas.drawRect(left, top, left + barW, top + barH, paint)
                canvas.drawRect(left, top + barH + gap, left + barW, top + barH + gap + barH, paint)
            }
        }
    }

    /** 把某个槽位的外层容器接上拖动排序（主列表 / 编辑弹层共用） */
    private fun enableDragSort(container: LinearLayout) {
        container.setOnDragListener { _, event -> onDragEvent(container, event) }
    }

    private fun onDragEvent(container: LinearLayout, event: DragEvent): Boolean {
        when (event.action) {
            DragEvent.ACTION_DRAG_STARTED -> return dragEntry != null
            DragEvent.ACTION_DRAG_LOCATION -> {
                if (dragEntry != null) moveGapTo(container, event.y)
                return true
            }
            DragEvent.ACTION_DROP -> return true
            DragEvent.ACTION_DRAG_ENDED -> {
                // 松手落点在可放置区域才提交，被系统取消则回到原顺序
                if (dragEntry != null) finishSlotDrag(container, event.result)
                return true
            }
            else -> return true
        }
    }

    /** 把卡片截成位图，给跟手浮层用 */
    private fun captureSlot(view: View): Bitmap? {
        val w = view.width
        val h = view.height
        if (w <= 0 || h <= 0) return null
        return try {
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { view.draw(Canvas(it)) }
        } catch (e: Exception) {
            null
        }
    }

    /**
     * 从手柄发起拖动：
     * 卡片本身跟手浮动（方案 B），原位塌陷成空位，目标位置用空槽预览，其它卡片实时让位。
     */
    private fun beginSlotDrag(container: LinearLayout, slot: View, e: Entry, ev: MotionEvent, onDone: () -> Unit): Boolean {
        val from = entries.indexOf(e)
        if (from < 0) return false

        val loc = IntArray(2)
        slot.getLocationOnScreen(loc)
        val shot = captureSlot(slot)
        val shadow = if (shot != null)
            FloatingShadow(shot, ev.rawX - loc[0], ev.rawY - loc[1])
        else
            View.DragShadowBuilder(slot)

        val gap = View(this).apply {
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#141821"))
                cornerRadius = dp(14).toFloat()
                setStroke(dp(1), Color.parseColor("#39435a"))
            }
        }
        val gapLp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, slot.height)
        (slot.layoutParams as? LinearLayout.LayoutParams)?.let {
            gapLp.topMargin = it.topMargin
            gapLp.bottomMargin = it.bottomMargin
        }

        dragEntry = e
        dragSlot = slot
        dragGap = gap
        dragIndex = from
        dragMoved = false
        dragOnDone = onDone

        if (!slot.startDragAndDrop(null, shadow, e, 0)) {
            dragEntry = null
            dragSlot = null
            dragGap = null
            dragIndex = -1
            dragOnDone = null
            return false
        }

        // 目标位置的空槽先落在原位，随后跟着手指走
        container.addView(gap, (container.indexOfChild(slot) + 1).coerceAtMost(container.childCount), gapLp)
        // 源槽塌陷：视图保持挂载，只把高度收成 0，避免拖动被系统取消
        slot.visibility = View.INVISIBLE
        slot.layoutParams = slot.layoutParams.apply { height = 0 }
        slot.requestLayout()
        return true
    }

    /** 手指移动时把空槽挪到对应位置，其它卡片自动让位（实时挤压） */
    private fun moveGapTo(container: LinearLayout, fingerY: Float) {
        val slot = dragSlot ?: return
        val gap = dragGap ?: return
        if (gap.parent !== container) return

        var k = 0
        for (i in 0 until container.childCount) {
            val c = container.getChildAt(i)
            if (c === gap || c === slot) continue
            if (fingerY > c.top + c.height / 2f) k++
        }
        if (k == dragIndex) return

        // 目标下标按「不含被拖卡片」的顺序计算
        val others = ArrayList<View>(container.childCount)
        for (i in 0 until container.childCount) {
            val c = container.getChildAt(i)
            if (c !== gap && c !== slot) others.add(c)
        }
        if (k > others.size) k = others.size

        dragIndex = k
        dragMoved = true
        container.removeView(gap)
        if (k < others.size) container.addView(gap, container.indexOfChild(others[k])) else container.addView(gap)
    }

    /** 结束拖动：撤掉空槽、恢复源槽；顺序变了就保存并刷新 */
    private fun finishSlotDrag(container: LinearLayout, drop: Boolean) {
        val e = dragEntry
        val slot = dragSlot
        val gap = dragGap
        val target = dragIndex
        val moved = dragMoved

        dragEntry = null
        dragSlot = null
        dragGap = null
        dragIndex = -1
        dragMoved = false

        if (gap != null && gap.parent === container) container.removeView(gap)
        if (slot != null) {
            slot.visibility = View.VISIBLE
            slot.alpha = 1f
            slot.layoutParams = slot.layoutParams.apply { height = ViewGroup.LayoutParams.WRAP_CONTENT }
            slot.requestLayout()
        }

        var changed = false
        if (drop && moved && e != null && target >= 0) {
            val from = entries.indexOf(e)
            if (from >= 0 && target != from) {
                entries.removeAt(from)
                entries.add(target.coerceIn(0, entries.size), e)
                persist()
                changed = true
            }
        }
        val done = dragOnDone
        dragOnDone = null
        if (changed) done?.invoke()
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
            val slot = buildCard(e)
            listContainer.addView(slot, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(10)
            })
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
        // 数字旁边的拖动手柄：按住它直接拖动排序
        val handle = dragHandle().apply {
            setOnTouchListener { _, ev ->
                if (ev.actionMasked == MotionEvent.ACTION_DOWN) {
                    beginSlotDrag(listContainer, card, e, ev) { rebuild() }
                } else {
                    false
                }
            }
        }
        codeRow.addView(codeView, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        codeRow.addView(leftView)
        codeRow.addView(handle, LinearLayout.LayoutParams(dp(46), dp(40)))
        card.addView(codeRow)

        val progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = e.period
            this.progress = e.period
        }
        card.addView(progress, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4)).apply {
            topMargin = dp(8)
        })

        card.setOnClickListener {
            val code = if (e.type == "totp") Totp.currentCode(e.secret, System.currentTimeMillis(), e.digits, e.period, e.algorithm) else SteamGuard.currentCode(e.secret)
            (getSystemService(CLIPBOARD_SERVICE) as ClipboardManager).setPrimaryClip(ClipData.newPlainText("steam", code))
            toast("已复制 " + code)
        }
        card.setOnLongClickListener {
            showRowMenu(e)
            true
        }

        rows.add(Row(codeView, leftView, progress, e))

        return card
    }

    private fun tick() {
        val now = System.currentTimeMillis()
        for (row in rows) {
            val period = if (row.entry.period > 0) row.entry.period else 30
            val remain = period - ((now / 1000) % period).toInt()
            row.code.text = if (row.entry.type == "totp")
                Totp.currentCode(row.entry.secret, now, row.entry.digits, period, row.entry.algorithm)
            else
                SteamGuard.currentCode(row.entry.secret, now)
            val tint = if (remain <= 5) colorWarn else colorAccent
            row.code.setTextColor(Color.parseColor(tint))
            row.remain.text = remain.toString() + "s"
            row.remain.setTextColor(Color.parseColor(if (remain <= 5) colorWarn else colorDim))
            row.bar.max = period
            row.bar.progress = remain
            row.bar.progressDrawable.setTint(Color.parseColor(tint))
        }
    }

    // ---------------- 数据 ----------------

    private fun loadEntries() {
        val loaded = store.load()
        val plain = loaded.text ?: return

        // 先整份解析成功，再替换列表。
        // 以前是「边解析边 add」，一旦某条记录坏掉就会留下半截列表，
        // 之后任何一次保存（改名 / 拖动 / 导入 / 删除）都会把半截写回去 —— 等于永久丢令牌。
        val parsed = ArrayList<Entry>()
        try {
            val arr = JSONArray(plain)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val name = o.optString("name", "未命名")
                val secret = o.getString("secret")
                val revocation = o.optString("revocation").ifEmpty { null }
                parsed.add(Entry(name, secret, revocation,
                    o.optString("type", "steam"),
                    o.optInt("digits", 6),
                    o.optInt("period", 30),
                    o.optString("algorithm", "SHA1")))
            }
        } catch (e: Exception) {
            // 解析失败就保持现状（不清空、不保存），文件原样留着，避免越弄越糟
            toast("本地数据解析失败，已保持原样（未写入任何改动）")
            return
        }

        entries.clear()
        entries.addAll(parsed)
        if (loaded.fromBackup) toast("主数据损坏，已从备份恢复（可能少了最后一次改动）")
    }

    private fun persist() {
        val arr = JSONArray()
        for (e in entries) {
            arr.put(JSONObject().apply {
                put("name", e.name)
                put("secret", e.secret)
e.revocation?.let { put("revocation", it) }
                put("type", e.type)
                put("digits", e.digits)
                put("period", e.period)
                put("algorithm", e.algorithm)
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
            entries.add(Entry(name, parsed.secretBase64, parsed.revocationCode, parsed.type, parsed.digits, parsed.period, parsed.algorithm))
            persist()
            rebuild()
            toast("已导入：" + name)
        } catch (e: Exception) {
            toast("内容无法识别为密钥：" + (e.message ?: "未知错误"))
        }
    }

    // ---------------- 从电脑同步（2FA 打包二维码） ----------------

    /** 菜单入口：开始 / 继续一次同步（已收到的分页保留，方便中断后再扫） */
    private fun startSync() {
        if (syncPages.isEmpty()) {
            toast("对准电脑端「同步到手机」里的二维码")
        } else {
            toast("继续同步：已收到 ${syncPages.size}/$syncTotal 张")
        }
        startScan()
    }

    private fun cancelSync() {
        syncPages.clear()
        syncTotal = 0
        toast("已取消未完成的同步")
    }

    /** 扫码 / 图片导入的统一入口：同步包走合并流程，其它内容按单条密钥导入 */
    private fun handleImportedText(raw: String, fallbackName: String, autoContinue: Boolean) {
        if (SyncPackage.isPage(raw)) {
            acceptSyncPage(raw, autoContinue)
            return
        }
        importFromText(raw, fallbackName)
    }

    /**
     * 收下一页同步包：没凑齐就提示继续扫（相机扫码会自动接着弹扫码界面），
     * 凑齐后按「密钥已存在就跳过」合并 —— 只新增，绝不删改手机上已有的条目。
     */
    private fun acceptSyncPage(raw: String, autoContinue: Boolean) {
        val page = try {
            SyncPackage.parse(raw)
        } catch (e: Exception) {
            toast("同步包无法识别：" + (e.message ?: "格式错误"))
            return
        }

        if (syncTotal != page.total) {   // 换成另一套同步包（总页数不同）→ 重新收集
            syncPages.clear()
            syncTotal = page.total
        }
        if (syncPages.containsKey(page.index)) {
            toast("第 ${page.index}/${page.total} 张已经扫过了")
        } else {
            syncPages[page.index] = page
        }

        if (syncPages.size < syncTotal) {
            toast("已收到 ${syncPages.size}/$syncTotal 张，继续扫下一张")
            if (autoContinue) handler.postDelayed({ startScan() }, 600)
            return
        }

        val incoming = (1..syncTotal).flatMap { syncPages[it]?.items ?: emptyList() }
        syncPages.clear()
        syncTotal = 0

        var added = 0
        var skipped = 0
        for (item in incoming) {
            if (entries.any { it.secret == item.secretBase64 }) {
                skipped++
                continue
            }
            entries.add(Entry(item.name, item.secretBase64, item.revocation,
                item.type, item.digits, item.period, item.algorithm))
            added++
        }
        if (added > 0) persist()
        rebuild()
        toast(if (skipped > 0) "同步完成：新增 $added 条，已存在 $skipped 条" else "同步完成：新增 $added 条")
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
            sheet.addView(sheetRow("重命名") {
                sheetDialog?.dismiss()
                renameEntry(e)
            }, fullWidthMargin(8))
            sheet.addView(sheetRow("删除") {
                sheetDialog?.dismiss()
                confirmDelete(e)
            }, fullWidthMargin(8))
            val revocation = e.revocation
            if (revocation != null) {
                sheet.addView(sheetRow("查看撤销码") {
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
        /** 跟手浮层的放大比例，视觉上「浮起来」 */
        private const val DRAG_SCALE = 1.03f
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)

        val scanResult = IntentIntegrator.parseActivityResult(requestCode, resultCode, data)
        if (scanResult != null) {
            val contents = scanResult.contents
            when {
                contents.isNullOrEmpty() ->
                    toast(if (syncTotal > 0) "同步已暂停（已收到 ${syncPages.size}/$syncTotal 张）" else "已取消扫码")
                else -> handleImportedText(contents, "扫码导入", true)
            }
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
        // 图片导入不自动接着弹扫码（可能是在相册里逐张选），收齐了才合并
        handleImportedText(decoded, "图片导入", false)
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }
}
