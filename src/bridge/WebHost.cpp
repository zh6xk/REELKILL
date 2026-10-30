#include "WebHost.h"
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>

// WebView2 header
#include <webview2/build/native/include/WebView2.h>

namespace fs = std::filesystem;

namespace ReelKill {

// Custom window message untuk thread-safe marshaling ke UI thread
#define WM_USER_WEB_POST_JSON (WM_USER + 201)
#define WM_USER_CALL_CALLBACK (WM_USER + 202)

// =========================================================================
// Light-weight COM Callback Handler Templates (Tanpa butuh WRL / wil)
// =========================================================================

template <typename TInterface, typename TFunc>
class ComCallback : public TInterface {
public:
    ComCallback(TFunc func) : m_func(func), m_refCount(1) {}
    virtual ~ComCallback() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(TInterface)) {
            *ppv = static_cast<TInterface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    template <typename... Args>
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) {
        return m_func(args...);
    }

private:
    TFunc m_func;
    long m_refCount;
};

// =========================================================================
// String & JSON Helpers
// =========================================================================

static std::wstring Utf8ToUtf16(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

static std::string Utf16ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], sizeNeeded, NULL, NULL);
    return str;
}

static std::string EscapeJsonString(const std::string& str) {
    std::ostringstream ss;
    for (char c : str) {
        switch (c) {
        case '"':  ss << "\\\""; break;
        case '\\': ss << "\\\\"; break;
        case '\b': ss << "\\b";  break;
        case '\f': ss << "\\f";  break;
        case '\n': ss << "\\n";  break;
        case '\r': ss << "\\r";  break;
        case '\t': ss << "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c;
            } else {
                ss << c;
            }
            break;
        }
    }
    return ss.str();
}

static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos += pattern.length();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t endPos = pos + 1;
    while (endPos < json.length()) {
        if (json[endPos] == '"' && json[endPos - 1] != '\\') break;
        endPos++;
    }
    if (endPos >= json.length()) return "";
    std::string raw = json.substr(pos + 1, endPos - pos - 1);
    std::string res;
    for (size_t i = 0; i < raw.length(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.length()) {
            i++;
            if (raw[i] == '"') res += '"';
            else if (raw[i] == '\\') res += '\\';
            else if (raw[i] == 'n') res += '\n';
            else if (raw[i] == 't') res += '\t';
            else res += raw[i];
        } else {
            res += raw[i];
        }
    }
    return res;
}

static double ExtractJsonNumber(const std::string& json, const std::string& key, double defaultVal = 0.0) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return defaultVal;
    pos += pattern.length();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return defaultVal;
    pos++;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) pos++;
    size_t endPos = pos;
    while (endPos < json.length() && (isdigit(json[endPos]) || json[endPos] == '.' || json[endPos] == '-' || json[endPos] == '+')) endPos++;
    if (endPos == pos) return defaultVal;
    try {
        return std::stod(json.substr(pos, endPos - pos));
    } catch (...) {
        return defaultVal;
    }
}

static std::string GetExeDirW() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    fs::path p(path);
    return p.parent_path().string();
}

// =========================================================================
// WebHost Implementation
// =========================================================================

WebHost::WebHost()
    : m_hWnd(NULL), m_hInstance(NULL), m_env(nullptr),
      m_controller(nullptr), m_webview(nullptr), m_isWebViewReady(false) {}

WebHost::~WebHost() {
    if (m_webview) { m_webview->Release(); m_webview = nullptr; }
    if (m_controller) { m_controller->Release(); m_controller = nullptr; }
    if (m_env) { m_env->Release(); m_env = nullptr; }
}

