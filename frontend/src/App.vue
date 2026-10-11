<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Copyright (C) 2026 Account Manager 开发者 -->
<script setup>
import { ref, onMounted, onUnmounted, computed, watch, nextTick } from 'vue';
import jsQR from 'jsqr';
import QRCode from 'qrcode';

// ============================================================ 保险库状态
const vaultState = ref('loading');   // loading | setup | locked | unlocked
const masterPassword = ref('');
const unlockPasswordInput = ref(null);   // 锁屏：未启用 Hello 时自动聚焦到主密码框

// 把焦点放回锁屏主密码框。Windows Hello 弹窗关闭时系统可能把焦点抢回窗口本身，
// 所以这里立即尝试，并在 300ms / 800ms 后再各试一次。
const focusUnlockPassword = () => {
  const tryFocus = () => nextTick(() => { if (unlockPasswordInput.value) unlockPasswordInput.value.focus(); });
  tryFocus();
  setTimeout(tryFocus, 300);
  setTimeout(tryFocus, 800);
};
const confirmPassword = ref('');
const unlockError = ref('');
const helloEnabled = ref(false);   // 是否已启用 Windows Hello 解锁
const helloBusy = ref(false);      // 正在等待 Windows 安全中心验证

// 进入锁定状态时优先弹出 Windows Hello 验证（用户取消或验证失败再输主密码）
watch(vaultState, (state) => {
  if (state !== 'locked') return;
  if (!helloEnabled.value) {
    // 未启用 Windows Hello：自动聚焦主密码输入框，方便直接输入
    focusUnlockPassword();
    return;
  }
  setTimeout(() => {
    if (vaultState.value === 'locked' && helloEnabled.value && !helloBusy.value) helloUnlock();
  }, 250);
});

// Windows Hello 被手动取消或验证失败后（busy 由 true 变 false 且仍在锁定态）自动聚焦主密码框
watch(helloBusy, (busy, wasBusy) => {
  if (wasBusy && !busy && vaultState.value === 'locked') {
    focusUnlockPassword();
  }
});
const showMasterPassword = ref(false);
const entries = ref([]);
const isBusy = ref(false);
const statusText = ref('就绪');
let isLoadingEntries = false;         // 载入保险库时抑制自动保存

// ============================================================ 界面状态
const isDarkTheme = ref(localStorage.getItem('theme') !== 'light');
const isSidebarCollapsed = ref(false);
const currentFilter = ref('all');     // all | website | software | cat:<分类>
const searchQuery = ref('');
const searchInput = ref(null);
const selectedIds = ref(new Set());
const visiblePasswords = ref({});
const contextMenu = ref({ show: false, x: 0, y: 0, entry: null });
const showSettings = ref(false);
const settingsMenuPos = ref({ x: 0, y: 0 });
const showAbout = ref(false);
const showEditPanel = ref(false);
const editingEntry = ref(null);
const isNewEntry = ref(false);
const deleteTargets = ref(null);
const showChangePassword = ref(false);
const changePasswordForm = ref({ current: '', next: '', confirm: '' });
const changePasswordError = ref('');
const showGenerator = ref(false);
const generator = ref({ length: 16, upper: true, lower: true, digits: true, symbols: true, preview: '' });
const showRecovery = ref(false);      // 恢复代码查看弹窗
const recoveryEntry = ref(null);
const totpCodes = ref({});            // 条目 id -> { code, remaining, period, error }
const qrInput = ref(null);            // 隐藏的图片选择框
const qrDecoding = ref(false);
const qrError = ref('');
const showNewCategory = ref(false);
const newCategoryName = ref('');
const editingCategory = ref(null);    // { original, value } 正在重命名的分类
const categoryToDelete = ref(null);
const steamLoginTarget = ref(null);   // 等待确认登录的 Steam 条目
const steamLoggingIn = ref(false);

// 登录确认弹窗里显示的当前 Steam Guard 码
const steamGuardCodeForTarget = computed(() => {
  const target = steamLoginTarget.value;
  if (!target || target.totpAlgorithm !== 'steam') return '';
  const state = totpCodes.value[target.id];
  return state && state.code ? state.code : '';
});

let lastSelectedId = null;
let saveTimer = null;
let lastSavedSnapshot = '';   // 避免重复加密写入相同内容
let totpTimer = null;         // 每秒刷新 2FA 动态码

// 窗口缩放相关变量
const resizeBorderSize = 8;
const mousePosition = ref({ x: 0, y: 0 });
const isOnEdge = ref(false);
const edgeType = ref('');
const resizeCursor = ref('default');

// ============================================================ 与宿主通信
const hasHost = () => !!(window.chrome && window.chrome.webview);

const post = (message) => {
  if (hasHost()) {
    window.chrome.webview.postMessage(message);
    return true;
  }
  return false;
};

const ERROR_MESSAGES = {
  wrong_password: '主密码错误',
  password_too_short: '主密码至少需要 6 位',
  vault_exists: '保险库已经存在，请直接解锁',
  vault_missing: '找不到 vault.json 文件',
  vault_corrupted: '保险库文件损坏或格式不正确',
  decrypt_failed: '解密失败，文件可能已被修改',
  derive_failed: '密钥派生失败',
  create_failed: '创建保险库失败（检查程序目录是否有写入权限）',
  save_failed: '保存失败（检查程序目录是否有写入权限）',
  locked: '保险库已锁定，请先解锁',
  hello_unsupported: '这台设备不支持 Windows Hello，请改用主密码',
  hello_unavailable: 'Windows Hello 不可用，请先在「设置 → 账户 → 登录选项」中配置 PIN / 指纹 / 人脸',
  hello_canceled: '已取消 Windows 安全中心验证',
  hello_failed: 'Windows Hello 验证失败，请重试或使用主密码',
  hello_not_enrolled: 'Hello 凭据已失效（可能重装系统或修改过 PIN），请用主密码解锁后重新启用',
  hello_key_mismatch: 'Hello 数据与当前保险库不匹配，请用主密码解锁后重新启用',
  hello_corrupted: 'Hello 解锁数据已损坏，请用主密码解锁后重新启用',
  hello_not_enabled: '尚未启用 Windows Hello 解锁',
  hello_busy: '正在等待 Windows 安全中心验证，请稍候'
};

// ============================================================ 计算属性
// 条目类型：website（网站）/ software（软件）/ recovery（恢复代码）/ steam（Steam 账号）
const ENTRY_TYPE_LABELS = { website: '🌐 网站', software: '💻 软件', recovery: '📄 恢复代码', steam: '🎮 Steam 账号' };
const entryType = (entry) => (entry && entry.type ? entry.type : 'website');
const typeLabel = (entry) => ENTRY_TYPE_LABELS[entryType(entry)] || '🌐 网站';

// 分类：categories 是显式保存的列表（可新建空分类），usedCategories 是条目里实际用到的
const categories = ref([]);
const usedCategories = computed(() => {
  const set = new Set();
  entries.value.forEach(item => {
    if (item.category) set.add(item.category);
  });
  return Array.from(set);
});
const allCategories = computed(() => {
  const set = new Set();
  categories.value.forEach(name => { if (name) set.add(name); });
  usedCategories.value.forEach(name => { if (name) set.add(name); });
  return Array.from(set).sort((a, b) => a.localeCompare(b, 'zh-Hans-CN'));
});

const filteredEntries = computed(() => {
  let list = entries.value;
  if (currentFilter.value === 'website') {
    list = list.filter(item => entryType(item) === 'website');
  } else if (currentFilter.value === 'software') {
    list = list.filter(item => entryType(item) === 'software');
  } else if (currentFilter.value === 'steam') {
    list = list.filter(item => entryType(item) === 'steam');
  } else if (currentFilter.value === 'recovery') {
    list = list.filter(item => entryType(item) === 'recovery');
  } else if (currentFilter.value.startsWith('cat:')) {
    const name = currentFilter.value.slice(4);
    list = list.filter(item => (item.category || '') === name);
  }

  const keyword = searchQuery.value.trim().toLowerCase();
  if (keyword) {
    list = list.filter(item => [item.name, item.username, item.url, item.path, item.notes, item.category, item.steamId]
      .some(value => (value || '').toLowerCase().includes(keyword)));
  }
  return list;
});

const viewTitle = computed(() => {
  if (currentFilter.value === 'website') return '🌐 网站密码';
  if (currentFilter.value === 'software') return '💻 软件密码';
  if (currentFilter.value === 'steam') return '🎮 Steam 账号';
  if (currentFilter.value === 'recovery') return '📄 2FA 恢复代码';
  if (currentFilter.value.startsWith('cat:')) return '🏷️ ' + currentFilter.value.slice(4);
  return '📋 全部条目';
});

const recoveryCount = (entry) => splitRecoveryCodes(entry ? entry.recoveryCodes : '').length;

const hasTotp = (entry) => !!(entry && entry.totpSecret && entry.totpSecret.trim());

const hasSelection = computed(() => selectedIds.value.size > 0);

// ============================================================ 保险库操作
const vaultSnapshot = () => JSON.stringify({
  entries: entries.value,
  categories: categories.value
});

// ---- 拖动排序（桌面端表格行）----
// 顺序即 entries 数组顺序，拖完直接持久化进保险库
// 不用 HTML5 拖放（WebView2 里原生拖动不可靠：dragstart 之后拖动不推进、dragend 也不来），
// 改成自己用鼠标事件实现：按住手柄克隆整行做跟手浮层，原行留半透明残影，
// 目标位置用虚线空槽实时预览、其它行实时让位；松开鼠标才写盘
const draggingId = ref(null);      // 正在拖动的行（残影样式 + 逻辑判断）
const dragGapPos = ref(-1);        // 空槽位置：filteredEntries 的下标，-1 表示不显示

let dragClone = null;              // 跟手浮层（外面包一层 table，克隆出来的 tr 才能正常渲染）
let dragOffset = { x: 0, y: 0 };   // 鼠标相对被拖行左上角的偏移
let dragFromIndex = -1;
let dragMovedId = null;
let dragListenersOn = false;
let suppressRowClick = false;      // 拖动刚结束，别把这一次当成点击选中

const dataRowEls = () => Array.from(document.querySelectorAll('table.data-grid > tbody > tr'))
  .filter((r) => !r.classList.contains('drag-gap-row'));

const beginRowDrag = (entry, event) => {
  if (event.button !== 0 || draggingId.value !== null) return;
  const row = event.currentTarget ? event.currentTarget.closest('tr') : null;
  if (!row) return;
  const index = dataRowEls().indexOf(row);
  if (index < 0) return;
  const rect = row.getBoundingClientRect();

  // 先克隆（此时原行还没加残影样式），做跟手浮层
  const wrap = document.createElement('div');
  wrap.className = 'drag-clone';
  wrap.style.width = rect.width + 'px';
  wrap.style.transform = `translate(${rect.left}px, ${rect.top}px)`;
  const table = document.createElement('table');
  table.className = 'data-grid';
  table.style.width = rect.width + 'px';
  const tbody = document.createElement('tbody');
  tbody.appendChild(row.cloneNode(true));
  table.appendChild(tbody);
  wrap.appendChild(table);
  document.body.appendChild(wrap);

  dragClone = wrap;
  dragOffset = { x: event.clientX - rect.left, y: event.clientY - rect.top };
  dragFromIndex = index;
  dragMovedId = entry.id;
  draggingId.value = entry.id;
  dragGapPos.value = index;
  document.body.classList.add('row-dragging-active');
  window.addEventListener('mousemove', onRowDragMove, true);
  window.addEventListener('mouseup', onRowDragUp, true);
  window.addEventListener('keydown', onRowDragKey, true);
  dragListenersOn = true;
  event.preventDefault();
};

const onRowDragMove = (event) => {
  if (dragClone) {
    dragClone.style.transform = `translate(${event.clientX - dragOffset.x}px, ${event.clientY - dragOffset.y}px)`;
  }
  // 看鼠标落在哪一行：上半 → 空槽插到这一行之前，下半 → 之后
  const el = document.elementFromPoint(event.clientX, event.clientY);
  const row = el && el.closest ? el.closest('table.data-grid > tbody > tr') : null;
  if (!row || row.classList.contains('drag-gap-row')) return;   // 停在空槽上位置不变
  const index = dataRowEls().indexOf(row);
  if (index < 0) return;
  const rect = row.getBoundingClientRect();
  dragGapPos.value = event.clientY < rect.top + rect.height / 2 ? index : index + 1;
};

const onRowDragUp = () => finishRowDrag(true);
const onRowDragKey = (event) => { if (event.key === 'Escape') finishRowDrag(false); };

const endRowDrag = () => {
  if (dragClone) { dragClone.remove(); dragClone = null; }
  if (dragListenersOn) {
    window.removeEventListener('mousemove', onRowDragMove, true);
    window.removeEventListener('mouseup', onRowDragUp, true);
    window.removeEventListener('keydown', onRowDragKey, true);
    dragListenersOn = false;
  }
  document.body.classList.remove('row-dragging-active');
  draggingId.value = null;
  dragGapPos.value = -1;
  dragFromIndex = -1;
  dragMovedId = null;
  suppressRowClick = true;
  setTimeout(() => { suppressRowClick = false; }, 0);
};

const finishRowDrag = (commit) => {
  const movedId = dragMovedId;
  const gapPos = dragGapPos.value;
  // 按空槽位置算落点：用可见列表换算，兼容搜索 / 分类筛选
  const visible = filteredEntries.value;
  const ghostPos = visible.findIndex((x) => x.id === movedId);
  const rest = visible.filter((x) => x.id !== movedId);   // 去掉被拖项之后的可见顺序
  let insert = gapPos > ghostPos ? gapPos - 1 : gapPos;
  insert = Math.max(0, Math.min(insert, rest.length));

  endRowDrag();
  if (!commit || ghostPos < 0 || gapPos < 0) return;

  const list = entries.value;
  const from = list.findIndex((x) => x.id === movedId);
  if (from < 0) return;
  const moved = list[from];
  const next = list.filter((x) => x.id !== movedId);
  if (insert >= rest.length) {
    next.push(moved);                                            // 落到最后
  } else {
    const at = next.findIndex((x) => x.id === rest[insert].id);   // 插在这一项之前
    next.splice(at < 0 ? next.length : at, 0, moved);
  }
  if (!next.some((x, i) => x.id !== list[i].id)) return;          // 顺序没变就不写盘
  entries.value = next;
  saveEntries();
};

const saveEntries = () => {
  if (vaultState.value !== 'unlocked') return;
  const data = JSON.parse(JSON.stringify(entries.value));
  const cats = JSON.parse(JSON.stringify(categories.value));
  const snapshot = JSON.stringify({ entries: data, categories: cats });
  if (snapshot === lastSavedSnapshot) return;
  lastSavedSnapshot = snapshot;
  post({ type: 'saveVault', entries: data, categories: cats });
};

const scheduleSave = () => {
  if (vaultState.value !== 'unlocked' || isLoadingEntries) return;
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(saveEntries, 400);
};

watch(entries, scheduleSave, { deep: true });

const createVault = () => {
  unlockError.value = '';
  if (masterPassword.value.length < 6) {
    unlockError.value = '主密码至少需要 6 位';
    return;
  }
  if (masterPassword.value !== confirmPassword.value) {
    unlockError.value = '两次输入的主密码不一致';
    return;
  }
  statusText.value = '正在创建保险库...';
  if (!post({ type: 'createVault', masterPassword: masterPassword.value })) {
    // 浏览器预览模式：直接进入
    vaultState.value = 'unlocked';
  }
};

const unlockVault = () => {
  unlockError.value = '';
  if (!masterPassword.value) {
    unlockError.value = '请输入主密码';
    return;
  }
  statusText.value = '正在解锁...';
  if (!post({ type: 'unlockVault', masterPassword: masterPassword.value })) {
    vaultState.value = 'unlocked';
  }
};

const lockVault = () => {
  post({ type: 'lockVault' });
  if (!hasHost()) {
    handleHostMessage({ data: { type: 'vaultLocked' } });
  }
};

const submitChangePassword = () => {
  changePasswordError.value = '';
  if (changePasswordForm.value.next.length < 6) {
    changePasswordError.value = '新主密码至少需要 6 位';
    return;
  }
  if (changePasswordForm.value.next !== changePasswordForm.value.confirm) {
    changePasswordError.value = '两次输入的新主密码不一致';
    return;
  }
  statusText.value = '正在更新主密码...';
  if (!post({
    type: 'changeMasterPassword',
    currentPassword: changePasswordForm.value.current,
    newPassword: changePasswordForm.value.next
  })) {
    showChangePassword.value = false;
    statusText.value = '主密码已更新';
  }
};

// Windows Hello：解锁 / 启用 / 关闭
const helloUnlock = () => {
  if (!helloEnabled.value || helloBusy.value) return;
  unlockError.value = '';
  helloBusy.value = true;
  statusText.value = '正在等待 Windows 安全中心验证...';
  if (!post({ type: 'helloUnlock' })) {
    helloBusy.value = false;
    unlockError.value = '当前环境不支持 Windows Hello';
  }
};

const enableHello = () => {
  if (helloBusy.value) return;
  helloBusy.value = true;
  statusText.value = '正在创建 Windows Hello 凭据...';
  if (!post({ type: 'helloEnable' })) {
    helloBusy.value = false;
    statusText.value = '当前环境不支持 Windows Hello';
  }
};

const disableHello = () => {
  if (helloBusy.value) return;
  helloBusy.value = true;
  statusText.value = '正在关闭 Windows Hello 解锁...';
  if (!post({ type: 'helloDisable' })) {
    helloBusy.value = false;
    helloEnabled.value = false;
  }
};

const handleVaultError = (data) => {
  const message = ERROR_MESSAGES[data.code] || ('操作失败: ' + (data.code || 'unknown'));
  if (showChangePassword.value) {
    changePasswordError.value = message;
  } else {
    unlockError.value = message;
  }
  statusText.value = message;
};

