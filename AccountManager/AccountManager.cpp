// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// AccountManager.cpp —— 程序入口 / 窗口过程 / WebView2 宿主 / 前端消息分发
//   业务逻辑按功能拆在 Function/ 目录（见 Function/Function.h 的说明）。

#include "Function/Function.h"

#define MAX_LOADSTRING 100


HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本

// 无边框窗口的初始尺寸（最小可拖拽尺寸见 WM_GETMINMAXINFO：1100x830）
int w = 1280;
int h = 860;


// 遍历 WebView2 的子窗口。
// 说明：本应用的拖拽 / 缩放由前端（windowControl 指令）驱动，不依赖子窗口命中测试，
// 因此这里保持不改变任何子窗口状态，避免影响页面内鼠标交互。
BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    (void)hwnd;
    (void)lParam;
    return TRUE;
}

WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名
bool g_isDarkTheme = true;                      // 主题状态
bool g_isHoveringWindowControls = false;        // 窗口控制按钮悬浮状态
bool g_isTrackingMouse = false;                 // 鼠标追踪状态

// WebView2 变量
wil::com_ptr<ICoreWebView2Controller> webviewController;
wil::com_ptr<ICoreWebView2> webview;

// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 解决高分屏模糊：开启进程 DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // WebView2 依赖于 COM，必须初始化 COM 环境
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        // 使用转义字符表示：“COM 初始化失败”，“错误”
        MessageBoxW(nullptr, L"COM \u521d\u59cb\u5316\u5931\u8d25", L"\u9519\u8bef", MB_OK);
        return FALSE;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_ACCOUNTMANAGER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_ACCOUNTMANAGER));

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();

    return (int) msg.wParam;
}

// 注册窗口类
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ACCOUNTMANAGER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = CreateSolidBrush(RGB(30, 30, 30)); // 深色主题背景 #1e1e1e
    wcex.lpszMenuName   = NULL; // Remove menu
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}
// 启动窗口拖动 / 缩放。
// 注意：这里必须用 PostMessage 而不是 SendMessage。
// SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION/HTxxx, 0) 会立刻进入系统的
// 模态拖动/缩放循环，而本函数是在 WebView2 的 WebMessageReceived 回调里被调用的，
// 模态循环会重入派发消息给 WebView2 的窗口，触发其内部 CHECK -> int3
// （Debug 下表现为“已执行断点指令”，Release 下可能直接崩溃）。
// 改为投递消息，让拖动从正常消息循环启动；同时用真实光标位置作为拖动锚点，
// 避免窗口因 lParam 为 0 而跳位。
void StartWindowMoveOrResize(HWND hWnd, WPARAM hitTest) {
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    ReleaseCapture();
    PostMessage(hWnd, WM_NCLBUTTONDOWN, hitTest, MAKELPARAM(cursor.x, cursor.y));
}

