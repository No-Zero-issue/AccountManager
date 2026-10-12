# Account Manager

## 简介
Account Manager 是一款**本地优先的网站 / 软件密码管理器**。核心与加密逻辑由 C++（Win32 + WebView2）实现，界面使用 Vue + Vite 编写，并以单文件形式嵌入 EXE，运行时释放加载，无需外部依赖。

## 预览图
<p align="center">
  <img src="Image/phone.png" alt="手机端" width="220">
  &nbsp;&nbsp;&nbsp;
  <img src="Image/pc.png" alt="桌面端" width="520">
</p>

## 功能
- 条目类型：🌐 网站、💻 软件、🎮 Steam 账号、📄 恢复代码
- 条目字段：名称、类型、网址或程序路径、账号、密码、SteamID64（Steam）、TOTP 密钥、2FA 恢复代码、分类、备注
- **Steam 账号专栏**：侧边栏独立入口，一键打开个人主页、**一键登录**
- **Steam Guard 动态码**：Steam 专用的 5 位验证码（不是标准 TOTP），支持导入 SDA 的 `.maFile`、Base64 `shared_secret`、`otpauth://` 链接或二维码；一键登录时会直接显示当前验证码
- 搜索（名称 / 账号 / 网址 / SteamID64 / 备注）、按类型与分类筛选、多选批量删除
- **拖动排序**：表格最左侧手柄可上下拖动调整条目顺序，顺序随保险库一起加密保存 —— 拖动时整行跟着鼠标浮动、被拖行留半透明残影、目标位置用虚线空槽实时预览，其它行**实时让位**（鼠标跨过某行中线时空槽才换位，自带迟滞不会抖），松手才写盘（见上方「预览图」里的拖动示例）
- **分类管理**：在侧边栏直接新建 / 重命名（同步更新该分类下所有记录）/ 删除分类；分类列表与条目一起加密保存（可以存在暂时没有条目使用的空分类）
- 密码显示 / 隐藏、一键复制（直接写入系统剪贴板）、随机密码生成器（可配置长度与字符集）
- **TOTP 动态验证码**：填入 `otpauth://` 链接或 Base32 密钥，列表实时显示 6 / 8 位动态码（带剩余秒数，点一下即复制）；支持 SHA-1 / SHA-256 / SHA-512 与自定义位数、周期
- **二维码导入**：编辑面板里点「📷 二维码图片」选图、把二维码截图拖进去，或直接 `Ctrl+V` 粘贴截图，自动识别出 `otpauth://` 链接并填好 TOTP 密钥（顺带用二维码里的 issuer 补全名称）
- **同步到手机（2FA）**：设置菜单里「📲 同步到手机（2FA）」把**全部**带 2FA 密钥的条目打包成二维码（条目多时按字节预算自动分成多张，逐张扫），手机端「从电脑同步」收齐后按密钥去重合并；纯离线，手机端不需要联网。详见「[同步 2FA 到手机](#同步-2fa-到手机)」
- **2FA 恢复代码**：任何条目都可以逐行保存备用代码，弹窗内可逐条复制、勾选「已使用」并统计剩余数量；也可以建独立的「📄 恢复代码」条目集中管理某服务的备用码
- 网站条目一键打开网址，软件条目一键启动对应程序
- **Windows Hello 解锁**（可选）：用 PIN / 指纹 / 人脸通过 Windows 安全中心验证后解锁，无需输入主密码，见下文
- 深色 / 浅色主题、无边框自定义标题栏、窗口边缘缩放、快捷键（`Ctrl+F` 搜索、`Ctrl+N` 新增、`Ctrl+L` 锁定）

## 数据与加密
- 数据文件：程序目录下的**单个 `AccountManager.json`**，分 `vault` / `hello` / `config` 三节，其中 `vault` 节整体加密
- 密钥派生：PBKDF2-HMAC-SHA256，200000 次迭代，16 字节随机盐
- 加密算法：AES-256-CBC（PKCS7 填充），每次保存使用新的随机 IV
- 完整性校验：HMAC-SHA256（先加密后校验），主密码错误或文件被篡改都会被拒绝解密
- 主密码不会被保存；派生密钥仅在解锁期间驻留内存，锁定或退出立即清零
- 首次运行需设置主密码（至少 6 位），之后每次启动都需要输入主密码解锁
- 界面设置（主题、侧边栏）在 `config` 节里明文保存，其中不含任何敏感信息
- TOTP 密钥、Steam `shared_secret`、2FA 恢复代码都属于 `vault` 节，同样被整体加密，不会以明文落盘
- 写盘一律「读整份 → 只替换自己那一节 → 原子写回（先写 `.tmp` 再替换）」，避免各模块互相覆盖
- ⚠️ 忘记主密码将无法恢复数据，请自行备份 `AccountManager.json` 并牢记主密码

## Steam 一键登录
只有在你点击「🚀 登录」并在确认弹窗里确认后，才会执行以下系统操作（在此之前不会碰任何系统设置）：

1. 结束当前正在运行的 `steam.exe`（Steam 同时只允许一个实例，切换账号必须先关掉）；
2. 写入注册表 `HKCU\Software\Valve\Steam`：`AutoLoginUser` = 账号名、`RememberPassword` = 1；
3. 以 `-no-browser -skipinitialbootstrap -nobootstrapupdate -noreactlogin -login 账号 密码` 参数启动 Steam。

⚠️ 已知取舍：Steam 客户端只支持用命令行参数传入凭据，因此**密码在登录瞬间会出现在该进程的命令行里**，同机其他进程可能读到；同时账号名会落到注册表。如果不接受，就不要使用一键登录，改为手动在 Steam 里登录。

## Steam Guard 动态码
Steam 的两步验证**不是标准 TOTP**：它用 Base64 的 `shared_secret`（不是 Base32），把动态截断的结果按自定义 26 字母表 `23456789BCDFGHJKMNPQRTVWXY` 编成 **5 位**码。标准验证器 App 从上表那个链接算出的是 6 位数字，**Steam 不认**，必须用 Steam 的自定义编码。

本项目的实现与 SteamKit 的 `GenerateSteamGuardCode` / go-steam-totp 一致，并**与 node-steam-totp 做了 200 组随机密钥 + 随机时间的差分比对，结果完全一致**。

导入方式（编辑 Steam 条目 →「TOTP 密钥」栏，可选文件、拖拽、粘贴截图，或直接在输入框粘贴后失焦自动识别）：

| 来源 | 说明 |
|---|---|
| Steam Desktop Authenticator 的 `.maFile` | 自动取出 `shared_secret`，并顺带填 `revocation_code` 与账号名 |
| Base64 `shared_secret` | 例如 `cnOgv/KdpLoP6Nbh0GMkXkPXALQ=` |
| `otpauth://totp/Steam:xxx?secret=<Base32>` | Steam 设置流程里给出的链接，Base32 会自动转成内部使用的 Base64 |
| 二维码图片 | 上面那个链接的二维码，截图后拖入 / 选择 / `Ctrl+V` |

其它说明：
- 密钥和密码一样存在**加密保险库**里，不会明文落盘；`identity_secret` 目前**只解析、不参与任何功能**（本项目不做交易 / 市场确认）。
- 一键登录的确认弹窗会直接显示当前 5 位码，方便 Steam 弹出验证时手输。
- 如果码总是提示错误，多半是**系统时间偏差超过 30 秒**，先校准系统时间。

## Windows Hello 解锁
在保险库解锁状态下，通过「设置 → 启用 Windows Hello 解锁」开启；开启后锁屏界面会多出一个「使用 Windows Hello 解锁」按钮。

实现方式（不是简单地跳过密码）：
1. 通过 `KeyCredentialManager` 创建一对 **TPM 保护的 Hello 密钥**，私钥只有在 PIN / 指纹 / 人脸验证通过后才能使用；
2. 对固定挑战签名，签名经 **HKDF-SHA256** 派生出包装密钥；
3. 用包装密钥加密真正的保险库密钥（AES 密钥 + HMAC 密钥 + 盐 + 迭代次数），结果写进程序目录 `AccountManager.json` 的 `hello` 节（同时留一份 `hello.json` 作为兼容缓存）。

因此：没有通过 Windows 安全中心验证就拿不到保险库密钥；`hello` 数据与本机、本用户、Hello 凭据绑定。修改主密码后会重新绑定一次 Hello 数据（会再弹一次验证）；如果重新绑定被取消，Hello 解锁会自动关闭，此时仍可用主密码解锁。

注意事项：
- 需要设备支持 Windows Hello 且已配置 PIN / 指纹 / 人脸，否则启用时会提示不可用；
- 如果需要更高的安全强度，可在「设置 → 关闭 Windows Hello 解锁」删除 `hello` 数据与对应凭据，改为每次输入主密码。

## 代码结构

C++ 侧原来是一个 3700 行的 `AccountManager.cpp`，现按功能拆到 `AccountManager/Function/`：

```
AccountManager/
├─ AccountManager.cpp      程序入口 / 窗口过程 / WebView2 宿主 / 前端消息分发（约 920 行）
├─ version.h               程序版本的唯一来源：VERSIONINFO / 关于框 / User-Agent / 界面版本号
└─ Function/
   ├─ Function.h           内部共享头：公共 include、宏、跨文件常量/结构体/全局量/函数声明
   ├─ Utils.cpp            字符编码 / Base64 / Hex / 随机数
   ├─ Crypto.cpp           CNG：PBKDF2 / AES-256-CBC / HMAC-SHA256 / HKDF / RSA
   ├─ DataStore.cpp        配置与单文件 AccountManager.json 的分区读写、旧文件迁移
   ├─ Vault.cpp            密码保险库：密钥驻留 + 信封 + 创建/解锁/保存/改密
   ├─ Hello.cpp            Windows Hello 解锁
   ├─ Proto.cpp            极简 protobuf（namespace steampb）
   ├─ Http.cpp             WinHTTP 传输层
   ├─ Vdf.cpp              VDF 解析 + 从本机 Steam 导入账号
   ├─ SteamClient.cpp      Steam 客户端一键登录（注册表 + 进程）
   └─ SteamAuth.cpp        Steam 账号登录 + 应用内手机令牌
```

拆分只搬代码、不改行为：`Function.h` 里放跨文件的那点东西（`kVault*` 常量、`SteamHttpResult`、`SteamEnrollSession`、两个跨线程标志、`RunBackgroundWorker` 模板、各模块函数声明）。

## 构建
- 需要 Visual Studio（v143 工具集，含 C++ 桌面开发负载）、Node.js，以及仓库内 `packages` 目录的 WebView2 / WIL NuGet 包
- MSBuild 会在编译前自动执行 `npm install` 与 `npm run build`，并把 `frontend/dist` 同步到输出目录，同时作为 HTML 资源打进 EXE
- 加密基于 Windows CNG（`bcrypt.lib`：`BCryptDeriveKeyPBKDF2` / `BCryptEncrypt` / `BCryptGenRandom`），无第三方加密库依赖
- Windows Hello 使用 C++/WinRT（`Windows.Security.Credentials`，链接 `windowsapp.lib`）
- 二维码解码使用 [jsQR](https://github.com/cozmo/jsQR)（Apache-2.0，纯 JS，随前端一起打进 EXE）

命令行构建（Release）：

```powershell
npm.cmd --prefix frontend run build                      # 前端产出 frontend/dist/index.html
(Get-Item AccountManager\AccountManager.rc).LastWriteTime = Get-Date   # 见下方提示
msbuild AccountManager.sln /p:Configuration=Release /p:Platform=x64
#   → x64\Release\AccountManager.exe
```

- ⚠️ **改过 `frontend/src` 之后必须触碰 `AccountManager.rc`**（或清理中间目录）再编译：MSBuild 的增量构建不会因为 `..\frontend\dist\index.html` 变化而重编资源，否则新界面进不了 EXE。
- 若 MSBuild 报 `MSB6001 ... CL.exe 的命令行开关无效 / 字典中的关键字"no_proxy"`，是当前 shell 里同时存在 `no_proxy` 与 `NO_PROXY` 这类大小写重名变量，先在同一个 shell 里清掉再编译：
  ```powershell
  foreach ($n in 'http_proxy','HTTP_PROXY','https_proxy','HTTPS_PROXY','all_proxy','ALL_PROXY','no_proxy','NO_PROXY') {
    [System.Environment]::SetEnvironmentVariable($n, $null)
  }
  ```
- 若报**一堆「无法解析的外部符号」但没有任何编译错误**，先确认工程文件里 `Function\*.cpp` 的 10 个条目还在（被 Visual Studio 用旧的内存副本覆盖过就会丢）：
  ```powershell
  Select-String -Path AccountManager\AccountManager.vcxproj -Pattern '<ClCompile Include='   # 正常 11 行
  git checkout -- AccountManager/AccountManager.vcxproj                                       # 丢了就恢复
  ```

## 使用提示：怎么拿到 otpauth:// 链接
- 网站正在给你**做 2FA 设置**时：二维码本身就是 `otpauth://` 链接，可直接截图后用「📷 二维码图片」导入；若页面同时给了「无法扫描？手动输入密钥」，直接粘贴那串密钥更省事。
- **已经开过 2FA**：回到网站重新走一次「添加验证器 / 添加登录方式」，会再给一个二维码或密钥（注意有些站点会作废旧密钥）。
- **从其他验证器导出**：Bitwarden / 1Password / KeePassXC / Aegis / 2FAS 等支持导出，直接取 `secret`；Authy 不提供导出；Microsoft Authenticator 不显示密钥。
- ⚠️ TOTP 密钥 / 恢复代码等同第二因素，不要粘贴到在线二维码解码网站或聊天工具。

## 应用内添加 Steam 令牌（已验证可用）
在 Steam 条目里点「🛡️ 应用内添加令牌」，按向导走完，本应用就能为该账号出 5 位 2FA 动态码。全部走 Steam 官方接口：

1. `IAuthenticationService/GetPasswordRSAPublicKey` —— **必须用 GET**，入参放在查询串的 `input_protobuf_encoded` 里（用 POST 会返回 405）
2. Windows CNG 做 **RSA PKCS#1 v1.5** 加密密码 → `BeginAuthSessionViaCredentials` 建立登录会话
3. 按 Steam 要求提交验证：邮箱验证码 / 现有令牌动态码 / 手机 App 确认
4. `UpdateAuthSessionWithSteamGuardCode` 提交验证码（**该接口不返回 token**）→ 再用 `PollAuthSessionStatus` 轮询拿 access / refresh token
5. `ITwoFactorService/AddAuthenticator` —— Steam 下发 `shared_secret` / `identity_secret` / `revocation_code`
6. `FinalizeAddAuthenticator` 用短信 / 邮箱激活码完成激活 —— **必须同时带上用新密钥算出的 `authenticator_code`**，且 `authenticator_time` 用服务端返回的 `server_time`，否则一律失败
7. 成功后密钥与撤销码自动写入该条目（随保险库一起加密）

注意事项：
- ⚠️ **务必离线保存撤销码**：换机、重装、保险库损坏时，它是唯一能移除令牌的凭据
- ⚠️ **手机 Steam App 与本应用二选一**：令牌由本应用持有后，官方 App 对该账号只提供「移除」，不会同步出码；官方 App 上**不要点「移除」**，那会作废本应用里的令牌
- ⚠️ 新增手机令牌会让账号进入 **7 天交易 / 市场冷却**
- 排查：登记过程会在 EXE 同目录写 `steam_enroll.log`（**不含密码、密钥、验证码**）
## 应用内移除 Steam 令牌
条目里点「🗑️ 移除该账号的手机令牌」即可（仅当该条目已有密钥或撤销码时显示），与添加令牌共用同一套登录机制：

1. 登录该账号（可能要求邮箱验证码或当前令牌动态码）
2. 输入该令牌的**撤销码**
3. 调用 `ITwoFactorService/RemoveAuthenticator`（**POST**；需要已登录的 access_token，该接口没有 steamid 字段），Steam 用 `revocation_attempts_remaining` 限制重试次数
4. 成功后自动清空该条目的密钥与撤销码

用途：令牌已不在你手上、或想把它从本应用彻底摘掉时，不必再碰官方 App。


## Android 手机端（2FA）

目录 `android/`，独立 Kotlin 应用：**不联网、不碰账号**，只保存密钥并出码。

- **两种令牌**：Steam Guard（5 位字母）与**标准 TOTP**（6/7/8 位数字，SHA1/SHA256/SHA512）自动识别，共用倒计时与复制交互
- 卡片式列表：大号动态码 + 剩余秒数 + 进度条（≤5 秒变橙），点卡片复制，长按改名 / 删除 / 看撤销码
- **拖动排序**：卡片数字右侧有自绘拖动手柄，按住即可拖动改顺序 —— 拖动时被拖卡片**跟手浮动**、原位置塌成空槽、目标位置实时预览，其它卡片**实时让位**，松手立即写入加密存储；右上角菜单「编辑卡片」里同样用这套拖动交互
- 右上角三条杆菜单：编辑卡片 / 从电脑同步 / 关于；右下角圆形＋：手动添加 / 扫描二维码 / 图片导入（「从电脑同步」见下文）
- **从电脑同步**：扫桌面端「同步到手机」生成的二维码（多张时扫完一张会自动接着弹下一张；也能用「图片导入」逐张选），收齐后按**密钥去重**合并 —— 只新增，绝不删改手机上已有的条目；扫了一半退出可以「继续同步」，菜单里也会出现「取消未完成的同步（i/n）」
- 密钥用 **Android Keystore + AES-256-GCM** 加密存本机（`filesDir/entries.bin`），「关于」页显示版本与项目地址（版本号默认与桌面端同步，见「版本与发布」一节）
- **防丢数据**：写盘先写 `entries.bin.tmp` 再原子改名，覆盖前留一份 `entries.bin.bak`，主文件读不出来时自动回退到备份；启动解析失败时**保持原样**（不清空、不保存），不会把「半截列表」写回去
- 启动器图标为手绘「手机 + 2FA + 锁」线稿：使用 Android 8+ **自适应图标**（黑底 + 白线稿前景，图案已留出安全边距，圆形 / 方形遮罩都不会切到），并附带传统方形 / 圆形多密度图标兜底
- 算法与桌面端一致（HMAC-SHA1 + 26 字母表取 5 位），23 个单测含与桌面端相同的固定向量

```
# 测试 + 调试版
gradlew -p android testDebugUnitTest assembleDebug
#   → android/app/build/outputs/apk/debug/app-debug.apk

# 发布版（用 android/keystore/release.jks 签名）
gradlew -p android testDebugUnitTest assembleRelease
#   → android/app/build/outputs/apk/release/app-release.apk
```

> 调试版与发布版**签名不同，无法互相覆盖安装**；换签名安装需先卸载，而这会**清空 App 内数据**。
> 签名密钥库不在版本控制中（`.gitignore` 有意排除），请自行备份 `android/keystore/release.jks` 与其中的密码。

单元测试 23 个用例：与桌面端一致的固定向量、字母表 / 时间窗 / 周期、标准 TOTP 的 RFC 6238 向量、`.maFile`/Base64/otpauth 解析、非法输入拒绝、二维码「编码→解码」往返、同步包解析（含桌面端实际产物样本）。

## 同步 2FA 到手机

桌面端「设置 → 📲 同步到手机（2FA）」→ 手机端「右上角菜单 → 从电脑同步」→ 扫码。全程离线：手机端不联网、不加 `INTERNET` 权限，密钥只从屏幕上的二维码进手机。

**传什么**：所有填了 2FA 密钥的条目（标准 TOTP 与 Steam Guard 都算），包含名称、类型、密钥、算法、位数、周期，Steam 条目还会带上撤销码；密码、备注等其它字段**不传**。密钥格式不正确的条目会跳过，界面上会提示跳过了几条。

**多张二维码**：同步包按 900 字节一页切分（约 10~15 条一页，视名称长度而定），一页一张二维码 —— 二维码越大格子越密、越难扫，所以宁可多分几张。手机端每扫完一张会自动接着弹扫码界面，全部收齐才一次性合并。

**合并规则**：以密钥去重 —— 手机上已有相同密钥的记录直接跳过，其余追加为**新条目**；不会覆盖、不会删除手机上已有的条目（所以重复同步是安全的，只会提示「已存在 N 条」）。

**格式**（两个平台必须一致，改动时请同步改两边实现与单测）：

```
AM2FA/1                                    ← 魔数 + 格式版本
1/3                                        ← 本页序号 / 总页数
名称 \t 类型 \t 密钥 \t 算法 \t 位数 \t 周期 \t 撤销码   ← 一条记录一行，制表符分隔
```

- 桌面端：`frontend/src/syncPackage.js`（打包 / 分页）
- 手机端：`android/app/src/main/java/com/accountmanager/steamguard/SyncPackage.kt`（解析，复用 `SecretParser` 做 Base32→Base64）
- 兼容性由 `SyncPackageTest` 兜底：测试里直接内嵌了一份桌面端真实生成的同步包

⚠️ 同步包等价于全部 2FA 密钥，别截图外传；扫完即关。

## 版本与发布

当前版本 **1.0.2** · 许可 **GPL-3.0-only**（第三方组件见 [LICENSE.md](LICENSE.md)）· 仓库 <https://github.com/No-Zero-issue/AccountManager>

版本号只在**一处**定义：`AccountManager/version.h` 顶部的四个数字

```c
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 0
#define APP_VERSION_PATCH 2
#define APP_VERSION_BUILD 0
```

改完这四行，下列位置会一起变，不需要再逐个手改：

| 位置 | 取值方式 |
|---|---|
| EXE 文件属性里的版本（`VERSIONINFO`，四位） | `AccountManager.rc` 引用 `version.h` 的宏 |
| RC 关于框里的「AccountManager, 版本 x.y.z」 | 同上 |
| HTTP `User-Agent`（`AccountManager/1.0.2`） | `Function/Http.cpp` 用 `APP_USER_AGENT` |
| 前端状态栏 `· v1.0.2` 与「关于」弹窗 | `frontend/vite.config.js` 解析 `version.h`，注入 `__APP_VERSION__` |

界面、关于框、`User-Agent` 显示的都是**主.次.修订**三位；第四位 `APP_VERSION_BUILD` 只进 EXE 文件属性，改它界面不会变。

两个注意点（都是 rc.exe 的预处理器太弱）：

- 给 `.rc` 用的版本号必须是**单个字符串字面量**（rc.exe 不支持 `"a" "b"` 拼接出现在对话框资源里），所以 `version.h` 里另有 `APP_VERSION_LABEL` / `APP_VERSION_FULL` 两个字符串要跟着数字一起改；忘了改的话，C++ 侧的 `static_assert` 会让编译直接失败并提示你。
- `version.h` 保持 **UTF-8 带 BOM**、正文只用 ASCII：`.rc` 是 UTF-16，rc.exe 从被 `#include` 的文件里读中文会报错。

### 手机端版本号：默认同步，可改独立

`android/app/build.gradle.kts` 不再写死版本号：

- **默认同步**：解析仓库里的 `AccountManager/version.h`，`versionName` 取**主.次.修订**（当前 `1.0.2`），`versionCode` 按 `主*10000 + 次*100 + 修订` 算（当前 `10002`，Android 要求只增不减）。
- **改成独立**：在 `android/gradle.properties` 里取消注释 `androidVersionName` / `androidVersionCode`（例如手机端单独发个热修 `1.1` / `10001`）。只写一个也行，另一个仍从 `version.h` 取；两个都写了就完全不读 `version.h`，连 `android/` 目录单独复制出去也能构建。
- 手机「关于」页显示的仍是系统里的 `versionName`（`packageManager` 读取），所以上面怎么改，界面就显示什么，Kotlin 侧不用动。

桌面端所有状态集中在 EXE 同目录的**单个 `AccountManager.json`**（分区：`vault` / `hello` / `config`）。首次运行若检测到旧的 `vault.json` / `hello.json` / `config.json`，会自动合并进去；**旧文件保留不删**，可随时回滚。

产物：
- 桌面端 `x64/Release/AccountManager.exe` —— 图标是手绘「密码保险箱」线稿，含 16 / 24 / 32 / 48 / 64 / 128 / 256 七档，且**全部使用 32bpp BMP 条目**（`LoadIcon` 对 PNG 压缩条目的支持不可靠）
- 手机端 `android/app/build/outputs/apk/release/2FA.apk` —— 图标是手绘「手机 + 2FA + 锁」线稿的自适应图标

构建提示：改过 `frontend/src` 后必须 `npm.cmd run build` **并触碰 `AccountManager/AccountManager.rc`** 再编译，否则新界面进不了 EXE；详见「构建」一节的排错条目。

## 许可
本项目以 **GNU 通用公共许可证第 3 版（GPL-3.0）** 发布，Copyright (C) 2026 Account Manager 开发者。

- 你可以自由使用、修改、再分发本项目，但**再分发（包括分发编译好的 EXE）时必须提供完整源代码**，且衍生作品必须以同样的许可（GPL-3.0）发布。
- 本程序不提供任何担保。完整条款见 [LICENSE.md](LICENSE.md)（含官方全文）。
- 项目内第三方组件（桌面端：nlohmann/json、Vue、jsQR、WIL、WebView2 SDK；手机端：ZXing core / zxing-android-embedded、AndroidX、JUnit）遵循各自许可，均与 GPL-3.0 兼容，清单与说明见 [LICENSE.md](LICENSE.md)。