const handleHostMessage = (event) => {
  const data = event.data || {};
  switch (data.type) {
    case 'vaultStatus':
      vaultState.value = data.initialized ? 'locked' : 'setup';
      helloEnabled.value = !!data.helloEnabled;
      helloBusy.value = !!data.helloBusy;
      break;
    case 'vaultUnlocked': {
      isLoadingEntries = true;
      const loadedEntries = stripRemovedFields(data.entries);
      entries.value = loadedEntries;
      categories.value = Array.isArray(data.categories) ? [...data.categories] : [];
      isLoadingEntries = false;
      lastSavedSnapshot = JSON.stringify({ entries: loadedEntries, categories: categories.value });
      vaultState.value = 'unlocked';
      helloBusy.value = false;
      refreshTotp();
      masterPassword.value = '';
      confirmPassword.value = '';
      unlockError.value = '';
      if (data.importedCount) {
        statusText.value = `保险库已创建，并导入 ${data.importedCount} 条旧账号`;
      } else {
        statusText.value = `已解锁，共 ${entries.value.length} 条记录`;
      }
      break;
    }
    case 'vaultLocked':
      entries.value = [];
      categories.value = [];
      visiblePasswords.value = {};
      totpCodes.value = {};
      showRecovery.value = false;
      recoveryEntry.value = null;
      editingCategory.value = null;
      categoryToDelete.value = null;
      showNewCategory.value = false;
      selectedIds.value = new Set();
      lastSavedSnapshot = '';
      vaultState.value = 'locked';
      masterPassword.value = '';
      unlockError.value = '';
      statusText.value = '保险库已锁定';
      break;
    case 'vaultSaved':
      statusText.value = '已加密保存';
      break;
    case 'masterPasswordChanged':
      showChangePassword.value = false;
      changePasswordForm.value = { current: '', next: '', confirm: '' };
      changePasswordError.value = '';
      helloBusy.value = false;
      if (data.helloRewrapFailed) {
        helloEnabled.value = false;
        statusText.value = '主密码已更新，但 Windows Hello 重新绑定失败（已关闭该功能）';
      } else {
        statusText.value = '主密码已更新';
      }
      break;
    case 'helloEnabled':
      helloEnabled.value = true;
      helloBusy.value = false;
      statusText.value = '已启用 Windows Hello 解锁';
      break;
    case 'helloDisabled':
      helloEnabled.value = false;
      helloBusy.value = false;
      statusText.value = '已关闭 Windows Hello 解锁';
      break;
    case 'steamLoginResult':
      steamLoggingIn.value = false;
      if (data.ok) {
        statusText.value = data.registryOk === false
          ? `已启动 Steam（${data.username}），但注册表自动登录写入失败`
          : `已关闭旧 Steam，正在以 ${data.username} 登录`;
      } else {
        statusText.value = STEAM_ERRORS[data.code] || ('Steam 登录失败: ' + (data.detail || data.code || '未知错误'));
      }
      break;
    case 'steamAccounts':
      applyImportedSteamAccounts(data);
      break;
    case 'steamEnrollStep': {
      enrollBusy.value = false;
      enrollError.value = '';
      enrollHint.value = data.message || '';
      const step = data.step;
      if (step === 'guard') {
        enrollCodeType.value = data.codeType || 3;
        enrollStep.value = 'guard';
      } else if (step === 'confirm') {
        enrollCodeType.value = data.codeType || 4;
        enrollStep.value = 'confirm';
      } else if (step === 'activation') {
        enrollCodeType.value = data.codeType || 3;
        enrollStep.value = 'activation';
      }
      break;
    }
    case 'steamEnrollDone':
      enrollBusy.value = false;
      enrollResult.value = data;
      enrollStep.value = 'done';
      applyEnrollResult(data);
      break;
    case 'steamEnrollError': {
      enrollBusy.value = false;
      enrollCode.value = '';
      const base = ENROLL_ERRORS[data.code] || '操作失败';
      enrollError.value = data.detail ? `${base}｜${data.detail}` : base;
      break;
    }
    case 'steamEnrollCancelled':
      enrollBusy.value = false;
      break;
    case 'clipboardCopied':
      break;
    case 'vaultError':
      helloBusy.value = false;
      if (enrollBusy.value) {
        // 令牌登记过程中后端线程异常/卡住：不能让向导静默停在「登录中…」
        enrollBusy.value = false;
        enrollError.value = `后端异常中断（${data.code || 'unknown'}）：${data.detail || ''}`;
      }
      if (data.helloDisabled) {
        // 后端已清理失效的 Hello 数据，锁屏界面同步隐藏 Hello 按钮
        helloEnabled.value = false;
      }
      handleVaultError(data);
      break;
    case 'setConfig':
      applyConfig(data.config || {});
      break;
    default:
      break;
  }
};

// ============================================================ 配置
const applyConfig = (config) => {
  if (config.sidebarCollapsed !== undefined) {
    isSidebarCollapsed.value = config.sidebarCollapsed;
  }
  if (config.theme !== undefined) {
    isDarkTheme.value = config.theme === 'dark';
    localStorage.setItem('theme', config.theme);
  }
};

const saveConfig = () => {
  post({
    type: 'saveConfig',
    config: {
      sidebarCollapsed: isSidebarCollapsed.value,
      theme: isDarkTheme.value ? 'dark' : 'light'
    }
  });
};

const toggleSidebar = () => {
  isSidebarCollapsed.value = !isSidebarCollapsed.value;
  saveConfig();
};

// ============================================================ 条目操作
const newId = () => 'id_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 9);

const defaultCategory = () => (currentFilter.value.startsWith('cat:') ? currentFilter.value.slice(4) : '');

// 新增条目时按当前所在的专栏给一个合适的默认类型
const defaultEntryType = () => {
  if (currentFilter.value === 'steam') return 'steam';
  if (currentFilter.value === 'software') return 'software';
  if (currentFilter.value === 'recovery') return 'recovery';
  return 'website';
};

// 补齐字段，保证编辑面板的 v-model 都有初始值
const normalizeEntry = (entry) => ({
  id: entry.id || newId(),
  name: entry.name || '',
  type: entry.type || 'website',
  url: entry.url || '',
  path: entry.path || '',
  username: entry.username || '',
  password: entry.password || '',
  steamId: entry.steamId || '',
  totpSecret: entry.totpSecret || '',
  totpAlgorithm: entry.totpAlgorithm === 'steam' ? 'steam' : 'totp',
  revocationCode: entry.revocationCode || '',
  recoveryCodes: entry.recoveryCodes || '',
  usedRecoveryCodes: Array.isArray(entry.usedRecoveryCodes) ? [...entry.usedRecoveryCodes] : [],
  category: entry.category || '',
  notes: entry.notes || ''
});

// Steam 状态字段已移除：载入旧保险库时顺手清理，下次保存即从 vault.json 中消失
const stripRemovedFields = (list) => (Array.isArray(list) ? list : []).map(item => {
  if (!item || typeof item !== 'object' || !('status' in item)) return item;
  const copy = { ...item };
  delete copy.status;
  return copy;
});

const createEntry = () => {
  editingEntry.value = normalizeEntry({ type: defaultEntryType(), category: defaultCategory() });
  isNewEntry.value = true;
  showGenerator.value = false;
  qrError.value = '';
  showEditPanel.value = true;
};

const handleEditSelected = () => {
  if (selectedIds.value.size === 0) {
    statusText.value = '请先选择一条记录';
    return;
  }
  const firstId = Array.from(selectedIds.value)[0];
  const target = entries.value.find(item => item.id === firstId);
  if (!target) return;
  editingEntry.value = normalizeEntry({ ...target });
  isNewEntry.value = false;
  showGenerator.value = false;
  qrError.value = '';
  showEditPanel.value = true;
};

const saveEdit = () => {
  if (!editingEntry.value) return;
  const draft = { ...editingEntry.value, updatedAt: new Date().toISOString() };
  if (!draft.name) {
    draft.name = draft.username || draft.url || draft.path || '未命名';
  }
  // 手动输入的新分类也登记到分类列表，保证侧边栏能看到、下次仍可选
  const draftCategory = (draft.category || '').trim();
  draft.category = draftCategory;
  if (draftCategory && !categories.value.includes(draftCategory)) {
    categories.value = [...categories.value, draftCategory];
  }
  if (isNewEntry.value) {
    entries.value = [...entries.value, draft];
    selectedIds.value = new Set([draft.id]);
    statusText.value = '已新增条目';
  } else {
    const index = entries.value.findIndex(item => item.id === draft.id);
    if (index !== -1) {
      entries.value[index] = draft;
      statusText.value = '已保存修改';
    }
  }
  showEditPanel.value = false;
  showRecovery.value = false;
  saveEntries();
};

const deleteEntry = (entry) => {
  deleteTargets.value = [entry];
};

const handleBulkDelete = () => {
  if (selectedIds.value.size === 0) {
    statusText.value = '请先选择要删除的记录';
    return;
  }
  deleteTargets.value = entries.value.filter(item => selectedIds.value.has(item.id));
};

const confirmDelete = () => {
  if (!deleteTargets.value || deleteTargets.value.length === 0) return;
  const removedIds = deleteTargets.value.map(item => item.id);
  entries.value = entries.value.filter(item => !removedIds.includes(item.id));
  const nextSelection = new Set(selectedIds.value);
  removedIds.forEach(id => nextSelection.delete(id));
  selectedIds.value = nextSelection;
  deleteTargets.value = null;
  if (recoveryEntry.value && removedIds.includes(recoveryEntry.value.id)) {
    showRecovery.value = false;
    recoveryEntry.value = null;
  }
  statusText.value = `已删除 ${removedIds.length} 条记录`;
  saveEntries();
};

// ============================================================ 复制 / 打开
const copyText = (text, label) => {
  if (!text) {
    statusText.value = '没有可复制的内容';
    return;
  }
  if (!post({ type: 'copyText', text: text })) {
    if (navigator.clipboard) navigator.clipboard.writeText(text);
  }
  statusText.value = label || '已复制到剪贴板';
};

const openEntry = (entry) => {
  if (!entry) return;
  if (entry.type === 'software' && entry.path) {
    post({ type: 'openPath', path: entry.path });
    statusText.value = '正在打开: ' + entry.path;
    return;
  }
  if (entry.url) {
    const url = /^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(entry.url) ? entry.url : 'https://' + entry.url;
    post({ type: 'openUrl', url: url });
    statusText.value = '正在打开: ' + url;
    return;
  }
  if (entry.path) {
    post({ type: 'openPath', path: entry.path });
    statusText.value = '正在打开: ' + entry.path;
    return;
  }
  statusText.value = '该条目没有填写网址或程序路径';
};

const togglePassword = (id) => {
  visiblePasswords.value[id] = !visiblePasswords.value[id];
};

// 固定长度掩码：不泄漏真实密码长度
const MASKED_PASSWORD = '••••••••';
const maskText = () => MASKED_PASSWORD;

// ============================================================ 密码生成器
const CHARSETS = {
  upper: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
  lower: 'abcdefghijklmnopqrstuvwxyz',
  digits: '0123456789',
  symbols: '!@#$%^&*()-_=+[]{};:,.?'
};

const randomInt = (max) => {
  const buffer = new Uint32Array(1);
  const limit = Math.floor(0x100000000 / max) * max;
  let value = 0;
  do {
    crypto.getRandomValues(buffer);
    value = buffer[0];
  } while (value >= limit);
  return value % max;
};

const buildPassword = () => {
  let pool = '';
  if (generator.value.upper) pool += CHARSETS.upper;
  if (generator.value.lower) pool += CHARSETS.lower;
  if (generator.value.digits) pool += CHARSETS.digits;
  if (generator.value.symbols) pool += CHARSETS.symbols;
  if (!pool) return '';
  let result = '';
  for (let i = 0; i < generator.value.length; i++) {
    result += pool[randomInt(pool.length)];
  }
  return result;
};

const generatePassword = () => {
  generator.value.preview = buildPassword();
};

const toggleGenerator = () => {
  showGenerator.value = !showGenerator.value;
  if (showGenerator.value) generatePassword();
};

const openExternal = (url) => {
  if (!post({ type: 'openUrl', url: url })) {
    window.open(url, '_blank');
  }
};

const applyGenerated = () => {
  if (!generator.value.preview) generatePassword();
  if (editingEntry.value) editingEntry.value.password = generator.value.preview;
  statusText.value = '已填入生成的密码';
};

// ============================================================ TOTP（2FA 动态验证码）
// ==== TOTP-HELPERS-BEGIN ==== （此段为纯函数，可独立提取做单元测试）
const BASE32_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
const MIN_BARE_SECRET_LENGTH = 16;   // 裸 Base32 密钥的最短长度（16 字符 = 80 位）

// Base32 解码（忽略空格、短横线、填充符，支持小写）
const base32Decode = (input) => {
  const text = (input || '').replace(/[\s-]/g, '').replace(/=+$/, '').toUpperCase();
  const bytes = [];
  let buffer = 0;
  let bits = 0;
  for (const ch of text) {
    const value = BASE32_ALPHABET.indexOf(ch);
    if (value < 0) throw new Error('invalid base32');
    buffer = (buffer << 5) | value;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      bytes.push((buffer >>> bits) & 0xFF);
    }
  }
  return new Uint8Array(bytes);
};

// 解析 TOTP 输入：otpauth:// 链接或裸 Base32 密钥
const parseTotpInput = (raw) => {
  const text = (raw || '').trim();
  if (!text) return null;

  const config = { secret: '', digits: 6, period: 30, algorithm: 'SHA-1' };
  const isOtpAuthUri = /^otpauth:\/\//i.test(text);

  if (isOtpAuthUri) {
    let url;
    try {
      url = new URL(text);
    } catch (e) {
      return null;
    }
    config.secret = url.searchParams.get('secret') || '';
    const digits = parseInt(url.searchParams.get('digits') || '6', 10);
    if (digits >= 6 && digits <= 10) config.digits = digits;
    const period = parseInt(url.searchParams.get('period') || '30', 10);
    if (period > 0) config.period = period;
    const algorithm = (url.searchParams.get('algorithm') || 'SHA1').toUpperCase().replace(/-/g, '');
    if (algorithm === 'SHA256') config.algorithm = 'SHA-256';
    else if (algorithm === 'SHA512') config.algorithm = 'SHA-512';
  } else {
    config.secret = text;
  }

  config.secret = config.secret.replace(/[\s-]/g, '').replace(/=+$/, '').toUpperCase();
  if (!config.secret) return null;

  // 裸密钥（非 otpauth 链接）要求足够长：否则像 "HELLOWORLD" 这种普通文本
  // 恰好全部由 Base32 字符组成，会被误判成 2FA 密钥
  if (!isOtpAuthUri && config.secret.length < MIN_BARE_SECRET_LENGTH) return null;

  try {
    if (base32Decode(config.secret).length === 0) return null;
  } catch (e) {
    return null;
  }
  return config;
};

// 计算某个时间点的 TOTP 码（HMAC-based，RFC 6238）
const totpCodeAt = async (config, unixSeconds) => {
  const counter = Math.floor(unixSeconds / config.period);
  const message = new ArrayBuffer(8);
  const view = new DataView(message);
  view.setUint32(0, Math.floor(counter / 0x100000000));
  view.setUint32(4, counter >>> 0);

  const key = await crypto.subtle.importKey(
    'raw',
    base32Decode(config.secret),
    { name: 'HMAC', hash: config.algorithm },
    false,
    ['sign']
  );
  const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, message));

  const offset = mac[mac.length - 1] & 0x0F;
  const binary = ((mac[offset] & 0x7F) << 24)
    | ((mac[offset + 1] & 0xFF) << 16)
    | ((mac[offset + 2] & 0xFF) << 8)
    | (mac[offset + 3] & 0xFF);
  const code = (binary % Math.pow(10, config.digits)).toString().padStart(config.digits, '0');
  return code;
};
// ---------- Steam Guard（非标准 TOTP：Base64 密钥 + 26 进制 5 位码）----------
// 算法与 SteamKit 的 GenerateSteamGuardCode / go-steam-totp 一致：
// HMAC-SHA1(shared_secret, BE64(floor(t/30))) -> 动态截断 -> 26 进制取 5 位
const STEAM_ALPHABET = '23456789BCDFGHJKMNPQRTVWXY';
const STEAM_PERIOD = 30;

// Base64 → 字节（shared_secret 是标准 Base64）
const base64ToBytes = (text) => {
  const normalized = (text || '').replace(/\s/g, '');
  if (!normalized) throw new Error('empty base64');
  const binary = atob(normalized);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
};

// 统一解析 Steam Guard 密钥来源：.maFile JSON / otpauth:// 链接 / 裸 Base64 shared_secret
const parseSteamGuardInput = (raw) => {
  const text = (raw || '').trim();
  if (!text) return null;

  // 1) .maFile（Steam Desktop Authenticator 导出的 JSON）
  if (text.startsWith('{')) {
    try {
      const data = JSON.parse(text);
      const shared = (data.shared_secret || '').trim();
      if (!shared) return null;
      base64ToBytes(shared);   // 校验合法性
      return {
        kind: 'mafile',
        secret: shared,
        identitySecret: (data.identity_secret || '').trim(),
        revocationCode: (data.revocation_code || '').trim(),
        accountName: (data.account_name || '').trim()
      };
    } catch (e) {
      return null;
    }
  }

  // 2) otpauth://totp/Steam:xxx?secret=<BASE32> —— 转成同一串字节的 Base64
  if (/^otpauth:\/\//i.test(text)) {
    const config = parseTotpInput(text);
    if (!config) return null;
    const bytes = base32Decode(config.secret);
    let binary = '';
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return {
      kind: 'otpauth',
      secret: btoa(binary),
      identitySecret: '',
      revocationCode: '',
      accountName: config.label || ''
    };
  }

  // 3) 裸 Base64 shared_secret
  try {
    const bytes = base64ToBytes(text);
    if (bytes.length < 5) return null;
    return {
      kind: 'secret',
      secret: text.replace(/\s/g, ''),
      identitySecret: '',
      revocationCode: '',
      accountName: ''
    };
  } catch (e) {
    return null;
  }
};