bool WebHost::Initialize(HINSTANCE hInstance, int nCmdShow, int width, int height) {
    m_hInstance = hInstance;

    WNDCLASSEXW wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WebHost::WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"ReelKillWebView2HostWindow";

    HICON hAppIcon = (HICON)LoadImageA(NULL, "app.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hAppIcon) hAppIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hIcon = hAppIcon;
    wcex.hIconSm = hAppIcon;

    RegisterClassExW(&wcex);

    m_hWnd = CreateWindowExW(
        0, L"ReelKillWebView2HostWindow",
        L"REELKILL - Esports AI Cut",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        NULL, NULL, hInstance, this
    );

    if (!m_hWnd) return false;

    ShowWindow(m_hWnd, nCmdShow);
    UpdateWindow(m_hWnd);

    InitWebView2();
    return true;
}

int WebHost::RunMessageLoop() {
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK WebHost::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    WebHost* self = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        self = (WebHost*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (WebHost*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    if (self) {
        switch (uMsg) {
        case WM_SIZE:
            self->OnResize();
            return 0;
        case WM_USER_WEB_POST_JSON: {
            std::string* pJson = (std::string*)lParam;
            if (pJson) {
                if (self->m_webview && self->m_isWebViewReady) {
                    std::wstring wJson = Utf8ToUtf16(*pJson);
                    self->m_webview->PostWebMessageAsJson(wJson.c_str());
                }
                delete pJson;
            }
            return 0;
        }
        case WM_CLOSE:
            self->m_scanner.Stop();
            self->m_exporter.Cancel();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void WebHost::OnResize() {
    if (m_controller) {
        RECT bounds;
        GetClientRect(m_hWnd, &bounds);
        m_controller->put_Bounds(bounds);
    }
}

void WebHost::InitWebView2() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    fs::path p(exePath);
    std::wstring userDataFolder = (p.parent_path() / L"webview2_data").wstring();

    auto envHandler = new ComCallback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        std::function<HRESULT(HRESULT, ICoreWebView2Environment*)>>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxW(m_hWnd, L"Gagal membuat environment WebView2. Pastikan Microsoft Edge WebView2 Runtime terpasang.", L"Error", MB_ICONERROR);
                    return result;
                }
                m_env = env;
                m_env->AddRef();

                auto controllerHandler = new ComCallback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                    std::function<HRESULT(HRESULT, ICoreWebView2Controller*)>>(
                        [this](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) {
                                MessageBoxW(m_hWnd, L"Gagal membuat WebView2 controller.", L"Error", MB_ICONERROR);
                                return res;
                            }
                            m_controller = controller;
                            m_controller->AddRef();

                            m_controller->get_CoreWebView2(&m_webview);
                            if (m_webview) {
                                m_webview->AddRef();
                                OnWebViewReady();
                            }
                            return S_OK;
                        });

                m_env->CreateCoreWebView2Controller(m_hWnd, controllerHandler);
                return S_OK;
            });

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr, envHandler);
    if (FAILED(hr)) {
        MessageBoxW(m_hWnd, L"WebView2 Runtime tidak terdeteksi.", L"Error", MB_ICONERROR);
    }
}

void WebHost::OnWebViewReady() {
    if (!m_webview || !m_controller) return;

    // Resize controller ke ukuran client area
    OnResize();
    m_controller->put_IsVisible(TRUE);

    // Dapatkan path folder exe dan folder ui
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path uiFolder = exeDir / L"ui";

    // Cast ke ICoreWebView2_3 untuk SetVirtualHostNameToFolderMapping
    ICoreWebView2_3* webview3 = nullptr;
    HRESULT hr = m_webview->QueryInterface(IID_ICoreWebView2_3, (void**)&webview3);
    if (SUCCEEDED(hr) && webview3) {
        webview3->SetVirtualHostNameToFolderMapping(
            L"app.reelkill",
            uiFolder.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
        );
        webview3->Release();
    }

    // Pasang WebMessageReceived handler
    EventRegistrationToken token;
    auto msgHandler = new ComCallback<ICoreWebView2WebMessageReceivedEventHandler,
        std::function<HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR pJson = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&pJson)) && pJson) {
                    std::string jsonStr = Utf16ToUtf8(pJson);
                    CoTaskMemFree(pJson);
                    HandleWebMessage(jsonStr);
                }
                return S_OK;
            });

    m_webview->add_WebMessageReceived(msgHandler, &token);

    m_isWebViewReady = true;

    // Navigasi ke URL aplikasi virtual
    m_webview->Navigate(L"https://app.reelkill/index.html");
}

void WebHost::MapMediaFolder(const std::wstring& folderPath) {
    if (!m_webview) return;
    if (m_currentMediaFolder == folderPath) return;

    ICoreWebView2_3* webview3 = nullptr;
    if (SUCCEEDED(m_webview->QueryInterface(IID_ICoreWebView2_3, (void**)&webview3)) && webview3) {
        webview3->SetVirtualHostNameToFolderMapping(
            L"media.reelkill",
            folderPath.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
        );
        webview3->Release();
        m_currentMediaFolder = folderPath;
    }
}

void WebHost::SendJsonToUI(const std::string& jsonString) {
    if (!m_hWnd) return;
    std::string* pCopy = new std::string(jsonString);
    PostMessage(m_hWnd, WM_USER_WEB_POST_JSON, 0, (LPARAM)pCopy);
}