// 初始化窗口
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance;

   // 加载主题设置
   json initConfig = ReadConfigJson();
   if (initConfig.contains("theme")) {
       std::string theme = initConfig.value("theme", "dark");
       g_isDarkTheme = (theme != "light");
   }

   // 1. WS_OVERLAPPEDWINDOW + WM_NCCALCSIZE(return 0) 实现无边框 + 可缩放，且保留系统动画
   // WS_EX_APPWINDOW 确保在任务栏显示图标
   HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, szWindowClass, szTitle, 
      WS_OVERLAPPEDWINDOW,   // 含 WS_CAPTION：让 DWM 播放标准最小化/最大化动画；非客户区由 WM_NCCALCSIZE 去掉
      CW_USEDEFAULT, 0, w, h, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd) return FALSE;

   // 2. 启用 DWM 阴影 (WS_POPUP 默认没阴影，需要手动开启)
   const MARGINS shadow_margins = { 1, 1, 1, 1 };
   DwmExtendFrameIntoClientArea(hWnd, &shadow_margins);

   // 获取程序目录
   WCHAR path[MAX_PATH];
   GetModuleFileNameW(NULL, path, MAX_PATH);
   std::filesystem::path exePath(path);
   std::wstring rootDir = exePath.parent_path().wstring();

   // 注册内联资源 Scheme 来处理 res:// 或者将 webview 拦截来处理内部资源
   // 不过为了方便，最快的方法仍然是将打包出来的那个单 HTML 文件在运行时写入到 Temp 目录再加载。
   
   // 为了单 EXE 发布，将 WebView2 的用户数据目录重定向到系统的临时目录 (Temp)
   WCHAR tempPath[MAX_PATH];
   GetTempPathW(MAX_PATH, tempPath);
   std::wstring userDataFolder = std::wstring(tempPath) + L"AccountManager_WebView2";

   // 释放内嵌的 HTML 到临时目录
   std::wstring htmlPath = userDataFolder + L"\\index.html";
   
   // 尝试从不同的资源类型名称加载
   HRSRC hResInfo = FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_HTML1), L"HTML");
   if (!hResInfo) {
       hResInfo = FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_HTML1), RT_HTML);
   }

   if (hResInfo) {
       HGLOBAL hResData = LoadResource(hInstance, hResInfo);
       if (hResData) {
           DWORD resSize = SizeofResource(hInstance, hResInfo);
           void* pData = LockResource(hResData);
           if (pData) {
               // 确保目录创建成功（如果是多级目录可能需要分别创建）
               CreateDirectoryW(userDataFolder.c_str(), NULL);
               
               HANDLE hFile = CreateFileW(htmlPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
               if (hFile != INVALID_HANDLE_VALUE) {
                   DWORD bytesWritten;
                   WriteFile(hFile, pData, resSize, &bytesWritten, NULL);
                   CloseHandle(hFile);
               } else {
                   MessageBoxW(NULL, L"无法释放 index.html 到 Temp 目录", L"文件写入错误", MB_OK);
                   return 0;
               }
           }
       } else {
           MessageBoxW(NULL, L"无法加载 HTML 资源", L"资源错误", MB_OK);
           return 0;
       }
   } else {
       MessageBoxW(NULL, L"找不到 IDR_HTML1 资源", L"资源错误", MB_OK);
       return 0;
   }

   // 初始化 WebView2
   HRESULT res = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
       Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
           [hWnd, rootDir, userDataFolder](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
               if (FAILED(result)) {
                   // 转义内容：“WebView2 环境创建失败: ”，“错误”
                   MessageBoxW(hWnd, (L"WebView2 \u73af\u5883\u521b\u5efa\u5931\u8d25: " + std::to_wstring(result)).c_str(), L"\u9519\u8bef", MB_OK);
                   return result;
               }

               env->CreateCoreWebView2Controller(hWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [hWnd, rootDir, userDataFolder](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                       if (FAILED(result) || controller == nullptr) {
                           // 转义内容：“WebView2 控制器创建失败: ”，“错误”
                           MessageBoxW(hWnd, (L"WebView2 \u63a7\u5236\u5668\u521b\u5efa\u5931\u8d25: " + std::to_wstring(result)).c_str(), L"\u9519\u8bef", MB_OK);
                           return result;
                       }

                       webviewController = controller;
                       webviewController->get_CoreWebView2(&webview);

                       // 彻底解决消息穿透问题：遍历并子类化 WebView2 下的所有子窗口
                       EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);

                       // 配置 WebView2
                       ICoreWebView2Settings* Settings;
                       webview->get_Settings(&Settings);
                       Settings->put_IsScriptEnabled(TRUE);
                       Settings->put_AreDefaultContextMenusEnabled(FALSE);
                       Settings->put_IsWebMessageEnabled(TRUE);

                       // 1. 拦截所有新窗口请求，改用系统默认浏览器打开
                       webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                           [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                               wil::unique_cotaskmem_string uri;
                               args->get_Uri(&uri);
                               ShellExecuteW(NULL, L"open", uri.get(), NULL, NULL, SW_SHOWNORMAL);
                               args->put_Handled(TRUE); // 禁止 WebView 内部打开新窗口
                               return S_OK;
                           }).Get(), nullptr);

                       // 移除之前的禁用滚动手脚...

                       // 设置初始大小并显示 (透明边框模式，填满整个窗口)
                       RECT rc;
                       GetClientRect(hWnd, &rc);
                       int border = 0; // 透明边框模式：WebView2 填满整个窗口
                       RECT webviewRect = {
                           border,
                           border,
                           rc.right - border,
                           rc.bottom - border
                       };
                       webviewController->put_Bounds(webviewRect);
                       webviewController->put_IsVisible(TRUE);

                       // 映射虚拟域名到本地目录，解决 file:/// 协议的各类限制
                       wil::com_ptr<ICoreWebView2_3> webview3;
                       if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
                           webview3->SetVirtualHostNameToFolderMapping(
                               L"app.assets", 
                               userDataFolder.c_str(), 
                               COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
                           );
                       }

                       // 注册消息处理
                       webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                           [hWnd](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                               wil::unique_cotaskmem_string messageRaw;
                               // 重要：前端发送的是对象，这里必须使用 get_WebMessageAsJson
                               args->get_WebMessageAsJson(&messageRaw);
                               if (!messageRaw) return S_OK;

                               try {
                                   std::wstring wmsg(messageRaw.get());
                                   std::string msgUtf8 = WStringToString(wmsg);
                                   json j = json::parse(msgUtf8);

                                   std::string type = j.value("type", "");

                                    // 1. 查询保险库状态（是否已初始化 / 是否已解锁 / 是否启用了 Windows Hello）
                                    if (type == "getVaultStatus") {
                                        json response = {
                                            {"type", "vaultStatus"},
                                            {"initialized", VaultFileExists()},
                                            {"unlocked", VaultIsUnlocked()},
                                            {"helloEnabled", HelloFileExists()},
                                            {"helloBusy", HelloIsBusy()}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 1.1 用 Windows Hello 解锁（弹出 Windows 安全中心验证，工作线程执行）
                                    else if (type == "helloUnlock") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            HelloSetBusy(true);
                                            std::thread([hWnd]() {
                                                RunBackgroundWorker(hWnd, [] { return HelloUnlockWorker(); });
                                            }).detach();
                                        }
                                    }
                                    // 1.2 启用 Windows Hello 解锁（创建/打开 TPM 凭据并包装保险库密钥）
                                    else if (type == "helloEnable") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (!VaultIsUnlocked()) {
                                            json response = { {"type", "vaultError"}, {"code", "locked"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            std::vector<BYTE> aesKey;
                                            std::vector<BYTE> macKey;
                                            std::vector<BYTE> salt;
                                            ULONGLONG iterations = 0;
                                            bool unlocked = false;
                                            SnapshotVaultKeys(aesKey, macKey, salt, iterations, unlocked);
                                            HelloSetBusy(true);
                                            std::thread([hWnd, aesKey, macKey, salt, iterations]() {
                                                RunBackgroundWorker(hWnd, [aesKey, macKey, salt, iterations] {
                                                    return HelloEnableWorker(aesKey, macKey, salt, iterations);
                                                });
                                            }).detach();
                                        }
                                    }
                                    // 1.3 关闭 Windows Hello 解锁（删除 hello.json 与 Hello 凭据）
                                    else if (type == "helloDisable") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            HelloSetBusy(true);
                                            std::thread([hWnd]() {
                                                RunBackgroundWorker(hWnd, [] { return HelloDisableWorker(); });
                                            }).detach();
                                        }
                                    }
                                    // 2. 首次创建保险库（设置主密码），会顺带导入旧的 accounts.json
                                    else if (type == "createVault") {
                                        std::string password = j.value("masterPassword", "");
                                        if (password.size() < 6) {
                                            json response = { {"type", "vaultError"}, {"code", "password_too_short"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (VaultFileExists()) {
                                            json response = { {"type", "vaultError"}, {"code", "vault_exists"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json entries = MigrateLegacyAccounts();
                                            json categories = j.value("categories", json::array());
                                            int imported = (int)entries.size();
                                            std::wstring err;
                                            if (VaultCreate(password, entries, categories, err)) {
                                                json response = {
                                                    {"type", "vaultUnlocked"},
                                                    {"entries", entries},
                                                    {"categories", categories},
                                                    {"importedCount", imported}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                            else {
                                                json response = {
                                                    {"type", "vaultError"},
                                                    {"code", "create_failed"},
                                                    {"detail", WStringToString(err)}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 3. 解锁保险库
                                    else if (type == "unlockVault") {
                                        std::string password = j.value("masterPassword", "");
                                        json entries = json::array();
                                        json categories = json::array();
                                        std::string code;
                                        if (VaultUnlock(password, entries, categories, code)) {
                                            json response = {
                                                {"type", "vaultUnlocked"},
                                                {"entries", entries},
                                                {"categories", categories}
                                            };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json response = { {"type", "vaultError"}, {"code", code} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                    }
                                    // 4. 保存条目（自动加密后写入 vault.json）
                                    else if (type == "saveVault") {
                                        if (!VaultIsUnlocked()) {
                                            json response = { {"type", "vaultError"}, {"code", "locked"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (j.contains("entries")) {
                                            std::wstring err;
                                            if (VaultSave(j["entries"], j.value("categories", json::array()), err)) {
                                                json response = { {"type", "vaultSaved"} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                            else {
                                                json response = {
                                                    {"type", "vaultError"},
                                                    {"code", "save_failed"},
                                                    {"detail", WStringToString(err)}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 5. 锁定保险库（清空内存密钥）
                                    else if (type == "lockVault") {
                                        VaultLock();
                                        json response = { {"type", "vaultLocked"} };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 6. 修改主密码
                                    else if (type == "changeMasterPassword") {
                                        std::string current = j.value("currentPassword", "");
                                        std::string next = j.value("newPassword", "");
                                        if (next.size() < 6) {
                                            json response = { {"type", "vaultError"}, {"code", "password_too_short"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json entries = json::array();
                                            std::string code;
                                            if (VaultChangeMasterPassword(current, next, entries, code)) {
                                                // 密钥已更换：若启用了 Hello 解锁，需要在工作线程里用新密钥重新包装
                                                std::vector<BYTE> aesKey;
                                                std::vector<BYTE> macKey;
                                                std::vector<BYTE> salt;
                                                ULONGLONG iterations = 0;
                                                bool unlocked = false;
                                                SnapshotVaultKeys(aesKey, macKey, salt, iterations, unlocked);
                                                HelloSetBusy(true);
                                                std::thread([hWnd, aesKey, macKey, salt, iterations]() {
                                                    RunBackgroundWorker(hWnd, [aesKey, macKey, salt, iterations] {
                                                        return HelloRewrapWorker(aesKey, macKey, salt, iterations);
                                                    });
                                                }).detach();
                                            }
                                            else {
                                                json response = { {"type", "vaultError"}, {"code", code} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 7. 处理获取配置请求
                                    else if (type == "getConfig") {
                                        json config = ReadConfigJson();
                                        json response = {
                                            {"type", "setConfig"},
                                            {"config", config}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 8. 处理保存配置请求
                                    else if (type == "saveConfig") {
                                        if (j.contains("config")) {
                                            SaveConfigJson(j["config"]);
                                            // 更新主题设置并重绘窗口边框
                                            if (j["config"].contains("theme")) {
                                                std::string theme = j["config"]["theme"].get<std::string>();
                                                bool newIsDark = (theme != "light");
                                                if (g_isDarkTheme != newIsDark) {
                                                    g_isDarkTheme = newIsDark;
                                                    // 重绘窗口边框
                                                    RECT rc;
                                                    GetClientRect(hWnd, &rc);
                                                    InvalidateRect(hWnd, &rc, TRUE);
                                                }
                                            }
                                        }
                                    }
                                    // 9. 复制文本到剪贴板（避免依赖浏览器剪贴板权限）
                                    else if (type == "copyText") {
                                        std::string text = j.value("text", "");
                                        if (!text.empty()) {
                                            std::wstring wText = StringToWString(text);
                                            if (OpenClipboard(hWnd)) {
                                                EmptyClipboard();
                                                size_t bytes = (wText.size() + 1) * sizeof(wchar_t);
                                                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                                                if (hMem) {
                                                    void* dst = GlobalLock(hMem);
                                                    if (dst) {
                                                        memcpy(dst, wText.c_str(), bytes);
                                                        GlobalUnlock(hMem);
                                                        SetClipboardData(CF_UNICODETEXT, hMem);
                                                    }
                                                }
                                                CloseClipboard();
                                                json response = { {"type", "clipboardCopied"} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 10. 处理打开外部链接请求
                                    else if (type == "openUrl") {
                                        std::string url = j.value("url", "");
                                        if (!url.empty()) {
                                            ShellExecuteW(NULL, L"open", StringToWString(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
                                        }
                                    }
                                    // 11. 打开本地程序 / 文件（软件类条目）
                                    else if (type == "openPath") {
                                        std::string target = j.value("path", "");
                                        if (!target.empty()) {
                                            ShellExecuteW(NULL, L"open", StringToWString(target).c_str(), NULL, NULL, SW_SHOWNORMAL);
                                        }
                                    }
                                    // 11.5 从本机 Steam 导入已登录账号（只读 loginusers.vdf）
                                    else if (type == "steamImportAccounts") {
                                        std::wstring importError;
                                        json accounts = ReadSteamLoginUsers(importError);
                                        json response = {
                                            {"type", "steamAccounts"},
                                            {"accounts", accounts},
                                            {"error", WStringToString(importError)}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 12. Steam 一键登录（关闭 Steam -> 写自动登录 -> 启动，工作线程执行）
                                    else if (type == "steamLogin") {
                                        std::string username = j.value("username", "");
                                        std::string password = j.value("password", "");
                                        if (username.empty()) {
                                            json response = { {"type", "steamLoginResult"}, {"ok", false}, {"code", "steam_no_username"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (g_steamBusy) {
                                            json response = { {"type", "steamLoginResult"}, {"ok", false}, {"code", "steam_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            g_steamBusy = true;
                                            std::wstring wUser = StringToWString(username);
                                            std::wstring wPass = StringToWString(password);
                                            std::thread([hWnd, wUser, wPass]() {
                                                RunBackgroundWorker(hWnd, [wUser, wPass] {
                                                    return SteamLoginWorker(wUser, wPass);
                                                });
                                            }).detach();
                                        }
                                    }
                                   // 13. Steam 令牌登记（应用内添加手机令牌，工作线程执行）
                                   else if (type == "steamEnrollStart") {
                                       std::string username = j.value("username", "");
                                       std::string password = j.value("password", "");
                                       std::string mode = j.value("mode", "add");
                                       std::string revocationCode = j.value("revocationCode", "");
                                       if (username.empty() || password.empty()) {
                                           json response = SteamEnrollErrorJson("missing_credentials", L"账号或密码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (g_steamEnrollBusy) {
                                           json response = SteamEnrollErrorJson("busy", L"已有令牌操作正在进行");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, username, password, mode, revocationCode]() {
                                               RunBackgroundWorker(hWnd, [username, password, mode, revocationCode] {
                                                   return SteamEnrollStartWorker(username, password, mode, revocationCode);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollGuardCode") {
                                       std::string code = j.value("code", "");
                                       int codeType = j.value("codeType", 3);
                                       if (code.empty()) {
                                           json response = SteamEnrollErrorJson("missing_code", L"验证码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, codeType, code]() {
                                               RunBackgroundWorker(hWnd, [codeType, code] {
                                                   return SteamEnrollGuardCodeWorker(codeType, code);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollPoll") {
                                       if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd]() {
                                               RunBackgroundWorker(hWnd, [] {
                                                   return SteamEnrollPollWorker();
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollActivation") {
                                       std::string code = j.value("code", "");
                                       if (code.empty()) {
                                           json response = SteamEnrollErrorJson("missing_code", L"激活码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, code]() {
                                               RunBackgroundWorker(hWnd, [code] {
                                                   return SteamEnrollActivationWorker(code);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollCancel") {
                                       {
                                           std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
                                           g_steamEnroll.Clear();
                                       }
                                       g_steamEnrollBusy = false;
                                       json response = { {"type", "steamEnrollCancelled"} };
                                       std::wstring wResponse = StringToWString(response.dump());
                                       webview->PostWebMessageAsJson(wResponse.c_str());
                                   }
                                   // 5. 窗口控制指令
                                   else if (type == "windowControl") {
                                       std::string action = j.value("action", "");
                                       if (action == "minimize") {
                                           PostMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);   // 走系统命令以保留最小化动画
                                       }
                                       else if (action == "maximize") {
                                           if (IsZoomed(hWnd)) PostMessage(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);   // 走系统命令以保留动画
                                           else PostMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);   // 走系统命令以保留动画
                                       }
                                       else if (action == "close") {
                                           PostMessage(hWnd, WM_CLOSE, 0, 0);
                                       }
                                       else if (action == "startDrag") {
                                           // 核心：请求系统拖拽（投递消息启动，勿在 WebView2 回调里同步调用）
                                                                                      StartWindowMoveOrResize(hWnd, HTCAPTION);
                                       }
                                       // 窗口缩放控制
                                       else if (action == "startResizeLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTLEFT);
                                       }
                                       else if (action == "startResizeRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTRIGHT);
                                       }
                                       else if (action == "startResizeTop") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOP);
                                       }
                                       else if (action == "startResizeBottom") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOM);
                                       }
                                       else if (action == "startResizeTopLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOPLEFT);
                                       }
                                       else if (action == "startResizeTopRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOPRIGHT);
                                       }
                                       else if (action == "startResizeBottomLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOMLEFT);
                                       }
                                       else if (action == "startResizeBottomRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOMRIGHT);
                                       }
                                   }
                               }
                               catch (const std::exception& e) {
                                   OutputDebugStringA(e.what());
                               }

                               return S_OK;
                           }).Get(), nullptr);

                       // 导航到页面 (使用资源协议加载内部打包的 html)
                       // 映射虚拟域名到虚拟的资源路径
                       webview->Navigate(L"https://app.assets/index.html");

                       return S_OK;
                   }).Get());
               return S_OK;
           }).Get());

   if (FAILED(res)) {
       // 转义内容：“WebView2 初始化调用失败，请检查是否安装了 WebView2 Runtime”，“错误”
       MessageBoxW(hWnd, L"WebView2 \u521d\u59cb\u5316\u8c03\u7528\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u662f\u5426\u5b89\u88c5\u4e86 WebView2 Runtime", L"\u9519\u8bef", MB_OK);
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}
// 窗口过程函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // 处理窗口消息
    switch (message)
    {
    //  绘制窗口背景（透明边框版本）
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        
        // 根据主题选择背景颜色（匹配 Vue CSS 变量）
        COLORREF backColor = g_isDarkTheme ? RGB(30, 30, 30) : RGB(243, 243, 243);    // 深色: #1e1e1e, 浅色: #f3f3f3
        HBRUSH hBackBrush = CreateSolidBrush(backColor);
        
        // 填充整个客户区为背景色，确保在WebView2加载前显示统一背景
        FillRect(hdc, &rc, hBackBrush);
        
        DeleteObject(hBackBrush);
        return 1;
    }
    // 处理鼠标移动消息
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);

        int windowControlsLeft = rc.right - 140;
        bool isOverControls = (pt.x >= windowControlsLeft && pt.y <= 32);

        if (isOverControls != g_isHoveringWindowControls) {
            g_isHoveringWindowControls = isOverControls;
            RECT hoverRect = { rc.right - 140, 0, rc.right, 4 };
            InvalidateRect(hWnd, &hoverRect, FALSE);
        }

        if (!g_isTrackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_isTrackingMouse = true;
        }
        break;
    }
    // 处理鼠标离开消息
    case WM_MOUSELEAVE:
    {
        g_isTrackingMouse = false;
        if (g_isHoveringWindowControls) {
            g_isHoveringWindowControls = false;
            RECT rc;
            GetClientRect(hWnd, &rc);
            RECT hoverRect = { rc.right - 140, 0, rc.right, 4 };
            InvalidateRect(hWnd, &hoverRect, FALSE);
        }
        break;
    }
    // 处理窗口激活消息
    case WM_NCACTIVATE:
        // 阻止系统在激活/非激活时绘制默认标题栏/边框
        return TRUE;
    // 处理鼠标双击消息
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // 注意：WM_LBUTTONDBLCLK 的坐标是 client 坐标，无需转换
        RECT rc;
        GetClientRect(hWnd, &rc);
        const int titleHeight = 32;

        // 如果双击在标题栏区域（且避开了右侧控制按钮），执行最大化/还原
        if (pt.y < titleHeight && pt.x < rc.right - 140) {
            if (IsZoomed(hWnd)) PostMessage(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            else PostMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            return 0;
        }
        break;
    }
    // 处理窗口大小调整消息
    case WM_NCCALCSIZE:
        // WS_POPUP 模式下不需要特殊处理 NC 区域，直接返回 0 即可
        return 0;
    // 处理窗口大小调整消息
    case WM_NCHITTEST:
    {
        // 前端检测鼠标位置并处理窗口缩放，此处始终返回HTCLIENT让前端处理
        return HTCLIENT;
    }
    // 处理窗口大小调整消息
    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        // 设置最小可拖拽尺寸
        lpMMI->ptMinTrackSize.x = 1100;
        lpMMI->ptMinTrackSize.y = 830;
        
        // 关键：处理 WS_POPUP 模式下的最大化溢出
        // 获取窗口所在的当前显示器
        HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        if (hMonitor != NULL) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(hMonitor, &mi)) {
                // ptMaxPosition 是相对于系统主显示器 (0,0) 的偏移
                // 所以不需要减去 rcMonitor.left，只需计算屏幕左上角相对工作区左上角的偏移
                // 在多显示器且不同分辨率下，正确设置偏移非常重要
                lpMMI->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                lpMMI->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                
                // 设置最大化时的宽高为工作区 (rcWork) 的宽高
                lpMMI->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                lpMMI->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                
                // 限制最大可拖拽范围
                lpMMI->ptMaxTrackSize.x = lpMMI->ptMaxSize.x;
                lpMMI->ptMaxTrackSize.y = lpMMI->ptMaxSize.y;
            }
        }
        return 0; // 必须返回 0 表示已处理
    }
    // 处理窗口大小调整消息
    case WM_SIZE:
        if (webviewController != nullptr) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            
            // 透明边框模式：WebView2 填满整个窗口，缩放通过 WM_NCHITTEST 处理
            int border = 0;
            
            RECT webviewRect = {
                border,
                border,
                rc.right - border,
                rc.bottom - border
            };
            webviewController->put_Bounds(webviewRect);
            
            // 关键：重新遍历子窗口
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理进入缩放/移动模式
    case WM_ENTERSIZEMOVE:
        // 缩放开始时重新子类化所有子窗口
        if (webviewController != nullptr) {
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理退出缩放/移动模式
    case WM_EXITSIZEMOVE:
        // 缩放结束后重新子类化所有子窗口
        if (webviewController != nullptr) {
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理后台工作线程（Windows Hello / Steam 登录）的结果：回到 UI 线程再发给前端
    case WM_WORKER_RESULT:
    {
        HelloSetBusy(false);
        g_steamBusy = false;
        std::string* payload = reinterpret_cast<std::string*>(lParam);
        if (payload) {
            // 窗口过程里不能让异常逃出去
            try {
                if (webview) {
                    std::wstring wResponse = StringToWString(*payload);
                    webview->PostWebMessageAsJson(wResponse.c_str());

                    // Windows Hello 的系统验证弹窗关闭后，窗口不一定重新获得键盘焦点。
                    // 主动夺回前台与键盘焦点，并让 WebView2 把焦点移进网页内容；
                    // 否则前端只能改 document.activeElement，用户打字仍进不去。
                    if (payload->find("hello_") != std::string::npos) {
                        SetForegroundWindow(hWnd);
                        SetFocus(hWnd);
                        if (webviewController) {
                            webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                        }
                    }
                }
            }
            catch (...) {
                OutputDebugStringW(L"[Worker] failed to deliver result\n");
            }
            delete payload;
        }
        return 0;
    }
    // 处理窗口销毁消息
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