// 计算某个时间点的 Steam Guard 码（5 位）
const steamGuardCodeAt = async (sharedSecret, unixSeconds) => {
  const key = base64ToBytes(sharedSecret);
  const counter = Math.floor(unixSeconds / STEAM_PERIOD);
  const message = new ArrayBuffer(8);
  const view = new DataView(message);
  view.setUint32(0, Math.floor(counter / 0x100000000));
  view.setUint32(4, counter >>> 0);

  const cryptoKey = await crypto.subtle.importKey('raw', key, { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
  const mac = new Uint8Array(await crypto.subtle.sign('HMAC', cryptoKey, message));

  const offset = mac[19] & 0x0F;
  let value = ((mac[offset] & 0x7F) << 24)
    | ((mac[offset + 1] & 0xFF) << 16)
    | ((mac[offset + 2] & 0xFF) << 8)
    | (mac[offset + 3] & 0xFF);

  let code = '';
  for (let i = 0; i < 5; i++) {
    code += STEAM_ALPHABET[value % 26];
    value = Math.floor(value / 26);
  }
  return code;
};
// ==== TOTP-HELPERS-END ====

const totpError = (entry) => {
  if (!hasTotp(entry)) return '';
  if (entry.totpAlgorithm === 'steam') {
    return parseSteamGuardInput(entry.totpSecret) ? '' : '密钥格式不正确（应为 .maFile / Base64 shared_secret / otpauth:// 链接）';
  }
  return parseTotpInput(entry.totpSecret) ? '' : '密钥格式不正确（应为 Base32 或 otpauth:// 链接）';
};

// 每秒刷新所有条目的动态码
const refreshTotp = async () => {
  if (vaultState.value !== 'unlocked') return;
  const now = Math.floor(Date.now() / 1000);
  const next = {};
  for (const entry of entries.value) {
    if (!hasTotp(entry)) continue;

    if (entry.totpAlgorithm === 'steam') {
      const steam = parseSteamGuardInput(entry.totpSecret);
      if (!steam) {
        next[entry.id] = { error: '密钥格式不正确' };
        continue;
      }
      try {
        const code = await steamGuardCodeAt(steam.secret, now);
        next[entry.id] = { code: code, remaining: STEAM_PERIOD - (now % STEAM_PERIOD), period: STEAM_PERIOD };
      } catch (e) {
        next[entry.id] = { error: '无法计算' };
      }
      continue;
    }

    const config = parseTotpInput(entry.totpSecret);
    if (!config) {
      next[entry.id] = { error: '密钥格式不正确' };
      continue;
    }
    try {
      const code = await totpCodeAt(config, now);
      next[entry.id] = {
        code: code,
        remaining: config.period - (now % config.period),
        period: config.period
      };
    } catch (e) {
      next[entry.id] = { error: '无法计算' };
    }
  }
  totpCodes.value = next;
};

const totpText = (entry) => {
  const state = totpCodes.value[entry.id];
  if (!state) return hasTotp(entry) ? '······' : '—';
  if (state.error) return state.error;
  return state.code;
};

const copyTotp = (entry) => {
  const state = totpCodes.value[entry.id];
  if (!state || !state.code) {
    statusText.value = hasTotp(entry) ? '动态码尚未生成' : '该条目没有设置 2FA 密钥';
    return;
  }
  copyText(state.code, '2FA 动态码已复制');
};

// ============================================================ 从二维码导入 otpauth://
// ==== QR-HELPERS-BEGIN ==== （纯函数段，可独立提取做单元测试）
// 从 ImageData 解出二维码文本
const decodeQrImageData = (imageData) => {
  if (!imageData || !imageData.data || !imageData.width || !imageData.height) return '';
  try {
    const result = jsQR(imageData.data, imageData.width, imageData.height);
    return result && result.data ? result.data : '';
  } catch (e) {
    return '';
  }
};

// 解析二维码文本：otpauth:// 链接 / 裸 Base32 密钥 / 其他
const parseQrText = (text) => {
  const raw = (text || '').trim();
  if (!raw) return { kind: 'empty', message: '二维码内容为空' };

  if (/^otpauth:\/\//i.test(raw)) {
    const config = parseTotpInput(raw);
    if (!config) return { kind: 'invalid', message: '二维码里的 otpauth 链接缺少有效密钥' };

    let label = '';
    let issuer = '';
    try {
      const url = new URL(raw);
      issuer = url.searchParams.get('issuer') || '';
      label = decodeURIComponent(url.pathname.replace(/^\/+/, ''));
    } catch (e) {
      // 解析失败不影响使用原始链接
    }
    return { kind: 'otpauth', value: raw, label: label, issuer: issuer, config: config };
  }

  // 有些站点把纯密钥编码进二维码
  const config = parseTotpInput(raw);
  if (config) {
    return { kind: 'secret', value: config.secret, label: '', issuer: '', config: config };
  }

  return { kind: 'unknown', message: '二维码内容既不是 otpauth:// 链接，也不是 Base32 密钥' };
};
// ==== QR-HELPERS-END ====

// 把任意来源（二维码文本 / .maFile 内容 / 手输密钥）写入当前条目的 2FA 字段
const applyTotpSource = (text, sourceLabel) => {
  if (!editingEntry.value) return false;
  const raw = (text || '').trim();
  if (!raw) {
    qrError.value = '内容为空';
    return false;
  }

  const label = sourceLabel || '二维码';
  const isSteamEntry = entryType(editingEntry.value) === 'steam';
  const looksSteamUri = /^otpauth:\/\/totp\/steam/i.test(raw) || /issuer=steam/i.test(raw);
  const looksMaFile = raw.startsWith('{');

  // Steam Guard 优先：.maFile、Steam 的 otpauth 链接，或 Steam 类型条目下的 Base64 密钥
  if (looksMaFile || looksSteamUri || isSteamEntry) {
    const steam = parseSteamGuardInput(raw);
    if (steam) {
      editingEntry.value.totpSecret = steam.secret;
      editingEntry.value.totpAlgorithm = 'steam';
      if (steam.revocationCode) editingEntry.value.revocationCode = steam.revocationCode;
      if (steam.accountName) {
        if (!editingEntry.value.username) editingEntry.value.username = steam.accountName;
        if (!editingEntry.value.name) editingEntry.value.name = steam.accountName;
      }
      qrError.value = '';
      statusText.value = `已从${label}导入 Steam Guard 密钥（5 位动态码）`;
      return true;
    }
  }

  const parsed = parseQrText(raw);
  if (parsed.kind !== 'otpauth' && parsed.kind !== 'secret') {
    qrError.value = parsed.message;
    statusText.value = parsed.message;
    return false;
  }

  editingEntry.value.totpSecret = parsed.value;
  editingEntry.value.totpAlgorithm = 'totp';
  const suggested = (parsed.issuer || parsed.label || '').split(':')[0].trim();
  if (!editingEntry.value.name && suggested) {
    editingEntry.value.name = suggested;
  }

  qrError.value = '';
  if (parsed.kind === 'otpauth') {
    statusText.value = `已从${label}导入 2FA 链接` + (parsed.issuer ? `（${parsed.issuer}）` : '');
  } else {
    statusText.value = `已从${label}导入 2FA 密钥`;
  }
  return true;
};

const applyQrText = (text) => applyTotpSource(text, '二维码');

// 导入 2FA 来源文件：图片走二维码识别，.maFile / .json / 文本走密钥解析
const importSecretFile = async (file) => {
  if (!file) return;
  if (file.type && file.type.startsWith('image/')) {
    decodeQrFromFile(file);
    return;
  }

  const name = (file.name || '').toLowerCase();
  const isText = name.endsWith('.mafile') || name.endsWith('.json') || name.endsWith('.txt')
    || file.type === 'application/json' || (file.type || '').startsWith('text/');
  if (!isText) {
    qrError.value = '请选择二维码图片，或 .maFile / .json / .txt 密钥文件';
    return;
  }

  try {
    const text = await file.text();
    applyTotpSource(text, '文件');
  } catch (e) {
    qrError.value = '文件读取失败：' + (e && e.message ? e.message : '未知错误');
    statusText.value = qrError.value;
  }
};

// 在 2FA 输入框里直接粘贴密钥后（失焦时）自动识别类型
const normalizeTotpField = () => {
  if (!editingEntry.value) return;
  const raw = (editingEntry.value.totpSecret || '').trim();
  if (!raw) return;

  const isSteam = editingEntry.value.totpAlgorithm === 'steam';
  if (isSteam && parseSteamGuardInput(raw)) return;
  if (!isSteam && parseTotpInput(raw)) return;

  const steam = parseSteamGuardInput(raw);
  const looksSteam = raw.startsWith('{') || /^otpauth:\/\/totp\/steam/i.test(raw) || /issuer=steam/i.test(raw);
  if (steam && (looksSteam || entryType(editingEntry.value) === 'steam')) {
    applyTotpSource(raw, '输入');
  }
};

const decodeQrFromFile = async (file) => {
  if (!file) return;
  if (file.type && !file.type.startsWith('image/')) {
    qrError.value = '请选择图片文件';
    return;
  }

  qrDecoding.value = true;
  qrError.value = '';
  try {
    const bitmap = await createImageBitmap(file);
    const canvas = document.createElement('canvas');
    canvas.width = bitmap.width;
    canvas.height = bitmap.height;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    context.drawImage(bitmap, 0, 0);
    const imageData = context.getImageData(0, 0, canvas.width, canvas.height);
    bitmap.close();

    const text = decodeQrImageData(imageData);
    if (!text) {
      qrError.value = '没能在图片里识别出二维码，换一张更清晰、完整的截图试试';
      statusText.value = qrError.value;
      return;
    }
    applyQrText(text);
  } catch (e) {
    qrError.value = '图片读取失败：' + (e && e.message ? e.message : '未知错误');
    statusText.value = qrError.value;
  } finally {
    qrDecoding.value = false;
  }
};

const pickQrImage = () => {
  if (qrInput.value) qrInput.value.click();
};

const handleQrFileChange = (event) => {
  const files = event.target.files;
  if (files && files.length) importSecretFile(files[0]);
  event.target.value = '';   // 允许重复选择同一张图片
};

const handleQrDrop = (event) => {
  const files = event.dataTransfer && event.dataTransfer.files;
  if (files && files.length) importSecretFile(files[0]);
};

const handleQrPaste = (event) => {
  if (!showEditPanel.value) return;
  const items = event.clipboardData && event.clipboardData.items;
  if (!items) return;
  // DataTransferItemList 只是类数组，不能用 for...of
  for (let i = 0; i < items.length; i++) {
    const item = items[i];
    if (item && item.type && item.type.startsWith('image/')) {
      const file = item.getAsFile();
      if (file) {
        event.preventDefault();
        decodeQrFromFile(file);
        return;
      }
    }
  }
};
// ============================================================ 2FA 恢复代码
const splitRecoveryCodes = (text) =>
  (text || '').split('\n').map(line => line.trim()).filter(line => line.length > 0);

const isCodeUsed = (entry, code) =>
  Array.isArray(entry.usedRecoveryCodes) && entry.usedRecoveryCodes.includes(code);

const recoveryRemaining = (entry) =>
  splitRecoveryCodes(entry ? entry.recoveryCodes : '').filter(code => !isCodeUsed(entry, code)).length;

const openRecovery = (entry) => {
  recoveryEntry.value = entry;
  showRecovery.value = true;
};

const copyAllRecovery = () => {
  const entry = recoveryEntry.value || editingEntry.value;
  if (!entry) return;
  const codes = splitRecoveryCodes(entry.recoveryCodes).filter(code => !isCodeUsed(entry, code));
  copyText(codes.join('\n'), `已复制 ${codes.length} 条未使用的恢复代码`);
};

const toggleCodeUsed = (entry, code) => {
  if (!entry) return;
  const used = Array.isArray(entry.usedRecoveryCodes) ? [...entry.usedRecoveryCodes] : [];
  const index = used.indexOf(code);
  if (index >= 0) used.splice(index, 1);
  else used.push(code);
  entry.usedRecoveryCodes = used;
  scheduleSave();
};

// ============================================================ Steam 一键登录
const STEAM_ERRORS = {
  steam_not_found: '注册表里找不到 Steam 安装路径，请先安装并运行一次 Steam',
  steam_start_failed: '启动 Steam 失败',
  steam_no_username: '该账号没有填写 Steam 账号名',
  steam_busy: '上一次 Steam 登录还在进行中，请稍候',
  worker_exception: '登录过程出现异常'
};

const openSteamProfile = (entry) => {
  if (!entry || !entry.steamId) {
    statusText.value = '该账号没有填写 SteamID64';
    return;
  }
  const steamId = String(entry.steamId).trim();
  openExternal('https://steamcommunity.com/profiles/' + steamId);
};

// 只选中一个条目时显示底部悬浮操作栏
const floatingEntry = computed(() => {
  if (selectedIds.value.size !== 1) return null;
  const onlyId = Array.from(selectedIds.value)[0];
  return entries.value.find(item => item.id === onlyId) || null;
});

const requestSteamLogin = (entry) => {
  if (!entry) return;
  if (!(entry.username || '').trim()) {
    statusText.value = '该账号没有填写 Steam 账号名';
    return;
  }
  if (steamLoggingIn.value) {
    statusText.value = '上一次 Steam 登录还在进行中，请稍候';
    return;
  }
  steamLoginTarget.value = entry;
};

// 从本机 Steam 的 loginusers.vdf 导入账号（只带账号名/昵称/SteamID64，密码需手填）
const importSteamAccounts = () => {
  statusText.value = '正在读取本机 Steam 账号…';
  if (!post({ type: 'steamImportAccounts' })) {
    statusText.value = '当前环境不支持读取本机 Steam';
  }
};

const applyImportedSteamAccounts = (data) => {
  const list = Array.isArray(data.accounts) ? data.accounts : [];
  if (!list.length) {
    statusText.value = data.error || '本机 Steam 里没有已登录的账号';
    return;
  }

  const existing = new Set(
    entries.value.map(item => `${item.steamId || ''}|${(item.username || '').toLowerCase()}`)
  );
  const additions = [];
  for (const item of list) {
    const key = `${item.steamId || ''}|${(item.accountName || '').toLowerCase()}`;
    if (existing.has(key)) continue;
    existing.add(key);
    additions.push(normalizeEntry({
      name: item.personaName || item.accountName || item.steamId,
      type: 'steam',
      username: item.accountName || '',
      steamId: item.steamId || '',
      category: 'Steam',
      notes: '由本机 Steam 导入（密码需自行填写）'
    }));
  }

  if (!additions.length) {
    statusText.value = '本机 Steam 的账号都已在列表中';
    return;
  }

  entries.value = [...entries.value, ...additions];
  if (!categories.value.includes('Steam')) {
    categories.value = [...categories.value, 'Steam'];
  }
  selectedIds.value = new Set(additions.map(item => item.id));
  statusText.value = `已从本机 Steam 导入 ${additions.length} 个账号，请补填密码`;
  saveEntries();
};

// ==================== 应用内添加 Steam 令牌（向导） ====================
const showEnrollModal = ref(false);
const enrollStep = ref('warning');      // warning | credentials | guard | confirm | activation | done
const enrollBusy = ref(false);
const enrollError = ref('');
const enrollTarget = ref(null);
const enrollMode = ref('add');        // add = 添加令牌，remove = 移除令牌
const enrollRevocation = ref('');
const enrollExitConfirm = ref(false);
const enrollPrevStep = ref('credentials');
const enrollUsername = ref('');
const enrollPassword = ref('');
const enrollCode = ref('');
const enrollCodeType = ref(3);          // 2=邮箱验证码 3=短信/令牌动态码 4=手机确认 5=邮件确认
const enrollHint = ref('');
const enrollResult = ref(null);

const ENROLL_ERRORS = {
  missing_credentials: '请填写账号与密码',
  busy: '已有令牌操作正在进行，请稍候',
  missing_code: '请输入验证码',
  rsa_key: '无法获取 Steam 登录公钥（网络不通或账号不存在）',
  rsa_encrypt: '密码加密失败',
  begin_session: '登录失败：账号名或密码不正确',
  guard_code: '验证码不正确或已过期',
  poll: '等待 Steam 响应超时，请重试',
  add_authenticator: '添加令牌失败：账号可能已存在令牌（需先在手机上移除），或 Steam 拒绝了本次请求',
  finalize: '激活失败：激活码不正确或已过期'
};

const enrollCodeLabel = computed(() => {
  if (enrollCodeType.value === 2) return '邮箱验证码';
  if (enrollCodeType.value === 5) return '邮件确认链接';
  return '短信验证码';
});

// 等待后端响应的看门狗：避免「登录中…」永久卡住（含后端线程异常/死锁的情况）
let enrollWaitId = 0;
const beginEnrollWait = () => {
  enrollBusy.value = true;
  enrollError.value = '';
  const waitId = ++enrollWaitId;
  setTimeout(() => {
    if (waitId !== enrollWaitId || !enrollBusy.value) return;   // 已收到响应，忽略
    enrollBusy.value = false;
    enrollError.value = '等待 Steam 响应超时（超过 120 秒）。请重试；若反复如此，把程序目录下的 steam_enroll.log 发给开发者。';
  }, 120000);
};
// 弹窗遮罩关闭：只有在遮罩上「按下」并且「松开」才关闭。
// 否则在弹窗内按住鼠标拖到外面松开时，click 的目标会变成遮罩，导致误关。
const overlayPressed = ref(false);
const closeOverlay = (action) => {
  if (overlayPressed.value) action();
  overlayPressed.value = false;
};
// Base64（Steam 的 shared_secret 形式）-> Base32（otpauth 链接要求的形式）
const base64ToBase32 = (base64) => {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  const binary = atob((base64 || '').replace(/\s/g, ''));
  let bits = 0, value = 0, out = '';
  for (let i = 0; i < binary.length; i++) {
    value = (value << 8) | binary.charCodeAt(i);
    bits += 8;
    while (bits >= 5) { out += alphabet[(value >>> (bits - 5)) & 31]; bits -= 5; }
  }
  if (bits > 0) out += alphabet[(value << (5 - bits)) & 31];
  return out;
};

// 生成该账号的 otpauth 链接（手机端与第三方验证器都能读）
const buildOtpAuthUri = (entry) => {
  const secret = base64ToBase32(entry.totpSecret || '');
  const label = encodeURIComponent(entry.username || entry.name || 'steam');
  return `otpauth://totp/Steam:${label}?secret=${secret}&issuer=Steam`;
};

const showQrTarget = ref(null);
const qrExportUrl = ref('');
const qrExportError = ref('');
const openQrCode = async (entry) => {
  if (!entry) return;
  if (!entry.totpSecret) { statusText.value = '该条目还没有令牌密钥，无法生成二维码'; return; }
  showQrTarget.value = entry;
  qrExportError.value = '';
  qrExportUrl.value = '';
  try {
    qrExportUrl.value = await QRCode.toDataURL(buildOtpAuthUri(entry), { width: 280, margin: 1 });
  } catch (e) {
    qrExportError.value = '二维码生成失败：' + (e && e.message ? e.message : '未知错误');
  }
};

const openEnrollModal = (entry, mode) => {
  if (!entry) return;
  enrollTarget.value = entry;
  enrollMode.value = mode || 'add';
  enrollRevocation.value = entry.revocationCode || '';
  enrollUsername.value = entry.username || '';
  enrollPassword.value = entry.password || '';
  enrollCode.value = '';
  enrollHint.value = '';
  enrollError.value = '';
  enrollResult.value = null;
  enrollStep.value = 'warning';
  showEnrollModal.value = true;
};

const requestCancelEnroll = () => {
  enrollPrevStep.value = enrollStep.value;
  enrollExitConfirm.value = true;
};

const cancelExitEnroll = () => {
  enrollExitConfirm.value = false;
  enrollStep.value = enrollPrevStep.value;
};

const confirmExitEnroll = () => {
  enrollExitConfirm.value = false;
  closeEnrollModal(true);
};

const closeEnrollModal = (force) => {

  if (enrollBusy.value) {
    post({ type: 'steamEnrollCancel' });
  }
  showEnrollModal.value = false;
  enrollBusy.value = false;
  enrollPassword.value = '';   // 不把密码留在内存里
  enrollCode.value = '';
};

const startEnroll = () => {
  if (!enrollUsername.value || !enrollPassword.value) {
    enrollError.value = '请填写账号与密码';
    return;
  }
  beginEnrollWait();
  post({ type: 'steamEnrollStart', username: enrollUsername.value, password: enrollPassword.value, mode: enrollMode.value, revocationCode: enrollRevocation.value });
};

const submitEnrollCode = () => {
  if (!enrollCode.value.trim()) {
    enrollError.value = '请输入验证码';
    return;
  }
  beginEnrollWait();
  if (enrollStep.value === 'guard') {
    post({ type: 'steamEnrollGuardCode', code: enrollCode.value.trim(), codeType: enrollCodeType.value });
  } else {
    post({ type: 'steamEnrollActivation', code: enrollCode.value.trim() });
  }
  enrollCode.value = '';
};

const continueEnrollPoll = () => {
  beginEnrollWait();
  post({ type: 'steamEnrollPoll' });
};

// 令牌登记成功后把密钥写回条目（密钥只进加密保险库）
const applyEnrollResult = (data) => {
  if (data && data.removed) {
    const removedTarget = enrollTarget.value;
    if (removedTarget) {
      removedTarget.totpSecret = '';
      removedTarget.totpAlgorithm = 'totp';
      removedTarget.revocationCode = '';
      saveEntries();
      refreshTotp();
      statusText.value = '已移除该账号的手机令牌';
    }
    return;
  }

  const target = enrollTarget.value;
  if (!target) return;
  target.totpSecret = data.sharedSecret || '';
  target.totpAlgorithm = 'steam';
  if (data.revocationCode) target.revocationCode = data.revocationCode;
  if (data.steamId && !target.steamId) target.steamId = data.steamId;
  if (data.accountName && !target.username) target.username = data.accountName;
  saveEntries();
  refreshTotp();
  statusText.value = '已添加 Steam 令牌并写入条目，请务必保存撤销码';
};

const confirmSteamLogin = () => {
  const entry = steamLoginTarget.value;
  if (!entry) return;
  steamLoginTarget.value = null;
  steamLoggingIn.value = true;
  statusText.value = `正在切换到 ${entry.username}…`;
  const sent = post({
    type: 'steamLogin',
    username: (entry.username || '').trim(),
    password: entry.password || ''
  });
  if (!sent) {
    steamLoggingIn.value = false;
    statusText.value = '当前环境不支持 Steam 登录';
  }
};

// ============================================================ 分类管理（侧边栏直接增删改）
const categoryUsage = (name) =>
  entries.value.filter(entry => (entry.category || '') === name).length;

const openNewCategory = () => {
  showNewCategory.value = true;
  newCategoryName.value = '';
};

const addCategory = () => {
  const name = (newCategoryName.value || '').trim();
  if (!name) {
    statusText.value = '请输入分类名';
    return;
  }
  if (allCategories.value.includes(name)) {
    statusText.value = `分类「${name}」已存在`;
    return;
  }
  categories.value = [...categories.value, name];
  newCategoryName.value = '';
  showNewCategory.value = false;
  statusText.value = `已新建分类「${name}」`;
  saveEntries();
};

const startRenameCategory = (name) => {
  showNewCategory.value = false;
  editingCategory.value = { original: name, value: name };
};

const cancelRenameCategory = () => {
  editingCategory.value = null;
};

const confirmRenameCategory = () => {
  const draft = editingCategory.value;
  if (!draft) return;

  const original = draft.original;
  const next = (draft.value || '').trim();
  if (!next || next === original) {
    editingCategory.value = null;
    return;
  }
  if (allCategories.value.includes(next)) {
    statusText.value = `分类「${next}」已存在`;
    return;
  }

  categories.value = categories.value.map(item => (item === original ? next : item));
  let changed = 0;
  entries.value = entries.value.map(entry => {
    if ((entry.category || '') === original) {
      changed++;
      return { ...entry, category: next };
    }
    return entry;
  });

  if (currentFilter.value === 'cat:' + original) {
    currentFilter.value = 'cat:' + next;
  }
  editingCategory.value = null;
  statusText.value = `分类「${original}」已重命名为「${next}」（同步更新 ${changed} 条记录）`;
  saveEntries();
};

const requestDeleteCategory = (name) => {
  editingCategory.value = null;
  categoryToDelete.value = name;
};

const confirmDeleteCategory = () => {
  const name = categoryToDelete.value;
  if (!name) return;

  categories.value = categories.value.filter(item => item !== name);
  let cleared = 0;
  entries.value = entries.value.map(entry => {
    if ((entry.category || '') === name) {
      cleared++;
      return { ...entry, category: '' };
    }
    return entry;
  });

  if (currentFilter.value === 'cat:' + name) {
    currentFilter.value = 'all';
  }
  categoryToDelete.value = null;
  statusText.value = `已删除分类「${name}」${cleared ? `（${cleared} 条记录的分类已清空，记录本身保留）` : ''}`;
  saveEntries();
};

// ============================================================ 选择与键盘
const selectEntry = (id, event) => {
  if (suppressRowClick) return;   // 刚拖动完，这一次不当点击
  const isCtrl = event && event.ctrlKey;
  const isShift = event && event.shiftKey;

  if (isShift && lastSelectedId) {
    const list = filteredEntries.value;
    const lastIndex = list.findIndex(item => item.id === lastSelectedId);
    const currentIndex = list.findIndex(item => item.id === id);
    if (lastIndex !== -1 && currentIndex !== -1) {
      const start = Math.min(lastIndex, currentIndex);
      const end = Math.max(lastIndex, currentIndex);
      const next = new Set(selectedIds.value);
      for (let i = start; i <= end; i++) next.add(list[i].id);
      selectedIds.value = next;
      lastSelectedId = id;
      return;
    }
  }

  if (isCtrl) {
    const next = new Set(selectedIds.value);
    if (next.has(id)) next.delete(id);
    else next.add(id);
    selectedIds.value = next;
  } else {
    selectedIds.value = new Set([id]);
  }
  lastSelectedId = id;
};

const clearSelection = () => {
  selectedIds.value = new Set();
};

const scrollToSelected = () => {
  setTimeout(() => {
    const row = document.querySelector('.is-selected');
    if (row) row.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }, 10);
};

const handleKeydown = (e) => {
  if (e.key === 'F12') {
    e.preventDefault();
    return;
  }

  const tag = document.activeElement ? document.activeElement.tagName : '';
  const isTyping = ['INPUT', 'TEXTAREA', 'SELECT'].includes(tag);

  if (e.ctrlKey && e.key.toLowerCase() === 'f') {
    e.preventDefault();
    if (searchInput.value) searchInput.value.focus();
    return;
  }
  if (e.ctrlKey && e.key.toLowerCase() === 'n') {
    e.preventDefault();
    if (vaultState.value === 'unlocked') createEntry();
    return;
  }
  if (e.ctrlKey && e.key.toLowerCase() === 'l') {
    e.preventDefault();
    if (vaultState.value === 'unlocked') lockVault();
    return;
  }
  if (e.key === 'Escape') {
    showEditPanel.value = false;
    showRecovery.value = false;
    showNewCategory.value = false;
    editingCategory.value = null;
    categoryToDelete.value = null;
    steamLoginTarget.value = null;
    showChangePassword.value = false;
    showAbout.value = false;
    contextMenu.value.show = false;
    clearSelection();
    return;
  }

  if (isTyping) return;
  if (showEditPanel.value) return;
  if (filteredEntries.value.length === 0) return;

  if (e.ctrlKey && e.key.toLowerCase() === 'a') {
    e.preventDefault();
    selectedIds.value = new Set(filteredEntries.value.map(item => item.id));
    return;
  }

  const list = filteredEntries.value;
  const currentIndex = list.findIndex(item => item.id === lastSelectedId);

  if (e.key === 'ArrowDown') {
    const nextIndex = currentIndex === -1 ? 0 : (currentIndex + 1) % list.length;
    selectEntry(list[nextIndex].id, e);
    e.preventDefault();
    scrollToSelected();
  } else if (e.key === 'ArrowUp') {
    const prevIndex = currentIndex === -1 ? list.length - 1 : (currentIndex - 1 + list.length) % list.length;
    selectEntry(list[prevIndex].id, e);
    e.preventDefault();
    scrollToSelected();
  } else if (e.key === 'Enter' && selectedIds.value.size > 0) {
    handleEditSelected();
    e.preventDefault();
  } else if (e.key === 'Delete' || e.key === 'Del') {
    if (selectedIds.value.size > 0) {
      handleBulkDelete();
      e.preventDefault();
    }
  }
};

// ============================================================ 右键菜单与设置
const contextMenuRef = ref(null);
const openContextMenu = (e, entry) => {
  e.preventDefault();
  e.stopPropagation();
  selectedIds.value = new Set([entry.id]);
  lastSelectedId = entry.id;
  contextMenu.value = { show: true, x: e.clientX, y: e.clientY, entry: entry };
  // 边界钳位：靠近窗口右/下边缘时把菜单挪回可视区，避免被截断
  nextTick(() => {
    const el = contextMenuRef.value;
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const margin = 8;
    const maxX = window.innerWidth - rect.width - margin;
    const maxY = window.innerHeight - rect.height - margin;
    if (contextMenu.value.x > maxX) contextMenu.value.x = Math.max(margin, maxX);
    if (contextMenu.value.y > maxY) contextMenu.value.y = Math.max(margin, maxY);
  });
};

const closeContextMenu = (e) => {
  if (e && e.target && e.target.closest && e.target.closest('.context-menu')) return;
  contextMenu.value.show = false;
  if (!e || !e.target || !e.target.closest || !e.target.closest('.settings-menu')) {
    showSettings.value = false;
  }
};

const handleContextAction = (action) => {
  const entry = contextMenu.value.entry;
  if (!entry) return;
  contextMenu.value.show = false;

  switch (action) {
    case 'open':
      openEntry(entry);
      break;
    case 'edit':
      selectedIds.value = new Set([entry.id]);
      lastSelectedId = entry.id;
      editingEntry.value = normalizeEntry({ ...entry });
      isNewEntry.value = false;
      showGenerator.value = false;
      qrError.value = '';
      showEditPanel.value = true;
      break;
    case 'copyUsername':
      copyText(entry.username, '账号已复制');
      break;
    case 'copyPassword':
      copyText(entry.password, '密码已复制');
      break;
    case 'copyUrl':
      copyText(entry.url || entry.path, '网址已复制');
      break;
    case 'copyTotp':
      copyTotp(entry);
      break;
    case 'steamLogin':
      requestSteamLogin(entry);
      break;
    case 'recovery':
      openRecovery(entry);
      break;
    case 'delete':
      deleteEntry(entry);
      break;
    default:
      break;
  }
};

const toggleSettingsMenu = (event) => {
  const rect = event.currentTarget.getBoundingClientRect();
  settingsMenuPos.value = { x: rect.left - 120, y: rect.bottom };
  showSettings.value = !showSettings.value;
};

const handleSettingsAction = (action) => {
  showSettings.value = false;
  switch (action) {
    case 'theme':
      isDarkTheme.value = !isDarkTheme.value;
      localStorage.setItem('theme', isDarkTheme.value ? 'dark' : 'light');
      statusText.value = isDarkTheme.value ? '已切换到深色主题' : '已切换到浅色主题';
      saveConfig();
      break;
    case 'changePassword':
      changePasswordForm.value = { current: '', next: '', confirm: '' };
      changePasswordError.value = '';
      showChangePassword.value = true;
      break;
    case 'enableHello':
      enableHello();
      break;
    case 'disableHello':
      disableHello();
      break;
    case 'lock':
      lockVault();
      break;
    case 'about':
      showAbout.value = true;
      break;
    default:
      break;
  }
};

// ============================================================ 窗口控制
const handleWindowControl = (action) => {
  post({ type: 'windowControl', action: action });
};

const handleDragStart = (e) => {
  if (e.button === 0) {
    if (e.target.closest('.win-btn')) return;
    if (e.detail === 2) return;
    if (isOnEdge.value) return;
    e.stopPropagation();
    handleWindowControl('startDrag');
  }
};

const handleMouseMove = (e) => {
  mousePosition.value = { x: e.clientX, y: e.clientY };

  const width = window.innerWidth;
  const height = window.innerHeight;
  const border = resizeBorderSize;
  const titleBarHeight = 32;
  const titleBarTopAreaHeight = 4;

  const isInTitleBarTopArea = e.clientY < titleBarTopAreaHeight;
  const isInTitleBarBody = e.clientY >= titleBarTopAreaHeight && e.clientY < titleBarHeight;

  const left = e.clientX < border;
  const right = e.clientX > width - border;
  const top = e.clientY < border;
  const bottom = e.clientY > height - border;

  let newEdgeType = '';
  let newCursor = 'default';

  if (isInTitleBarTopArea) {
    if (top && left) { newEdgeType = 'top-left'; newCursor = 'nwse-resize'; }
    else if (top && right) { newEdgeType = 'top-right'; newCursor = 'nesw-resize'; }
    else if (bottom && left) { newEdgeType = 'bottom-left'; newCursor = 'nesw-resize'; }
    else if (bottom && right) { newEdgeType = 'bottom-right'; newCursor = 'nwse-resize'; }
    else if (left) { newEdgeType = 'left'; newCursor = 'ew-resize'; }
    else if (right) { newEdgeType = 'right'; newCursor = 'ew-resize'; }
    else if (top) { newEdgeType = 'top'; newCursor = 'ns-resize'; }
    else if (bottom) { newEdgeType = 'bottom'; newCursor = 'ns-resize'; }
  } else if (isInTitleBarBody) {
    if (bottom && left) { newEdgeType = 'bottom-left'; newCursor = 'nesw-resize'; }
    else if (bottom && right) { newEdgeType = 'bottom-right'; newCursor = 'nesw-resize'; }
    else if (left) { newEdgeType = 'left'; newCursor = 'ew-resize'; }
    else if (right) { newEdgeType = 'right'; newCursor = 'ew-resize'; }
    else if (bottom) { newEdgeType = 'bottom'; newCursor = 'ns-resize'; }
  } else {
    if (top && left) { newEdgeType = 'top-left'; newCursor = 'nwse-resize'; }
    else if (top && right) { newEdgeType = 'top-right'; newCursor = 'nesw-resize'; }
    else if (bottom && left) { newEdgeType = 'bottom-left'; newCursor = 'nesw-resize'; }
    else if (bottom && right) { newEdgeType = 'bottom-right'; newCursor = 'nwse-resize'; }
    else if (left) { newEdgeType = 'left'; newCursor = 'ew-resize'; }
    else if (right) { newEdgeType = 'right'; newCursor = 'ew-resize'; }
    else if (top) { newEdgeType = 'top'; newCursor = 'ns-resize'; }
    else if (bottom) { newEdgeType = 'bottom'; newCursor = 'ns-resize'; }
  }

  isOnEdge.value = newEdgeType !== '';
  edgeType.value = newEdgeType;
  resizeCursor.value = newCursor;
};

const handleMouseLeave = () => {
  isOnEdge.value = false;
  edgeType.value = '';
  resizeCursor.value = 'default';
};

const handleResizeStart = (e) => {
  if (e.button !== 0) return;
  if (!isOnEdge.value) return;

  if (e.target.closest('.custom-titlebar')) {
    if (edgeType.value !== 'left' && edgeType.value !== 'right' &&
        edgeType.value !== 'top' && edgeType.value !== 'top-left' && edgeType.value !== 'top-right') {
      return;
    }
  }

  e.preventDefault();
  e.stopPropagation();

  const actions = {
    'left': 'startResizeLeft',
    'right': 'startResizeRight',
    'top': 'startResizeTop',
    'bottom': 'startResizeBottom',
    'top-left': 'startResizeTopLeft',
    'top-right': 'startResizeTopRight',
    'bottom-left': 'startResizeBottomLeft',
    'bottom-right': 'startResizeBottomRight'
  };
  const action = actions[edgeType.value];
  if (action) handleWindowControl(action);
};

// ============================================================ 生命周期
onMounted(() => {
  window.addEventListener('keydown', handleKeydown);
  window.addEventListener('click', closeContextMenu);
  window.addEventListener('paste', handleQrPaste);
  window.addEventListener('wheel', (e) => {
    if (e.ctrlKey) e.preventDefault();
  }, { passive: false });

  if (hasHost()) {
    window.chrome.webview.addEventListener('message', handleHostMessage);
    post({ type: 'getVaultStatus' });
    post({ type: 'getConfig' });
  } else {
    // 浏览器内预览：不连接宿主时使用示例数据
    entries.value = [
      { id: 'demo_1', name: 'GitHub', type: 'website', url: 'https://github.com', username: 'noob-xiaoyu', password: 'Demo#Passw0rd', category: '开发', notes: '示例条目', totpSecret: 'JBSWY3DPEHPK3PXP', recoveryCodes: 'aaaa-1111\nbbbb-2222\ncccc-3333', usedRecoveryCodes: [] },
      { id: 'demo_2', name: 'Steam', type: 'software', path: 'C:\\Program Files (x86)\\Steam\\steam.exe', username: 'demo_user', password: 'another-pass', category: '游戏', notes: '' },
      { id: 'demo_3', name: 'Google 恢复代码', type: 'recovery', url: 'https://accounts.google.com', username: 'demo@gmail.com', category: '邮箱', notes: '启用两步验证时保存', recoveryCodes: '1234-5678-9012\n2345-6789-0123\n3456-7890-1234', usedRecoveryCodes: [] },
      { id: 'demo_4', name: '大号', type: 'steam', username: 'demo_steam_main', password: 'steam-pass-1', steamId: '76561198000000001', totpSecret: 'cnOgv/KdpLoP6Nbh0GMkXkPXALQ=', totpAlgorithm: 'steam', revocationCode: 'R12345', category: 'Steam', notes: '示例 Steam 账号' },
      { id: 'demo_5', name: '小号', type: 'steam', username: 'demo_steam_alt', password: 'steam-pass-2', steamId: '76561198000000002', category: 'Steam', notes: '' }
    ];
    vaultState.value = 'unlocked';
  }

  // 2FA 动态码每秒刷新一次
  refreshTotp();
  totpTimer = setInterval(refreshTotp, 1000);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleKeydown);
  window.removeEventListener('click', closeContextMenu);
  window.removeEventListener('paste', handleQrPaste);
  if (saveTimer) clearTimeout(saveTimer);
  if (totpTimer) clearInterval(totpTimer);
});
</script>

<template>
  <div class="app-container" :class="{ 'is-busy': isBusy, 'light-theme': !isDarkTheme }"
       @mousemove="handleMouseMove"
       @mouseleave="handleMouseLeave"
       @mousedown="handleResizeStart"
       :style="{ cursor: resizeCursor }">
    <!-- 自定义标题栏 -->
    <div class="custom-titlebar"
         @dblclick="handleWindowControl('maximize')"
         @mousedown="handleDragStart">
      <div class="title-content">
        <span class="app-title">Account Manager</span>
        <span class="title-vault-state">{{ vaultState === 'unlocked' ? '🔓 已解锁' : '🔒 已锁定' }}</span>
      </div>
      <div class="window-controls">
        <button class="win-btn settings-btn" @click.stop="toggleSettingsMenu" @dblclick.stop title="设置">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="12" cy="12" r="3"/>
            <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/>
          </svg>
        </button>
        <button class="win-btn" @click="handleWindowControl('minimize')" title="最小化">
          <svg viewBox="0 0 10 10"><path d="M0 5h10v1H0z" fill="currentColor"/></svg>
        </button>
        <button class="win-btn" @click="handleWindowControl('maximize')" title="最大化/还原">
          <svg viewBox="0 0 10 10"><path d="M0 0v10h10V0H0zm1 1h8v8H1V1z" fill="currentColor"/></svg>
        </button>
        <button class="win-btn close" @click="handleWindowControl('close')" title="关闭">
          <svg viewBox="0 0 10 10"><path d="M0 0l10 10M10 0L0 10" stroke="currentColor" stroke-width="1.2"/></svg>
        </button>
      </div>
    </div>

    <!-- 加载中 -->
    <div v-if="vaultState === 'loading'" class="vault-screen">
      <div class="vault-card">
        <div class="vault-icon">⏳</div>
        <h2 class="vault-title">正在读取保险库...</h2>
      </div>
    </div>

    <!-- 首次运行：设置主密码 -->
    <div v-else-if="vaultState === 'setup'" class="vault-screen">
      <div class="vault-card">
        <div class="vault-icon">🔐</div>
        <h2 class="vault-title">设置主密码</h2>
        <p class="vault-hint">
          所有网站与软件的账号密码都会用主密码派生的密钥加密后保存在程序目录的
          <code>vault.json</code> 中（PBKDF2-HMAC-SHA256 + AES-256-CBC）。
          主密码不会被保存，一旦忘记将无法解密数据。
        </p>
        <div class="vault-field">
          <input v-model="masterPassword" :type="showMasterPassword ? 'text' : 'password'"
                 placeholder="主密码（至少 6 位）" @keyup.enter="createVault" />
          <button class="toggle-btn" @click="showMasterPassword = !showMasterPassword" title="显示/隐藏">👁</button>
        </div>
        <div class="vault-field">
          <input v-model="confirmPassword" :type="showMasterPassword ? 'text' : 'password'"
                 placeholder="再次输入主密码" @keyup.enter="createVault" />
        </div>
        <div v-if="unlockError" class="vault-error">{{ unlockError }}</div>
        <button class="btn success vault-submit" @click="createVault">创建保险库</button>
      </div>
    </div>

    <!-- 已存在保险库：解锁 -->
    <div v-else-if="vaultState === 'locked'" class="vault-screen">
      <div class="vault-card">
        <div class="vault-icon">🔒</div>
        <h2 class="vault-title">保险库已锁定</h2>
        <p class="vault-hint">输入主密码以解密账号数据</p>
        <div class="vault-field">
          <input v-model="masterPassword" :type="showMasterPassword ? 'text' : 'password'"
                 placeholder="主密码" ref="unlockPasswordInput" @keyup.enter="unlockVault" />
          <button class="toggle-btn" @click="showMasterPassword = !showMasterPassword" title="显示/隐藏">👁</button>
        </div>
        <div v-if="unlockError" class="vault-error">{{ unlockError }}</div>
        <button class="btn success vault-submit" :disabled="helloBusy" @click="unlockVault">解锁</button>
        <template v-if="helloEnabled">
          <div class="vault-divider"><span>或</span></div>
          <button class="btn vault-submit hello-btn" :disabled="helloBusy" @click="helloUnlock">
            {{ helloBusy ? '⏳ 等待 Windows 安全中心验证…' : '🪟 使用 Windows Hello 解锁' }}
          </button>
          <p class="vault-hint" style="text-align:center;">使用 PIN / 指纹 / 人脸通过 Windows 安全中心验证</p>
        </template>
      </div>
    </div>

    <!-- 主界面 -->
    <div v-else class="layout-body">
      <!-- 左侧导航 -->
      <aside class="sidebar" :class="{ 'collapsed': isSidebarCollapsed }">
        <div class="sidebar-header">
          <span v-if="!isSidebarCollapsed">🔐</span>
          <button @click="toggleSidebar" class="toggle-sidebar-btn">
            {{ isSidebarCollapsed ? '→' : '←' }}
          </button>
        </div>
        <nav class="nav-menu">
          <button @click="currentFilter = 'all'" :class="['nav-item', { active: currentFilter === 'all' }]" :title="isSidebarCollapsed ? '全部条目' : ''">
            <span class="icon">📋</span>
            <span class="label">全部条目</span>
          </button>
          <button @click="currentFilter = 'website'" :class="['nav-item', { active: currentFilter === 'website' }]" :title="isSidebarCollapsed ? '网站密码' : ''">
            <span class="icon">🌐</span>
            <span class="label">网站密码</span>
          </button>
          <button @click="currentFilter = 'software'" :class="['nav-item', { active: currentFilter === 'software' }]" :title="isSidebarCollapsed ? '软件密码' : ''">
            <span class="icon">💻</span>
            <span class="label">软件密码</span>
          </button>
          <button @click="currentFilter = 'steam'" :class="['nav-item', { active: currentFilter === 'steam' }]" :title="isSidebarCollapsed ? 'Steam 账号' : ''">
            <span class="icon">🎮</span>
            <span class="label">Steam 账号</span>
          </button>
          <button @click="currentFilter = 'recovery'" :class="['nav-item', { active: currentFilter === 'recovery' }]" :title="isSidebarCollapsed ? '2FA 恢复代码' : ''">
            <span class="icon">📄</span>
            <span class="label">恢复代码</span>
          </button>
          <!-- 分类：可在侧边栏直接新建 / 重命名 / 删除 -->
          <div v-if="!isSidebarCollapsed" class="nav-section-head">
            <span class="nav-section-title">分类</span>
            <button class="nav-cat-btn" title="新建分类" @click="openNewCategory">＋</button>
          </div>
          <div v-if="!isSidebarCollapsed && showNewCategory" class="nav-cat-edit">
            <input v-model="newCategoryName" placeholder="新分类名" autofocus
                   @keyup.enter="addCategory" @keyup.esc="showNewCategory = false" />
            <button class="nav-cat-btn ok" title="确定" @click="addCategory">✓</button>
            <button class="nav-cat-btn" title="取消" @click="showNewCategory = false">✕</button>
          </div>
          <div v-for="name in allCategories" :key="name" class="nav-cat-row">
            <template v-if="editingCategory && editingCategory.original === name">
              <div class="nav-cat-edit">
                <input v-model="editingCategory.value" autofocus
                       @keyup.enter="confirmRenameCategory" @keyup.esc="cancelRenameCategory" />
                <button class="nav-cat-btn ok" title="确定" @click="confirmRenameCategory">✓</button>
                <button class="nav-cat-btn" title="取消" @click="cancelRenameCategory">✕</button>
              </div>
            </template>
            <template v-else>
              <button @click="currentFilter = 'cat:' + name"
                      :class="['nav-item', 'nav-cat-item', { active: currentFilter === 'cat:' + name }]"
                      :title="isSidebarCollapsed ? name : name">
                <span class="icon">🏷️</span>
                <span class="label">{{ name }}</span>
                <span v-if="!isSidebarCollapsed && categoryUsage(name)" class="nav-cat-count">{{ categoryUsage(name) }}</span>
              </button>
              <div v-if="!isSidebarCollapsed" class="nav-cat-actions">
                <button class="nav-cat-btn" title="重命名分类" @click.stop="startRenameCategory(name)">✏️</button>
                <button class="nav-cat-btn danger" title="删除分类" @click.stop="requestDeleteCategory(name)">🗑️</button>
              </div>
            </template>
          </div>
          <div v-if="!isSidebarCollapsed && !allCategories.length && !showNewCategory" class="nav-cat-empty">
            还没有分类，点右上「＋」新建
          </div>
        </nav>
      </aside>

      <!-- 右侧主区域 -->
      <div class="main-content">
        <header class="toolbar">
          <div class="view-title">{{ viewTitle }}</div>
          <div class="spacer"></div>
          <div class="search-box">
            <span>🔍</span>
            <input ref="searchInput" v-model="searchQuery" type="text" placeholder="搜索名称 / 账号 / 网址 / 备注 (Ctrl+F)" />
            <button v-if="searchQuery" class="clear-search" @click="searchQuery = ''" title="清除">✕</button>
          </div>
          <button v-if="currentFilter === 'steam'" class="btn" @click="importSteamAccounts">📥 从本机 Steam 导入</button>
          <button @click="createEntry" class="btn btn-blue">➕ 新增</button>
          <button @click="handleEditSelected" class="btn" :disabled="!hasSelection">✏️ 编辑</button>
          <button @click="handleBulkDelete" class="btn btn-red" :disabled="!hasSelection">🗑️ 删除</button>
          <button @click="lockVault" class="btn">🔒 锁定</button>
        </header>

        <main class="table-container">
          <div class="table-section">
            <table class="data-grid">
              <thead>
                <tr>
                  <th style="width: 30px"></th>
                  <th style="width: 150px">名称</th>
                  <th style="width: 130px">账号</th>
                  <th style="width: 130px">密码</th>
                  <th style="width: 120px">2FA 动态码</th>
                  <th>网址 / 程序 / SteamID64</th>
                  <th style="width: 90px">分类</th>
                  <th style="width: 120px">备注</th>
                </tr>
              </thead>
              <tbody>
                <template v-for="(entry, index) in filteredEntries" :key="entry.id">
                <tr v-if="dragGapPos === index" class="drag-gap-row"><td colspan="8"><div class="drag-gap-box"></div></td></tr>
                <tr
                    :class="{ 'is-selected': selectedIds.has(entry.id), 'row-dragging': draggingId === entry.id }"
                    @click="selectEntry(entry.id, $event)"
                    @contextmenu="openContextMenu($event, entry)">
                    <td class="drag-cell">
                      <span class="drag-handle" @mousedown.prevent="beginRowDrag(entry, $event)" title="按住拖动排序">⠿</span>
                    </td>
                  <td>
                    <span class="type-icon">{{ entryType(entry) === 'software' ? '💻' : (entryType(entry) === 'recovery' ? '📄' : (entryType(entry) === 'steam' ? '🎮' : '🌐')) }}</span>
                    <span class="entry-name">{{ entry.name || '(未命名)' }}</span>
                  </td>
                  <td>{{ entry.username || '---' }}</td>
                  <td>
                    <button v-if="entryType(entry) === 'recovery'" class="btn-sm login" @click.stop="openRecovery(entry)">
                      📄 恢复码 {{ recoveryRemaining(entry) }}/{{ recoveryCount(entry) }}
                    </button>
                    <!-- 列表只读：点击复制，修改密码请用「✏️ 编辑」 -->
                    <div v-else class="pwd-cell">
                      <span v-if="entry.password"
                            class="pwd-text mono"
                            :class="{ 'is-revealed': visiblePasswords[entry.id] }"
                            :title="visiblePasswords[entry.id] ? '点击复制密码' : '点击复制密码（按住右侧眼睛可查看）'"
                            @click.stop="copyText(entry.password, '密码已复制')">{{ visiblePasswords[entry.id] ? entry.password : maskText(entry.password) }}</span>
                      <span v-else class="muted-cell">（无密码）</span>
                      <button v-if="entry.password" class="toggle-btn" @click.stop="togglePassword(entry.id)" title="显示/隐藏">
                        <svg v-if="visiblePasswords[entry.id]" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                          <path d="M2 12C2 12 5 5 12 5C19 5 22 12 22 12C22 12 19 19 12 19C5 19 2 12 2 12Z" />
                          <circle cx="12" cy="12" r="3" />
                          <line x1="3" y1="3" x2="21" y2="21" />
                        </svg>
                        <svg v-else width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                          <path d="M2 12C2 12 5 5 12 5C19 5 22 12 22 12C22 12 19 19 12 19C5 19 2 12 2 12Z" />
                          <circle cx="12" cy="12" r="3" />
                        </svg>
                      </button>
                    </div>
                  </td>
                  <td>
                    <div v-if="hasTotp(entry) && totpCodes[entry.id] && totpCodes[entry.id].code" class="totp-cell">
                      <span class="totp-code mono" @click.stop="copyTotp(entry)"
                            :title="'点击复制，' + totpCodes[entry.id].remaining + ' 秒后刷新'">{{ totpCodes[entry.id].code }}</span>
                      <span class="totp-remaining">{{ totpCodes[entry.id].remaining }}s</span>
                    </div>
                    <span v-else-if="hasTotp(entry)" class="totp-error">
                      {{ totpCodes[entry.id] && totpCodes[entry.id].error ? totpCodes[entry.id].error : '······' }}
                    </span>
                    <span v-else class="muted-cell">—</span>
                  </td>
                  <td class="link-cell"><span class="link-text" @click="entryType(entry) === 'steam' ? openSteamProfile(entry) : openEntry(entry)">
                    <template v-if="entryType(entry) === 'steam'">{{ entry.steamId || '未填写 SteamID64' }}</template>
                    <template v-else>{{ entryType(entry) === 'software' ? (entry.path || '---') : (entry.url || '---') }}</template>
                    </span>
                  </td>
                  <td><span v-if="entry.category" class="tag">{{ entry.category }}</span></td>
                  <td>
                    <input v-model="entry.notes" class="edit-input" placeholder="点击编辑备注..." />
                  </td>
                </tr>
                </template>
                <tr v-if="draggingId !== null && dragGapPos === filteredEntries.length" class="drag-gap-row"><td colspan="8"><div class="drag-gap-box"></div></td></tr>
                <tr v-if="filteredEntries.length === 0">
                  <td colspan="8" style="text-align: center; padding: 40px; color: var(--text-muted);">
                    {{ searchQuery ? '没有匹配的记录' : '这里还没有记录，点击右上角「➕ 新增」' }}
                  </td>
                </tr>
              </tbody>
            </table>

            <!-- 选中单个账号后的底部悬浮操作栏 -->
            <Transition name="modal-fade">
              <div v-if="floatingEntry" class="floating-bar">
                <span class="floating-name">{{ floatingEntry.name || '(未命名)' }}</span>
                <TransitionGroup name="bar-btn" tag="div" class="floating-items">
                <button :key="'bar1'" v-if="entryType(floatingEntry) === 'steam'" class="btn-sm steam" :disabled="steamLoggingIn" @click="requestSteamLogin(floatingEntry)">🚀 登录</button>
                <button :key="'bar2'" v-if="entryType(floatingEntry) === 'recovery'" class="btn-sm login" @click="openRecovery(floatingEntry)">📄 恢复码</button>
                <button :key="'bar3'" v-else class="btn-sm login" @click="copyText(floatingEntry.password, '密码已复制')">复制密码</button>
                <button :key="'bar4'" v-if="hasTotp(floatingEntry)" class="btn-sm totp" @click="copyTotp(floatingEntry)">2FA</button>
                <button :key="'bar5'" class="btn-sm" @click="handleEditSelected">✏️ 编辑</button>
                <button :key="'bar6'" class="btn-sm delete" @click="deleteEntry(floatingEntry)">🗑️ 删除</button>
                <button :key="'bar7'" class="btn-sm" @click="clearSelection">✕</button>
                </TransitionGroup>
              </div>
            </Transition>
          </div>
        </main>
      </div>
    </div>

    <!-- 弹窗：新增 / 编辑条目 -->
    <Transition name="modal-fade">
      <div v-if="showEditPanel && editingEntry" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { showEditPanel = false })">
        <div class="modal-dialog edit-panel">
          <h3 class="modal-title">{{ isNewEntry ? '➕ 新增条目' : '✏️ 编辑条目' }}</h3>
          <div class="edit-grid">
            <div class="input-group">
              <label>名称（网站或软件名）:</label>
              <input v-model="editingEntry.name" type="text" placeholder="例如 GitHub / Steam" />
            </div>
            <div class="input-group">
              <label>类型:</label>
              <select v-model="editingEntry.type">
                <option value="website">🌐 网站</option>
                <option value="software">💻 软件</option>
                <option value="steam">🎮 Steam 账号</option>
                <option value="recovery">📄 恢复代码</option>
              </select>
            </div>
            <div class="input-group" v-if="editingEntry.type !== 'software' && editingEntry.type !== 'steam'">
              <label>网址:</label>
              <input v-model="editingEntry.url" type="text" placeholder="https://example.com" />
            </div>
            <div class="input-group" v-else-if="editingEntry.type === 'software'">
              <label>程序路径:</label>
              <input v-model="editingEntry.path" type="text" placeholder="C:\Program Files\App\app.exe" />
            </div>
            <div class="input-group">
              <label>{{ editingEntry.type === 'steam' ? 'Steam 账号名:' : '账号 / 用户名:' }}</label>
              <input v-model="editingEntry.username" type="text" />
            </div>
            <div class="input-group" v-if="editingEntry.type !== 'recovery'">
              <label>{{ editingEntry.type === 'steam' ? 'Steam 密码:' : '密码:' }}</label>
              <div class="input-wrapper">
                <input v-model="editingEntry.password" :type="showMasterPassword ? 'text' : 'password'" autocomplete="new-password" />
                <button class="toggle-btn" @click="showMasterPassword = !showMasterPassword" title="显示/隐藏">👁</button>
              </div>
            </div>
            <!-- Steam 专有字段 -->
            <div class="input-group" v-if="editingEntry.type === 'steam'">
              <label>SteamID64:</label>
              <div class="qr-row">
                <input v-model="editingEntry.steamId" type="text" placeholder="76561198xxxxxxxxx" spellcheck="false" />
                <button class="btn" type="button"
                        @click="openExternal('https://steamcommunity.com/profiles/' + editingEntry.steamId)">主页</button>
              </div>
            </div>
            <div class="input-group" v-if="editingEntry.type === 'steam'">
              <label>撤销码:</label>
              <input v-model="editingEntry.revocationCode" type="text" placeholder="R12345" spellcheck="false" />
            </div>
            <div class="input-group" style="grid-column: span 2;">
              <label>TOTP 密钥（Base32 / otpauth:// 链接；Steam 可用 .maFile 或 Base64 shared_secret）:</label>
              <div class="qr-row" @dragover.prevent @drop.prevent="handleQrDrop">
                <input v-model="editingEntry.totpSecret" type="text" spellcheck="false"
                       @change="normalizeTotpField"
                       :placeholder="editingEntry.totpAlgorithm === 'steam' ? '例如 cnOgv/KdpLoP6Nbh0GMkXkPXALQ= 或 .maFile 内容' : '例如 JBSWY3DPEHPK3PXP 或 otpauth://totp/GitHub?secret=...'" />
                <select v-if="editingEntry.type === 'steam'" v-model="editingEntry.totpAlgorithm" class="algo-select" title="2FA 类型">
                  <option value="steam">🎮 Steam Guard</option>
                  <option value="totp">🔢 标准 TOTP</option>
                </select>
                <button class="btn" :disabled="qrDecoding" @click="pickQrImage">
                  {{ qrDecoding ? '识别中…' : '📷 / 📄 导入' }}
                </button>
                <input ref="qrInput" type="file" accept="image/*,.mafile,.json,.txt,application/json,text/plain" class="hidden-file" @change="handleQrFileChange" />
              </div>
              <span class="field-hint">可选二维码截图、把文件拖到上面、或直接 Ctrl+V 粘贴截图；Steam 可拖入 SDA 导出的 .maFile</span>
              <span v-if="qrError" class="field-hint error">{{ qrError }}</span>
              <span v-else-if="totpError(editingEntry)" class="field-hint error">{{ totpError(editingEntry) }}</span>
              <div v-if="editingEntry.type === 'steam'" class="enroll-row">
                <button class="btn btn-blue" @click="openEnrollModal(editingEntry)">🛡️ 应用内添加令牌（自动获取密钥）</button>
                <button v-if="editingEntry.totpSecret || editingEntry.revocationCode" class="btn" @click="openEnrollModal(editingEntry, 'remove')">🗑️ 移除该账号的手机令牌</button>
                <span class="field-hint">登录后由 Steam 下发密钥并直接写入本条目</span>
              </div>
            </div>
            <div class="input-group" style="grid-column: span 2;">
              <label>2FA 恢复代码（每行一个）:</label>
              <textarea v-model="editingEntry.recoveryCodes" rows="3"
                        placeholder="启用两步验证时网站给出的备用代码，一行一个"></textarea>
              <span class="field-hint">
                共 {{ recoveryCount(editingEntry) }} 条，未使用 {{ recoveryRemaining(editingEntry) }} 条
                <template v-if="recoveryCount(editingEntry)">
                  · <a href="#" @click.prevent="openRecovery(editingEntry)">查看 / 逐条复制</a>
                </template>
              </span>
            </div>
            <div class="input-group">
              <label>分类（可留空）:</label>
              <input v-model="editingEntry.category" type="text" placeholder="例如 工作 / 邮箱 / 游戏" list="category-options" />
              <datalist id="category-options">
                <option v-for="name in allCategories" :key="name" :value="name"></option>
              </datalist>
            </div>
            <div class="input-group">
              <label>操作:</label>
              <button class="btn" @click="toggleGenerator">
                🎲 密码生成器
              </button>
            </div>
            <div class="input-group" style="grid-column: span 2;">
              <label>备注:</label>
              <textarea v-model="editingEntry.notes" rows="2" placeholder="密保问题、绑定邮箱等"></textarea>
            </div>
            <div v-if="showGenerator" class="generator-box">
              <div class="generator-row">
                <label>长度</label>
                <input type="range" min="6" max="48" v-model.number="generator.length" @input="generatePassword" />
                <span class="mono">{{ generator.length }}</span>
              </div>
              <div class="generator-row options">
                <label><input type="checkbox" v-model="generator.upper" @change="generatePassword" /> 大写字母</label>
                <label><input type="checkbox" v-model="generator.lower" @change="generatePassword" /> 小写字母</label>
                <label><input type="checkbox" v-model="generator.digits" @change="generatePassword" /> 数字</label>
                <label><input type="checkbox" v-model="generator.symbols" @change="generatePassword" /> 符号</label>
              </div>
              <div class="generator-preview mono">{{ generator.preview || '点击「生成」预览随机密码' }}</div>
              <div class="generator-actions">
                <button class="btn-sm login" @click="generatePassword">重新生成</button>
                <button class="btn-sm" @click="applyGenerated">使用该密码</button>
              </div>
            </div>
          </div>
          <div class="edit-actions">
            <button @click="saveEdit" class="btn success">✅ 保存</button>
            <button @click="showEditPanel = false" class="btn">❌ 取消</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：2FA 恢复代码 -->
    <Transition name="modal-fade">
      <div v-if="showRecovery && recoveryEntry" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { showRecovery = false })">
        <div class="modal-dialog recovery-panel">
          <h3 class="modal-title">📄 2FA 恢复代码</h3>
          <div class="recovery-head">
            <div>
              <div class="recovery-name">{{ recoveryEntry.name || '(未命名)' }}</div>
              <div class="recovery-sub">
                {{ recoveryEntry.username || '未填写账号' }} · 未使用 {{ recoveryRemaining(recoveryEntry) }} / {{ recoveryCount(recoveryEntry) }} 条
              </div>
            </div>
            <button class="btn-sm login" @click="copyAllRecovery">复制全部未使用</button>
          </div>
          <div class="recovery-list">
            <div v-for="code in splitRecoveryCodes(recoveryEntry.recoveryCodes)" :key="code"
                 class="recovery-item" :class="{ 'is-used': isCodeUsed(recoveryEntry, code) }">
              <label class="recovery-check" title="标记为已使用">
                <input type="checkbox" :checked="isCodeUsed(recoveryEntry, code)"
                       @change="toggleCodeUsed(recoveryEntry, code)" />
              </label>
              <span class="recovery-code mono">{{ code }}</span>
              <button class="btn-sm" @click="copyText(code, '恢复代码已复制')">复制</button>
            </div>
            <div v-if="recoveryCount(recoveryEntry) === 0" class="recovery-empty">
              该条目还没有填写恢复代码。可在「✏️ 编辑」面板的「2FA 恢复代码」里逐行填入。
            </div>
          </div>
          <div class="edit-actions">
            <button @click="showRecovery = false" class="btn">关闭</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：修改主密码 -->
    <Transition name="modal-fade">
      <div v-if="showChangePassword" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { showChangePassword = false })">
        <div class="modal-dialog" style="width: 420px;">
          <h3 class="modal-title">🔑 修改主密码</h3>
          <div class="input-group" style="margin-bottom: 12px;">
            <label>当前主密码:</label>
            <input v-model="changePasswordForm.current" type="password" autocomplete="off" />
          </div>
          <div class="input-group" style="margin-bottom: 12px;">
            <label>新主密码（至少 6 位）:</label>
            <input v-model="changePasswordForm.next" type="password" autocomplete="off" />
          </div>
          <div class="input-group" style="margin-bottom: 12px;">
            <label>确认新主密码:</label>
            <input v-model="changePasswordForm.confirm" type="password" autocomplete="off" />
          </div>
          <div v-if="changePasswordError" class="vault-error">{{ changePasswordError }}</div>
          <div class="edit-actions">
            <button @click="submitChangePassword" class="btn success">确认修改</button>
            <button @click="showChangePassword = false" class="btn">取消</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：Steam 一键登录确认 -->
    <Transition name="modal-fade">
      <div v-if="steamLoginTarget" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { steamLoginTarget = null })">
        <div class="modal-dialog" style="width: 430px;">
          <h3 class="modal-title">🚀 Steam 一键登录</h3>
          <p class="cat-delete-text" style="text-align: left; margin-bottom: 6px;">
            将以 <strong>{{ steamLoginTarget.username }}</strong> 登录，过程中会：
          </p>
          <ul class="steam-login-steps">
            <li>结束当前正在运行的 <code>steam.exe</code>（没在运行则跳过）</li>
            <li>写入注册表 <code>HKCU\Software\Valve\Steam</code> 的 <code>AutoLoginUser</code> 与 <code>RememberPassword</code></li>
            <li>以 <code>-login 账号 密码</code> 参数启动 Steam</li>
          </ul>
          <div v-if="steamGuardCodeForTarget" class="steam-guard-box">
            <span class="steam-guard-label">Steam Guard 验证码</span>
            <span class="steam-guard-code mono">{{ steamGuardCodeForTarget }}</span>
            <button class="btn-sm steam" @click="copyTotp(steamLoginTarget)">复制</button>
            <span class="field-hint">Steam 弹出要求输入时用这个（30 秒刷新）</span>
          </div>
          <p class="cat-delete-text" style="text-align: left; color: #e0a030;">
            ⚠️ 密码会作为启动参数出现在进程命令行里，登录瞬间可能被同机其他程序读到。
          </p>
          <div class="edit-actions">
            <button class="btn success" :disabled="steamLoggingIn" @click="confirmSteamLogin">确认登录</button>
            <button class="btn" @click="steamLoginTarget = null">取消</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：应用内添加 Steam 令牌 -->
    <Transition name="modal-fade">
      <div v-if="showEnrollModal" class="modal-overlay">
        <div class="modal-dialog" style="width: 470px;">
          <h3 class="modal-title" v-if="enrollMode !== 'remove'">🛡️ 给账号添加 Steam 令牌</h3>
          <h3 class="modal-title" v-else>🗑️ 移除账号的手机令牌</h3>

          <!-- 1. 风险确认 -->
          <div v-show="!enrollExitConfirm">
          <div v-if="enrollStep === 'warning' && enrollMode === 'remove'">
            <p class="cat-delete-text" style="text-align: left; margin-bottom: 6px;">
              将移除 <strong>{{ enrollTarget && (enrollTarget.username || enrollTarget.name) }}</strong> 的手机令牌：
            </p>
            <ul class="steam-login-steps">
              <li>需要先登录该账号（可能要求邮箱验证码或当前令牌动态码）</li>
              <li>需要提供该令牌的 <b>撤销码</b>（revocation code）</li>
              <li>移除后<b>本应用将无法再为该账号生成动态码</b>，账号退回邮箱保护</li>
              <li>令牌已不在你手上时，这是唯一的移除途径</li>
            </ul>
            <div class="edit-actions">
              <button class="btn btn-blue" @click="enrollStep = 'credentials'">我明白，继续</button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <div v-if="enrollStep === 'warning' && enrollMode === 'add'">
            <p class="cat-delete-text" style="text-align: left; margin-bottom: 6px;">
              将对 <strong>{{ enrollTarget && (enrollTarget.username || enrollTarget.name) }}</strong> 执行以下操作：
            </p>
            <ul class="steam-login-steps">
              <li>登录 Steam 并调用官方接口<b>新增手机令牌</b>（账号若已有令牌会失败，需先在手机上移除）</li>
              <li>原本没有手机令牌的账号会进入 <b>7 天交易 / 市场冷却</b></li>
              <li>成功后 <b>手机上的旧令牌将被本应用取代</b></li>
              <li>过程中需要能接收短信或邮件的验证方式</li>
            </ul>
            <p class="cat-delete-text" style="text-align: left; color: #e0a030;">
              ⚠️ 成功后请务必保存「撤销码」—— 应用或手机丢失时，它是唯一的退路。
            </p>
            <div class="edit-actions">
              <button class="btn btn-blue" @click="enrollStep = 'credentials'">我明白，继续</button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <!-- 2. 账号密码 -->
          <div v-else-if="enrollStep === 'credentials'">
            <div class="input-group">
              <label>Steam 账号名:</label>
              <input v-model="enrollUsername" type="text" spellcheck="false" placeholder="账号名（不是昵称）" />
            </div>
            <div class="input-group">
              <label>密码:</label>
              <input v-model="enrollPassword" type="password" placeholder="Steam 登录密码" @keyup.enter="startEnroll" />
            </div>
            <div class="input-group" v-if="enrollMode === 'remove'">
              <label>撤销码（revocation code）:</label>
              <input v-model="enrollRevocation" type="text" spellcheck="false" placeholder="R12345" />
            </div>

            <p class="field-hint">密码只用于本次登录请求（RSA 加密后发给 Steam），不会以明文保存或记录。</p>
            <p v-if="enrollError" class="field-hint error">{{ enrollError }}</p>
            <div class="edit-actions">
              <button class="btn success" :disabled="enrollBusy" @click="startEnroll">
                {{ enrollBusy ? '登录中…' : '开始' }}
              </button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <!-- 3. 需要 Steam Guard / 邮箱验证码 -->
          <div v-else-if="enrollStep === 'guard'">
            <p class="cat-delete-text" style="text-align: left;">
              该账号开启了保护，请提供{{ enrollCodeType === 2 ? '邮箱验证码' : 'Steam Guard 验证码' }}：
            </p>
            <p v-if="enrollHint" class="field-hint">Steam 提示：{{ enrollHint }}</p>
            <div class="input-group">
              <label>{{ enrollCodeType === 2 ? '邮箱验证码' : '当前令牌动态码' }}:</label>
              <input v-model="enrollCode" type="text" spellcheck="false" @keyup.enter="submitEnrollCode" />
            </div>
            <p v-if="enrollError" class="field-hint error">{{ enrollError }}</p>
            <div class="edit-actions">
              <button class="btn success" :disabled="enrollBusy" @click="submitEnrollCode">
                {{ enrollBusy ? '提交中…' : '提交' }}
              </button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <!-- 4. 需要手机 App 确认 -->
          <div v-else-if="enrollStep === 'confirm'">
            <p class="cat-delete-text" style="text-align: left;">
              请在手机 Steam App 上确认本次登录（或点击邮件里的确认链接），然后点下面的按钮继续。
            </p>
            <p v-if="enrollError" class="field-hint error">{{ enrollError }}</p>
            <div class="edit-actions">
              <button class="btn success" :disabled="enrollBusy" @click="continueEnrollPoll">
                {{ enrollBusy ? '等待确认…' : '我已确认，继续' }}
              </button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <!-- 5. 激活码 -->
          <div v-else-if="enrollStep === 'activation'">
            <p class="cat-delete-text" style="text-align: left;">
              Steam 已下发令牌密钥，最后一步：输入 <b>{{ enrollCodeLabel }}</b> 完成激活。
            </p>
            <p v-if="enrollHint" class="field-hint">发送目标：{{ enrollHint }}</p>
            <div class="input-group">
              <label>{{ enrollCodeLabel }}:</label>
              <input v-model="enrollCode" type="text" spellcheck="false" @keyup.enter="submitEnrollCode" />
            </div>
            <p v-if="enrollError" class="field-hint error">{{ enrollError }}</p>
            <div class="edit-actions">
              <button class="btn success" :disabled="enrollBusy" @click="submitEnrollCode">
                {{ enrollBusy ? '激活中…' : '完成激活' }}
              </button>
              <button class="btn" @click="requestCancelEnroll">取消</button>
            </div>
          </div>

          <!-- 6. 完成 -->
          <div v-else-if="enrollStep === 'done'">
            <p class="cat-delete-text" style="text-align: left; color: #2e9e5b;">
              ✅ 令牌已添加并写入本条目，2FA 动态码列现在会显示 5 位码。
            </p>
            <div v-if="enrollResult && !enrollResult.removed" class="revocation-box">
              <div class="revocation-label">撤销码（revocation code）—— 请抄下来单独保存</div>
              <div class="revocation-value mono">{{ enrollResult.revocationCode || '（Steam 未返回）' }}</div>
              <button class="btn-sm" @click="copyText(enrollResult.revocationCode, '撤销码已复制')">复制撤销码</button>
            </div>
            <p class="field-hint">撤销码用于日后移除该令牌；同时它也是账号被他人添加令牌时的救命码，请离线保存。</p>
            <div class="edit-actions">
              <button class="btn success" @click="closeEnrollModal">完成</button>
            </div>
          </div>
          </div>

          <!-- 二级确认：退出令牌登记 -->
          <div v-if="enrollExitConfirm">
            <p class="cat-delete-text" style="text-align: left;">
              令牌登记尚未完成：现在退出需要从头再来，账号上还可能留下「待激活」的令牌。
            </p>
            <div class="edit-actions">
              <button class="btn" @click="cancelExitEnroll">继续登记</button>
              <button class="btn" style="background-color:#8b0000;color:#fff;" @click="confirmExitEnroll">确认退出</button>
            </div>
          </div>

        </div>
      </div>
    </Transition>

    <!-- 弹窗：导出二维码给手机扫描 -->
    <Transition name="modal-fade">
      <div v-if="showQrTarget" class="modal-overlay">
        <div class="modal-dialog" style="width: 380px;">
          <h3 class="modal-title">📱 导出二维码</h3>
          <p class="cat-delete-text" style="text-align: left; margin-bottom: 8px;">
            用手机上的 Steam 令牌 App 扫描这个二维码即可导入 <strong>{{ showQrTarget.name }}</strong> 的密钥：
          </p>
          <div class="qr-image-box">
            <img v-if="qrExportUrl" :src="qrExportUrl" alt="otpauth 二维码" />
            <div v-else-if="qrExportError" class="field-hint error">{{ qrExportError }}</div>
            <div v-else class="field-hint">正在生成…</div>
          </div>
          <div v-if="showQrTarget" class="qr-link-row">
            <span class="field-hint">或手动复制链接：</span>
            <span class="qr-link mono" @click="copyText(buildOtpAuthUri(showQrTarget), 'otpauth 链接已复制')">{{ buildOtpAuthUri(showQrTarget) }}</span>
          </div>
          <p class="field-hint">⚠️ 二维码等价于密钥本身，别截图外传；扫完即关。</p>
          <div class="edit-actions">
            <button class="btn success" @click="showQrTarget = null">完成</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：删除分类 -->
    <Transition name="modal-fade">
      <div v-if="categoryToDelete" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { categoryToDelete = null })">
        <div class="modal-dialog" style="width: 360px; text-align: center;">
          <h3 class="modal-title" style="color: #FF4D4F; margin-bottom: 10px;">🗑️ 删除分类</h3>
          <p class="cat-delete-text">
            确定删除分类
            <strong>「{{ categoryToDelete }}」</strong> 吗？<br/>
            <template v-if="categoryUsage(categoryToDelete)">
              该分类下的 <strong>{{ categoryUsage(categoryToDelete) }}</strong> 条记录会被清空分类，<strong>记录本身不会删除</strong>。
            </template>
            <template v-else>该分类下暂无记录。</template>
          </p>
          <div class="actions" style="display: flex; justify-content: center; gap: 15px; margin-top: 20px;">
            <button @click="confirmDeleteCategory" class="btn"
                    style="background-color: #FF4D4F; color: white; border-color: #FF4D4F; min-width: 100px;">删除分类</button>
            <button @click="categoryToDelete = null" class="btn" style="min-width: 100px;">取消</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 弹窗：确认删除 -->
    <Transition name="modal-fade">
      <div v-if="deleteTargets && deleteTargets.length > 0" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { deleteTargets = null })">
        <div class="modal-dialog" style="width: 340px; text-align: center;">
          <h3 class="modal-title" style="color: #FF4D4F; margin-bottom: 10px;">⚠️ 确认删除</h3>
          <p style="margin: 20px 0; font-size: 14px; color: var(--text-secondary);">
            <template v-if="deleteTargets.length === 1">
              确定要删除 <br/><br/>
              <strong style="color: var(--text-primary); font-size: 18px;">{{ deleteTargets[0].name || deleteTargets[0].username }}</strong> <br/><br/>
              吗？
            </template>
            <template v-else>
              确定要删除选中的 <br/><br/>
              <strong style="color: var(--text-primary); font-size: 18px;">{{ deleteTargets.length }} 条记录</strong> <br/><br/>
              吗？
            </template>
          </p>
          <div class="actions" style="display: flex; justify-content: center; gap: 15px; margin-top: 25px;">
            <button @click="confirmDelete" class="btn" style="background-color: #FF4D4F; color: white; border-color: #FF4D4F; min-width: 100px;">确定删除</button>
            <button @click="deleteTargets = null" class="btn" style="min-width: 100px;">取消</button>
          </div>
        </div>
      </div>
    </Transition>

    <!-- 右键菜单 -->
    <Transition name="context-menu">
      <div v-if="contextMenu.show"
           class="context-menu" ref="contextMenuRef"
           :style="{ left: contextMenu.x + 'px', top: contextMenu.y + 'px' }"
           @click.stop>
        <div class="context-menu-item" @click="handleContextAction('copyUsername')">📋 复制账号</div>
        <div class="context-menu-item" @click="handleContextAction('copyPassword')">🔐 复制密码</div>
        <div class="context-menu-item" @click="handleContextAction('copyUrl')">🔗 复制网址 / 路径</div>
        <div class="context-menu-item" v-if="contextMenu.entry && hasTotp(contextMenu.entry)"
             @click="handleContextAction('copyTotp')">🔢 复制 2FA 动态码</div>
        <div class="context-menu-item" v-if="contextMenu.entry && recoveryCount(contextMenu.entry)"
             @click="handleContextAction('recovery')">📄 查看恢复代码 ({{ recoveryCount(contextMenu.entry) }})</div>
        <div class="context-menu-divider"></div>
        <div class="context-menu-item" @click="handleContextAction('open')">🚀 打开</div>
        <div class="context-menu-item" v-if="contextMenu.entry && entryType(contextMenu.entry) === 'steam'"
             @click="handleContextAction('steamLogin')">🎮 登录 Steam</div>
        <div class="context-menu-item" @click="handleContextAction('edit')">✏️ 编辑</div>