void WebHost::SendLog(const std::string& line) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"log\",\"payload\":{\"line\":\"" << EscapeJsonString(line) << "\"}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendProgress(double percent, int64_t frame, int64_t total, double fps, int etaSec) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"progress\",\"payload\":{"
       << "\"percent\":" << std::fixed << std::setprecision(1) << percent << ","
       << "\"frame\":" << frame << ","
       << "\"total\":" << total << ","
       << "\"fps\":" << std::fixed << std::setprecision(1) << fps << ","
       << "\"etaSec\":" << etaSec
       << "}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendClipFound(double start, double end, double score) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"clipFound\",\"payload\":{"
       << "\"start\":" << std::fixed << std::setprecision(2) << start << ","
       << "\"end\":" << std::fixed << std::setprecision(2) << end << ","
       << "\"score\":" << std::fixed << std::setprecision(2) << score
       << "}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendScanDone(bool stopped) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"scanDone\",\"payload\":{"
       << "\"stopped\":" << (stopped ? "true" : "false")
       << "}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendFileSelected(const std::string& target, const std::string& path) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"fileSelected\",\"payload\":{"
       << "\"target\":\"" << EscapeJsonString(target) << "\","
       << "\"path\":\"" << EscapeJsonString(path) << "\""
       << "}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendToast(const std::string& message) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"toast\",\"payload\":{\"message\":\"" << EscapeJsonString(message) << "\"}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendError(const std::string& message) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"error\",\"payload\":{\"message\":\"" << EscapeJsonString(message) << "\"}}";
    SendJsonToUI(ss.str());
}

void WebHost::HandleWebMessage(const std::string& json) {
    std::string type = ExtractJsonString(json, "type");
    if (type.empty()) return;

    if (type == "pickFile") {
        std::string target = ExtractJsonString(json, "target");
        HandlePickFile(target.empty() ? "video" : target);
    } else if (type == "pickFolder") {
        std::string target = ExtractJsonString(json, "target");
        HandlePickFolder(target.empty() ? "output" : target);
    } else if (type == "startScan") {
        std::string video = ExtractJsonString(json, "video");
        std::string output = ExtractJsonString(json, "output");
        std::string player = ExtractJsonString(json, "player");
        double pre = ExtractJsonNumber(json, "pre", 4.0);
        double post = ExtractJsonNumber(json, "post", 3.0);
        int roiX = (int)ExtractJsonNumber(json, "x", 0);
        int roiY = (int)ExtractJsonNumber(json, "y", 0);
        int roiW = (int)ExtractJsonNumber(json, "w", 0);
        int roiH = (int)ExtractJsonNumber(json, "h", 0);
        std::string roiPreset = ExtractJsonString(json, "roiPreset");

        HandleStartScan(video, output, player, pre, post, roiX, roiY, roiW, roiH, roiPreset);
    } else if (type == "stopScan") {
        HandleStopScan();
    }
}

void WebHost::HandlePickFile(const std::string& target) {
    char szFile[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = "Video Files (*.mp4;*.mkv)\0*.mp4;*.mkv\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        std::string filePath(szFile);
        // Map folder video ke media.reelkill
        fs::path p(Utf8ToUtf16(filePath));
        MapMediaFolder(p.parent_path().wstring());
        SendFileSelected(target, filePath);
    }
}

void WebHost::HandlePickFolder(const std::string& target) {
    char szDir[MAX_PATH] = { 0 };
    BROWSEINFOA bi = { 0 };
    bi.hwndOwner = m_hWnd;
    bi.lpszTitle = "Pilih Folder Output";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        if (SHGetPathFromIDListA(pidl, szDir)) {
            SendFileSelected(target, std::string(szDir));
        }
        CoTaskMemFree(pidl);
    }
}

void WebHost::HandleStartScan(const std::string& video, const std::string& output,
                             const std::string& player, double pre, double post,
                             int roiX, int roiY, int roiW, int roiH,
                             const std::string& roiPreset) {
    if (video.empty() || player.empty()) {
        SendError("File video dan nama player wajib diisi!");
        return;
    }

    std::string outDir = output;
    if (outDir.empty()) {
        fs::path vp(Utf8ToUtf16(video));
        outDir = Utf16ToUtf8(vp.parent_path().wstring());
    }

    // Map parent folder video ke media.reelkill
    fs::path vp(Utf8ToUtf16(video));
    MapMediaFolder(vp.parent_path().wstring());

    ScanParams params;
    params.videoPath = video;
    params.outDir = outDir;
    params.targetName = player;
    params.preSec = pre;
    params.postSec = post;
    params.minSimilarity = 0.45;
    params.roiX = roiX;
    params.roiY = roiY;
    params.roiW = roiW;
    params.roiH = roiH;
    params.roiPreset = roiPreset;

    ScanCallbacks callbacks;
    callbacks.onProgress = [this](int percent, int64_t frame, int64_t total, double fps, int etaSec) {
        SendProgress(percent, frame, total, fps, etaSec);
    };

    callbacks.onLog = [this](const std::string& line) {
        SendLog(line);
    };

    callbacks.onClip = [this](double start, double end, const std::string& label, double score) {
        SendClipFound(start, end, score);
    };

    callbacks.onDone = [this](bool stopped) {
        SendScanDone(stopped);
    };

    callbacks.onError = [this](const std::string& err) {
        SendError(err);
    };

    m_scanner.Start(params, callbacks);
}

void WebHost::HandleStopScan() {
    m_scanner.Stop();
    SendScanDone(true);
}

} // namespace ReelKill