<div class="context-menu-item" v-if="contextMenu.entry && hasTotp(contextMenu.entry)" @click="closeContextMenu(); openQrCode(contextMenu.entry)">📱 导出二维码（给手机扫）</div>
        <div class="context-menu-divider"></div>
        <div class="context-menu-item danger" @click="handleContextAction('delete')">🗑️ 删除</div>
      </div>
    </Transition>

    <!-- 底部状态栏 -->
    <footer class="status-bar">
      <span>{{ statusText }}</span>
      <span class="right">
        {{ vaultState === 'unlocked' ? entries.length + ' 条记录 · 🔓 已解锁' : '🔒 已锁定' }} · v1.0
      </span>
    </footer>

    <!-- 设置菜单 -->
    <Transition name="context-menu">
      <div v-if="showSettings"
           class="context-menu settings-menu"
           :style="{ left: settingsMenuPos.x + 'px', top: settingsMenuPos.y + 'px' }"
           @click.stop>
        <div class="context-menu-item" @click="handleSettingsAction('theme')">
          {{ isDarkTheme ? '☀️ 浅色主题' : '🌙 深色主题' }}
        </div>
        <div class="context-menu-divider"></div>
        <div class="context-menu-item" @click="handleSettingsAction('changePassword')">🔑 修改主密码</div>
        <div class="context-menu-item" @click="handleSettingsAction(helloEnabled ? 'disableHello' : 'enableHello')">
          {{ helloBusy ? '⏳ 正在等待 Windows 验证…' : (helloEnabled ? '🪟 关闭 Windows Hello 解锁' : '🪟 启用 Windows Hello 解锁') }}
        </div>
        <div class="context-menu-item" @click="handleSettingsAction('lock')">🔒 立即锁定</div>
        <div class="context-menu-divider"></div>
        <div class="context-menu-item" @click="handleSettingsAction('about')">ℹ️ 关于</div>
      </div>
    </Transition>

    <!-- 弹窗：关于 -->
    <Transition name="modal-fade">
      <div v-if="showAbout" class="modal-overlay" @mousedown.self="overlayPressed = true" @mouseup.self="closeOverlay(() => { showAbout = false })">
        <div class="modal-dialog about-panel">
          <h3 class="modal-title">ℹ️ 关于</h3>
          <div class="about-content">
            <div class="about-logo">🔐</div>
            <div class="about-name">Account Manager</div>
            <div class="about-version">版本 1.0 · GPL-3.0</div>
            <div class="about-divider"></div>
            <div class="about-author">开发者: No-Zero-issue</div>
            <div class="about-desc">本地加密的网站 / 软件密码管理器<br/>PBKDF2-HMAC-SHA256 + AES-256-CBC + HMAC</div>
            <div class="about-link" @click="openExternal('https://github.com/No-Zero-issue/AccountManager')">
              🌐 GitHub: No-Zero-issue/AccountManager
            </div>
          </div>
          <div class="about-actions">
            <button @click="showAbout = false" class="btn">关闭</button>
          </div>
        </div>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
/* 表格行选择样式 */
.is-selected {
  background-color: var(--selected-bg) !important;
  outline: 1px solid var(--accent-primary);
  outline-offset: -1px;
}

/* 弹窗遮罩层 */
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  width: 100vw;
  height: 100vh;
  background-color: rgba(0, 0, 0, 0.7);
  display: flex;
  justify-content: center;
  align-items: center;
  z-index: 1000;
  backdrop-filter: blur(2px);
}

/* 弹窗主体 */
.modal-dialog {
  max-height: 88vh;
  overflow-y: auto;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: 8px;
  padding: 20px;
  width: 500px;
  max-width: 90vw;
  box-shadow: var(--shadow);
}

.modal-title {
  margin-top: 0;
  margin-bottom: 20px;
  font-size: 16px;
  font-weight: 600;
  color: var(--accent-primary);
}

.edit-panel {
  /* 移除之前的相对布局样式 */
}

.edit-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 15px;
  margin-bottom: 15px;
}

.input-group {
  display: flex;
  flex-direction: column;
}

.input-group label {
  font-size: 12px;
  color: var(--text-secondary);
  margin-bottom: 5px;
}

.input-group input, .input-group select {
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  padding: 8px;
  border-radius: 4px;
}

.input-group input:focus {
  border-color: var(--accent-primary);
  outline: none;
}

/* 强制穿透 Scoped Style 以隐藏所有浏览器/Edge 自带的图标按钮 */
:global(input::-ms-reveal),
:global(input::-ms-clear),
:global(input::-webkit-contacts-auto-fill-button),
:global(input::-webkit-credentials-auto-fill-button),
:global(input::-webkit-search-cancel-button),
:global(input::-webkit-search-results-button) {
  display: none !important;
  visibility: hidden !important;
  pointer-events: none !important;
  width: 0 !important;
  height: 0 !important;
}

.cooldown-setter-box {
  display: flex;
  gap: 5px;
}

.cooldown-setter-box input {
  flex: 1;
}

.edit-actions {
  display: flex;
  gap: 10px;
  justify-content: flex-end;
}

.app-container {
  display: flex;
  flex-direction: column;
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background-color: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
  overflow: hidden;
  box-sizing: border-box;
}

/* 自定义标题栏 */
.custom-titlebar {
  height: 32px;
  background-color: var(--bg-secondary);
  display: flex;
  justify-content: space-between;
  align-items: center;
  user-select: none;
  z-index: 9999;
  border-bottom: 1px solid var(--border-color);
  flex-shrink: 0;
}

.title-content {
  display: flex;
  align-items: center;
  padding-left: 12px;
  color: var(--text-muted);
  font-size: 12px;
  pointer-events: none;
}

.window-controls {
  display: flex;
  height: 100%;
}

.settings-btn {
  margin-left: 10px;
}

.settings-btn svg {
  width: 16px;
  height: 16px;
}

.win-btn {
  width: 46px;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  cursor: pointer;
  transition: background-color 0.1s;
}

.win-btn:hover {
  background-color: var(--hover-bg);
}

.win-btn.close:hover {
  background-color: #e81123;
}

.win-btn svg {
  width: 10px;
  height: 10px;
}

.layout-body {
  display: flex;
  flex: 1;
  overflow: hidden;
}

/* 侧边栏样式 */
.sidebar {
  width: 160px;
  background-color: var(--bg-secondary);
  border-right: 1px solid var(--border-color);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
  transition: width 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  overflow-x: hidden;
}

.sidebar.collapsed {
  width: 50px;
}

.sidebar-header {
  padding: 0 15px;
  font-weight: bold;
  font-size: 18px;
  color: var(--accent-primary);
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid var(--border-color);
  height: 44.5px;
  white-space: nowrap;
  overflow: hidden;
}

.sidebar.collapsed .sidebar-header {
  justify-content: center;
  padding: 0;
}

.toggle-sidebar-btn {
  background: transparent;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  padding: 5px;
  font-size: 14px;
  border-radius: 4px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}

.toggle-sidebar-btn:hover {
  background-color: var(--hover-bg);
  color: var(--text-primary);
}

.nav-menu {
  flex: 1;
  padding: 10px 0;
  position: relative;
}

.nav-indicator {
  position: absolute;
  top: 10px;
  left: 0;
  width: 3px;
  height: 40px;
  background-color: var(--accent-primary);
  border-radius: 0 2px 2px 0;
  transition: transform 0.2s ease;
  z-index: 1;
}

.nav-item {
  width: 160px;
  height: 40px;
  display: flex;
  align-items: center;
  background: transparent;
  border: none;
  color: var(--text-secondary);
  cursor: pointer;
  transition: background-color 0.2s, color 0.2s;
  font-size: 14px;
  padding: 0;
  text-align: left;
}

.nav-item:hover {
  background-color: var(--hover-bg);
  color: var(--text-primary);
}

.nav-item.active {
  background-color: var(--selected-bg);
  color: var(--text-primary);
}

.nav-item .icon {
  font-size: 16px;
  width: 50px;
  min-width: 50px;
  display: flex;
  justify-content: center;
  align-items: center;
  flex-shrink: 0;
}

.nav-item .label {
  white-space: nowrap;
  opacity: 1;
  transition: opacity 0.2s;
}

.sidebar.collapsed .nav-item .label {
  opacity: 0;
}

.sidebar-footer {
  padding: 10px;
  text-align: center;
  font-size: 10px;
  color: var(--text-muted);
  border-top: 1px solid var(--border-color);
}

/* 主内容区 */
.main-content {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.view-title {
  font-size: 14px;
  font-weight: 600;
  color: var(--text-secondary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 200px;
}

.spacer {
  flex: 1;
}

/* 工具栏 */
.toolbar {
  display: flex;
  gap: 10px;
  padding: 10px 15px;
  background-color: var(--bg-secondary);
  border-bottom: 1px solid var(--border-color);
}

/* 自定义标题栏 */
.custom-titlebar {
  height: 32px;
  background-color: var(--bg-secondary);
  display: flex;
  justify-content: space-between;
  align-items: center;
  user-select: none;
  z-index: 9999;
}

.title-content {
  display: flex;
  align-items: center;
  padding-left: 10px;
  color: var(--text-muted);
  font-size: 12px;
  pointer-events: none;
}

.window-controls {
  display: flex;
  height: 100%;
}

.win-btn {
  width: 46px;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  color: var(--text-primary);
  cursor: pointer;
  transition: background-color 0.2s;
}

.win-btn:hover {
  background-color: var(--hover-bg);
}

.win-btn.close:hover {
  background-color: #e81123;
}

.win-btn svg {
  width: 10px;
  height: 100%;
  fill: none;
  stroke: currentColor;
  stroke-width: 1;
}

.btn {
  background-color: var(--bg-tertiary);
  color: var(--text-primary);
  border: 1px solid var(--border-color);
  padding: 0 12px;
  cursor: pointer;
  font-size: 13px;
  border-radius: 3px;
  height: 24px;
  display: flex;
  align-items: center;
  white-space: nowrap;
  flex-shrink: 0;
}

.btn:hover {
  background-color: var(--hover-bg);
}

.btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
  pointer-events: none;
}

.btn.primary {
  border-color: var(--accent-primary);
}

.btn.success {
  background-color: var(--success-bg);
}

.btn.btn-blue {
  border-color: var(--accent-primary) !important;
  color: var(--accent-primary) !important;
}

.btn.btn-red {
  border-color: #ff5252 !important;
  color: #ff5252 !important;
}

/* API Key 区 */
.api-key-area {
  height: 34px;
  padding: 0 18px;
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 13px;
  background-color: var(--bg-secondary);
  border-bottom: 1px solid var(--border-color);
}

.input-wrapper {
  position: relative;
  display: flex;
  align-items: center;
}

.api-key-area .input-wrapper {
  width: 300px;
}

.api-key-area input {
  background-color: var(--bg-tertiary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  padding: 0 30px 0 8px;
  width: 100%;
  height: 24px;
}

.toggle-btn {
  position: absolute;
  right: 4px;
  background: none;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 2px;
  transition: color 0.2s;
}

.toggle-btn:hover {
  color: var(--text-secondary);
}

/* 列表中的密码：只读展示，点击即复制（修改请走编辑面板） */
.pwd-cell {
  display: flex;
  align-items: center;
  gap: 6px;
}

.pwd-text {
  cursor: pointer;
  font-size: 13px;
  letter-spacing: 1px;
  color: var(--text-primary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.pwd-text:hover {
  color: var(--accent-primary);
  text-decoration: underline;
}

.pwd-text.is-revealed {
  letter-spacing: normal;
  font-size: 12px;
}

.api-key-area a {
  color: var(--accent-primary);
  text-decoration: none;
}

.modal-fade-enter-active,
.modal-fade-leave-active {
  transition: opacity 0.2s ease;
}

.modal-fade-enter-from,
.modal-fade-leave-to {
  opacity: 0;
}

.modal-fade-enter-active .modal-dialog,
.modal-fade-leave-active .modal-dialog {
  transition: transform 0.2s ease;
}

.modal-fade-enter-from .modal-dialog,
.modal-fade-leave-to .modal-dialog {
  transform: scale(0.95);
}

.context-menu {
  position: fixed;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: 6px;
  padding: 5px 0;
  min-width: 160px;
  box-shadow: var(--shadow);
  z-index: 10000;
}

.context-menu-item {
  white-space: nowrap;
  padding: 8px;
  cursor: pointer;
  font-size: 13px;
  color: var(--text-secondary);
  display: flex;
  align-items: center;
  gap: 8px;
}

.context-menu-item:hover {
  padding: 8px 8px;
  background-color: var(--accent-dark);
  color: var(--text-primary);
}

.context-menu-item.danger:hover {
  background-color: #8b0000;
}

.context-menu-divider {
  height: 1px;
  background-color: var(--border-color);
  margin: 5px 0;
}

.settings-menu {
  min-width: 180px;
}

.context-menu-enter-active,
.context-menu-leave-active {
  transition: opacity 0.15s ease, transform 0.15s ease;
}

.context-menu-enter-from,
.context-menu-leave-to {
  opacity: 0;
  transform: scale(0.95);
}

/* 表格样式 */
.table-container {
  flex: 1;
  overflow: auto;
}

.data-grid {
  width: 100%;
  /* 窗口变窄时不再压缩列宽（否则「网址 / 程序」会被压成 0 宽、操作列按钮被切掉），
     改为让表格横向滚动，操作列固定在右侧 */
  min-width: 860px;
  border-collapse: collapse;
  font-size: 13px;
  table-layout: fixed;
}

.data-grid th, .data-grid td {
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  padding: 0 8px;
}

.data-grid th {
  background-color: var(--bg-tertiary);
  text-align: left;
  height: 34px;
  border: 1px solid var(--border-color);
  position: sticky;
  top: 0;
}

.data-grid td {
  height: 34px;
  border-bottom: 1px solid var(--border-color);
  vertical-align: middle;
}

.edit-input {
  background: transparent;
  border: 1px solid transparent;
  color: var(--text-secondary);
  padding: 2px 5px;
  width: 100%;
  box-sizing: border-box;
  font-size: 13px;
  height: 26px;
}

.edit-input:hover, .edit-input:focus {
  background: var(--bg-primary);
  border-color: var(--border-color);
  color: var(--text-primary);
  outline: none;
}

.data-grid tr:hover {
  background-color: var(--hover-bg);
}

.link-text { cursor: pointer; }

.link-cell {
  color: var(--accent-primary);
  cursor: pointer;
}

.link-text:hover { text-decoration: underline; }

.row-actions {
  display: flex;
  gap: 5px;
  align-items: center;
  justify-content: center;
  height: 34px;
}

.btn-sm {
  padding: 0 8px;
  height: 24px;
  font-size: 11px;
  border: none;
  border-radius: 2px;
  cursor: pointer;
  /* 必须显式给背景色：否则会落到浏览器默认按钮底色，与主题文字色冲突 */
  background-color: var(--bg-tertiary);
  color: var(--text-primary);
  display: flex;
  align-items: center;
  justify-content: center;
}

.btn-sm.login {
  background-color: var(--accent-primary);
  color: #ffffff;
}

.btn-sm.totp {
  background-color: #7c3aed;
  color: #ffffff;
}

.btn-sm.steam {
  background-color: #2a475e;
  color: #ffffff;
}

.btn-sm.delete {
  background-color: #8b0000;
  color: #ffffff;
}

.btn-sm:hover { filter: brightness(1.2); }

/* 状态栏 */
.status-bar {
  background-color: var(--accent-primary);
  color: var(--text-primary);
  padding: 2px 10px;
  display: flex;
  justify-content: space-between;
  font-size: 12px;
}

/* 自定义滚动条 */
.table-container::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

.table-container::-webkit-scrollbar-thumb {
  background: var(--border-color);
}

.table-container::-webkit-scrollbar-track {
  background: var(--bg-primary);
}

.modal-dialog::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

.modal-dialog::-webkit-scrollbar-thumb {
  background: var(--border-color);
  border-radius: 4px;
}

.modal-dialog::-webkit-scrollbar-track {
  background: var(--bg-primary);
}

/* 全局滚动条：统一主题（含横向 height），避免其它容器出现系统默认浅色滚动条 */
::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

::-webkit-scrollbar-thumb {
  background: var(--border-color);
  border-radius: 4px;
}

::-webkit-scrollbar-thumb:hover {
  background: var(--text-secondary);
}

::-webkit-scrollbar-track {
  background: var(--bg-primary);
}

::-webkit-scrollbar-corner {
  background: var(--bg-primary);
}

/* 选中单个条目时的底部悬浮操作栏 */
.floating-bar {
  position: fixed;
  left: 50%;
  bottom: 28px;
  transform: translateX(-50%);
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  border: 1px solid var(--border-color);
  border-radius: 999px;
  background-color: var(--bg-secondary);
  box-shadow: 0 6px 24px rgba(0, 0, 0, 0.35);
  z-index: 60;
}

.floating-name {
  max-width: 180px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 13px;
  color: var(--text-primary);
  margin-right: 4px;
}

.floating-items {
  display: flex;
  align-items: center;
  gap: 8px;
}

/* 悬浮栏按钮：不换行、不被压缩（否则「复制密码」会被拆成两行） */
.floating-bar .btn-sm {
  white-space: nowrap;
  flex-shrink: 0;
  line-height: 1.2;
}

.floating-bar {
  flex-wrap: nowrap;
}

/* 按钮增减时的进出动画（切换选中条目时会变） */
.bar-btn-enter-active,
.bar-btn-leave-active {
  transition: opacity 0.18s ease, transform 0.18s ease;
}

.bar-btn-enter-from,
.bar-btn-leave-to {
  opacity: 0;
  transform: translateY(6px) scale(0.92);
}

/* 二维码导出 */
.qr-image-box {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 300px;
  padding: 10px;
  background-color: #ffffff;
  border-radius: 8px;
}

.qr-link-row {
  display: flex;
  align-items: center;
  gap: 6px;
  margin: 6px 0 2px;
}

.qr-link {
  flex: 1;
  font-size: 11px;
  color: var(--text-secondary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  cursor: pointer;
}

.qr-link:hover { text-decoration: underline; }

.qr-image-box img {
  width: 280px;
  height: 280px;
}

.drag-cell { width: 30px; padding: 0 4px; }

.drag-handle {
  display: inline-block;
  cursor: grab;
  color: var(--text-muted);
  font-size: 15px;
  user-select: none;
}

.drag-handle:active { cursor: grabbing; }

/* 拖动排序反馈（鼠标事件实现，不依赖 HTML5 拖放）：
   被拖的行留半透明残影，克隆出的整行跟着鼠标浮动，目标位置用虚线空槽实时预览 */
.data-grid tbody tr.row-dragging {
  opacity: 0.3;
}

.data-grid tbody tr.drag-gap-row {
  pointer-events: none;
}

.data-grid tbody tr.drag-gap-row > td {
  padding: 0;
  border: none;
  background: transparent;
}

/* 空槽用一个有固定高度的块来撑，保证它在表格里真的占一行的高度 */
.data-grid tbody tr.drag-gap-row .drag-gap-box {
  height: 30px;
  margin: 2px 0;
  border: 1px dashed var(--accent-primary);
  border-radius: 8px;
  background: rgba(78, 161, 255, 0.08);
}

/* 跟手浮层：克隆的整行 */
.drag-clone {
  position: fixed;
  left: 0;
  top: 0;
  z-index: 9999;
  pointer-events: none;
  opacity: 0.96;
  border-radius: 8px;
  overflow: hidden;
  box-shadow: 0 12px 30px rgba(0, 0, 0, 0.5);
}

.drag-clone table.data-grid {
  border-collapse: separate;
  border-spacing: 0;
}

body.row-dragging-active {
  user-select: none;
  cursor: grabbing;
}

body.row-dragging-active * {
  cursor: grabbing !important;
}

/* 浅色主题 */
.light-theme {
  --bg-primary: #ffffff;
  --bg-secondary: #f3f3f3;
  --bg-tertiary: #e8e8e8;
  --accent-primary: #0078d4;
  --accent-secondary: #106ebe;
  --accent-dark: #005a9e;
  --text-primary: #1e1e1e;
  --text-secondary: #333333;
  --text-muted: #666666;
  --border-color: #d4d4d4;
  --hover-bg: #e1e1e1;
  --selected-bg: #cce8f4;
  --success-bg: #a8d8a8;
  --danger-bg: #f0a8a8;
  --shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}

.light-theme {
  background-color: var(--bg-primary) !important;
  color: var(--text-primary) !important;
}

.light-theme .custom-titlebar {
  background-color: var(--bg-secondary) !important;
  border-bottom-color: var(--border-color) !important;
}

.light-theme .title-content {
  color: var(--text-muted) !important;
}

.light-theme .app-title {
  color: var(--text-primary) !important;
}

.light-theme .sidebar {
  background-color: var(--bg-secondary) !important;
  border-right-color: var(--border-color) !important;
}

.light-theme .sidebar-header {
  color: var(--accent-primary) !important;
  border-bottom-color: var(--border-color) !important;
}

.light-theme .nav-item {
  color: var(--text-secondary) !important;
}

.light-theme .nav-item:hover {
  background-color: var(--hover-bg) !important;
  color: var(--text-primary) !important;
}

.light-theme .nav-item.active {
  background-color: var(--selected-bg) !important;
  color: var(--text-primary) !important;
}

.light-theme .toolbar {
  background-color: var(--bg-secondary) !important;
  border-bottom-color: var(--border-color) !important;
}

.light-theme .view-title {
  color: var(--text-primary) !important;
}

.light-theme .data-grid th {
  background-color: var(--bg-tertiary) !important;
  color: var(--text-primary) !important;
  border-color: var(--border-color) !important;
}

.light-theme .data-grid td {
  background-color: var(--bg-primary) !important;
  color: var(--text-primary) !important;
  border-bottom-color: var(--border-color) !important;
}

.light-theme .data-grid tr:hover td {
  background-color: var(--hover-bg) !important;
}

.light-theme .edit-input {
  background: transparent !important;
  color: var(--text-primary) !important;
}

.light-theme .edit-input:hover,
.light-theme .edit-input:focus {
  background: var(--bg-primary) !important;
  border-color: var(--accent-primary) !important;
  color: var(--text-primary) !important;
}

.light-theme .status-bar {
  background-color: var(--accent-primary) !important;
  color: var(--bg-primary) !important;
}

.light-theme .win-btn {
  color: var(--text-primary) !important;
}

.light-theme .win-btn svg {
  stroke: var(--text-primary) !important;
}

.light-theme .modal-overlay {
  background-color: rgba(0, 0, 0, 0.4) !important;
}

.light-theme .modal-dialog {
  background-color: var(--bg-primary) !important;
  border-color: var(--border-color) !important;
  color: var(--text-primary) !important;
}

.light-theme .modal-title {
  color: var(--accent-primary) !important;
}

.light-theme .input-group label {
  color: var(--text-secondary) !important;
}

.light-theme .input-group input,
.light-theme .input-group select {
  background-color: var(--bg-primary) !important;
  border-color: var(--border-color) !important;
  color: var(--text-primary) !important;
}

.light-theme .input-group input:focus {
  border-color: var(--accent-primary) !important;
}

.light-theme .context-menu {
  background-color: var(--bg-primary) !important;
  border-color: var(--border-color) !important;
  color: var(--text-primary) !important;
}

.light-theme .context-menu-item {
  color: var(--text-primary) !important;
}

.light-theme .context-menu-item:hover {
  background-color: var(--accent-secondary) !important;
  color: var(--bg-primary) !important;
}

.light-theme .btn {
  background-color: var(--bg-tertiary) !important;
  color: var(--text-primary) !important;
  border-color: var(--border-color) !important;
}

.light-theme .btn:hover {
  background-color: var(--hover-bg) !important;
}

.light-theme .btn.btn-blue {
  border-color: #0078d4 !important;
  color: #0078d4 !important;
}

.light-theme .btn.btn-red {
  border-color: #d32f2f !important;
  color: #d32f2f !important;
}

.light-theme .settings-menu {
  background-color: var(--bg-primary) !important;
  border-color: var(--border-color) !important;
}

.light-theme .is-selected {
  background-color: var(--selected-bg) !important;
  outline-color: var(--accent-primary) !important;
}

.about-panel {
  width: 360px;
  text-align: center;
}

.about-content {
  padding: 20px 0;
}

.about-logo {
  font-size: 48px;
  margin-bottom: 15px;
}

.about-name {
  font-size: 20px;
  font-weight: 600;
  color: var(--accent-primary);
  margin-bottom: 8px;
}

.about-version {
  font-size: 13px;
  color: var(--text-muted);
  margin-bottom: 15px;
}

.about-divider {
  height: 1px;
  background-color: var(--border-color);
  margin: 15px 0;
}

.about-author {
  font-size: 14px;
  color: var(--text-secondary);
  margin-bottom: 8px;
}

.about-desc {
  font-size: 13px;
  color: var(--text-muted);
  margin-bottom: 15px;
}

.about-link {
  font-size: 13px;
  color: var(--accent-primary);
  cursor: pointer;
  padding: 8px;
  border-radius: 4px;
  transition: background-color 0.2s;
}

.about-link:hover {
  background-color: var(--hover-bg);
}

.about-actions {
  margin-top: 20px;
}

/* ===================== 保险库解锁 / 初始化界面 ===================== */
.vault-screen {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  overflow: auto;
}

.vault-card {
  width: 430px;
  max-width: 92vw;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: 12px;
  padding: 28px;
  box-shadow: var(--shadow);
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.vault-icon {
  font-size: 40px;
  text-align: center;
  line-height: 1;
}

.vault-title {
  margin: 0;
  font-size: 18px;
  text-align: center;
  color: var(--text-primary);
}

.vault-hint {
  margin: 0;
  font-size: 12px;
  line-height: 1.7;
  color: var(--text-secondary);
}

.vault-hint code {
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  padding: 1px 5px;
  border-radius: 3px;
  font-family: Consolas, 'Courier New', monospace;
}

.vault-field {
  position: relative;
  display: flex;
  align-items: center;
}

.vault-field input {
  width: 100%;
  padding: 10px 38px 10px 12px;
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  border-radius: 6px;
  font-size: 14px;
}

.vault-field input:focus {
  border-color: var(--accent-primary);
  outline: none;
}

.vault-field .toggle-btn {
  position: absolute;
  right: 8px;
}

.vault-error {
  font-size: 12px;
  color: #FF4D4F;
}

.vault-submit {
  width: 100%;
  justify-content: center;
}

/* Windows Hello 解锁按钮与分隔线 */
.vault-divider {
  display: flex;
  align-items: center;
  gap: 10px;
  color: var(--text-muted);
  font-size: 11px;
}

.vault-divider::before,
.vault-divider::after {
  content: '';
  flex: 1;
  height: 1px;
  background-color: var(--border-color);
}

.hello-btn {
  background-color: var(--bg-tertiary);
  border: 1px solid var(--accent-primary);
  color: var(--text-primary);
}

.hello-btn:hover:not(:disabled) {
  background-color: var(--hover-bg);
}

.hello-btn:disabled {
  opacity: 0.6;
  cursor: default;
}

/* ===================== 标题栏状态与工具栏搜索框 ===================== */
.title-vault-state {
  margin-left: 12px;
  font-size: 11px;
  color: var(--text-muted);
}

.search-box {
  display: flex;
  align-items: center;
  gap: 6px;
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  border-radius: 6px;
  padding: 0 8px;
  height: 28px;
  margin-right: 8px;
}

.search-box input {
  background: transparent;
  border: none;
  outline: none;
  color: var(--text-primary);
  width: 210px;
  font-size: 12px;
}

.search-box .clear-search {
  background: transparent;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  font-size: 12px;
}

.search-box .clear-search:hover {
  color: var(--text-primary);
}

/* ===================== 列表中的类型标记与分类标签 ===================== */
.type-icon {
  margin-right: 6px;
}

.entry-name {
  font-weight: 500;
}

.tag {
  display: inline-block;
  padding: 2px 8px;
  font-size: 11px;
  border-radius: 10px;
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-secondary);
  white-space: nowrap;
}

.mono {
  font-family: Consolas, 'Courier New', monospace;
}

/* ===================== 2FA 动态码 ===================== */
.totp-cell {
  display: flex;
  align-items: center;
  gap: 8px;
}

.totp-code {
  font-size: 15px;
  letter-spacing: 1px;
  color: var(--accent-primary);
  cursor: pointer;
  font-weight: 600;
}

.totp-code:hover {
  text-decoration: underline;
}

.totp-remaining {
  font-size: 11px;
  color: var(--text-muted);
}

.totp-error {
  font-size: 11px;
  color: #FF4D4F;
}

.muted-cell {
  color: var(--text-muted);
}

/* ===================== Steam 一键登录 ===================== */
.steam-login-steps {
  margin: 6px 0 10px 20px;
  font-size: 12px;
  line-height: 1.9;
  color: var(--text-secondary);
  text-align: left;
}

.steam-login-steps code {
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  padding: 0 4px;
  border-radius: 3px;
  font-family: Consolas, 'Courier New', monospace;
}

/* Steam Guard 码展示（登录确认弹窗内） */
.steam-guard-box {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
  margin: 6px 0 10px;
  padding: 8px 10px;
  border: 1px dashed var(--accent-primary);
  border-radius: 6px;
  background-color: var(--bg-primary);
}

.steam-guard-label {
  font-size: 12px;
  color: var(--text-secondary);
}

.steam-guard-code {
  font-size: 18px;
  font-weight: 600;
  letter-spacing: 3px;
  color: var(--accent-primary);
}

/* 应用内添加令牌入口 */
.enroll-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px dashed var(--border-color);
}

/* 撤销码展示 */
.revocation-box {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 6px;
  margin: 8px 0;
  padding: 10px;
  border: 1px dashed var(--accent-primary);
  border-radius: 6px;
  background-color: var(--bg-primary);
}

.revocation-label {
  font-size: 12px;
  color: var(--text-secondary);
}

.revocation-value {
  font-size: 16px;
  font-weight: 600;
  letter-spacing: 1px;
  color: var(--accent-primary);
  user-select: all;
}

.algo-select {
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  border-radius: 4px;
  padding: 4px 6px;
  font-size: 12px;
  flex-shrink: 0;
}

.field-hint {
  margin-top: 4px;
  font-size: 11px;
  color: var(--text-muted);
}

.field-hint.error {
  color: #FF4D4F;
}

.field-hint a {
  color: var(--accent-primary);
  text-decoration: none;
}

/* ===================== 二维码导入 ===================== */
.qr-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.qr-row input[type="text"] {
  flex: 1;
  min-width: 0;
}

.qr-row .btn {
  flex-shrink: 0;
  white-space: nowrap;
}

.hidden-file {
  display: none;
}

/* ===================== 2FA 恢复代码弹窗 ===================== */
.recovery-panel {
  width: 520px;
}

.recovery-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
}

.recovery-name {
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
}

.recovery-sub {
  margin-top: 4px;
  font-size: 12px;
  color: var(--text-secondary);
}

.recovery-list {
  max-height: 320px;
  overflow-y: auto;
  border: 1px solid var(--border-color);
  border-radius: 6px;
  background-color: var(--bg-primary);
  padding: 6px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.recovery-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 8px;
  border-radius: 4px;
}

.recovery-item:hover {
  background-color: var(--hover-bg);
}

.recovery-item.is-used .recovery-code {
  text-decoration: line-through;
  color: var(--text-muted);
}

.recovery-check {
  display: flex;
  align-items: center;
  cursor: pointer;
}

.recovery-code {
  flex: 1;
  font-size: 13px;
  color: var(--text-primary);
  word-break: break-all;
}

.recovery-empty {
  padding: 20px;
  text-align: center;
  font-size: 12px;
  color: var(--text-muted);
}

.nav-section-title {
  padding: 10px 15px 4px;
  font-size: 11px;
  color: var(--text-muted);
}

/* ===================== 分类管理（侧边栏内增删改） ===================== */
.nav-section-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding-right: 6px;
}

.nav-section-head .nav-section-title {
  padding: 10px 0 4px 15px;
}

.nav-cat-row {
  display: flex;
  align-items: center;
}

.nav-cat-row .nav-cat-item {
  flex: 1;
  min-width: 0;
}

.nav-cat-count {
  margin-left: auto;
  padding: 0 5px;
  font-size: 10px;
  line-height: 14px;
  border-radius: 8px;
  background-color: var(--bg-tertiary);
  color: var(--text-muted);
}

.nav-cat-actions {
  display: flex;
  align-items: center;
  opacity: 0;
  transition: opacity 0.15s ease;
}

.nav-cat-row:hover .nav-cat-actions {
  opacity: 1;
}

.nav-cat-btn {
  background: transparent;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  font-size: 12px;
  line-height: 1;
  padding: 4px 5px;
  border-radius: 3px;
}

.nav-cat-btn:hover {
  background-color: var(--hover-bg);
  color: var(--text-primary);
}

.nav-cat-btn.ok {
  color: var(--accent-primary);
}

.nav-cat-btn.danger:hover {
  color: #FF4D4F;
}

.nav-cat-edit {
  display: flex;
  align-items: center;
  gap: 2px;
  flex: 1;
  min-width: 0;
  padding: 3px 6px 3px 12px;
}

.nav-cat-edit input {
  flex: 1;
  min-width: 0;
  background-color: var(--bg-primary);
  border: 1px solid var(--accent-primary);
  color: var(--text-primary);
  border-radius: 3px;
  padding: 3px 5px;
  font-size: 12px;
  outline: none;
}

.nav-cat-empty {
  padding: 6px 15px;
  font-size: 11px;
  color: var(--text-muted);
}

.cat-delete-text {
  margin: 12px 0;
  font-size: 13px;
  line-height: 1.9;
  color: var(--text-secondary);
}

/* ===================== 密码生成器 ===================== */
.generator-box {
  grid-column: span 2;
  border: 1px dashed var(--border-color);
  border-radius: 6px;
  padding: 12px;
  display: flex;
  flex-direction: column;
  gap: 10px;
  background-color: var(--bg-primary);
}

.generator-row {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 12px;
  color: var(--text-secondary);
}

.generator-row input[type="range"] {
  flex: 1;
}

.generator-row.options {
  gap: 16px;
  flex-wrap: wrap;
}

.generator-row.options label {
  display: flex;
  align-items: center;
  gap: 4px;
  cursor: pointer;
}

.generator-preview {
  font-size: 13px;
  padding: 8px;
  background-color: var(--bg-secondary);
  border-radius: 4px;
  word-break: break-all;
  color: var(--accent-primary);
  min-height: 18px;
}

.generator-actions {
  display: flex;
  gap: 10px;
}

/* 编辑面板中的多行备注 */
.edit-grid textarea {
  background-color: var(--bg-primary);
  border: 1px solid var(--border-color);
  color: var(--text-primary);
  padding: 8px;
  border-radius: 4px;
  resize: vertical;
  font-family: inherit;
}

.edit-grid textarea:focus {
  border-color: var(--accent-primary);
  outline: none;
}
</style>

