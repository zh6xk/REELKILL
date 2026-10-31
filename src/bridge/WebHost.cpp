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
        L"REELKILL v2.0 - Esports AI Cut",
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
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
            lpMMI->ptMinTrackSize.x = 960;
            lpMMI->ptMinTrackSize.y = 600;
            return 0;
        }
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

void WebHost::SendFrameInfo(int width, int height, const std::string& previewUrl) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"frameInfo\",\"payload\":{"
       << "\"width\":" << width << ","
       << "\"height\":" << height;
    if (!previewUrl.empty()) {
        ss << ",\"previewUrl\":\"" << EscapeJsonString(previewUrl) << "\"";
    }
    ss << "}}";
    SendJsonToUI(ss.str());
}

void WebHost::SendResultsLoaded(const std::string& videoUrl, double duration,
                                const std::vector<ExportClip>& clips) {
    std::ostringstream ss;
    ss << "{\"v\":1,\"type\":\"resultsLoaded\",\"payload\":{"
       << "\"videoUrl\":\"" << EscapeJsonString(videoUrl) << "\","
       << "\"duration\":" << std::fixed << std::setprecision(2) << duration << ","
       << "\"clips\":[";
    for (size_t i = 0; i < clips.size(); i++) {
        if (i > 0) ss << ",";
        ss << "{"
           << "\"start\":" << std::fixed << std::setprecision(2) << clips[i].start << ","
           << "\"end\":" << std::fixed << std::setprecision(2) << clips[i].end << ","
           << "\"label\":\"" << EscapeJsonString(clips[i].label) << "\","
           << "\"score\":0.9"
           << "}";
    }
    ss << "]}}";
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
    } else if (type == "loadResults") {
        HandleLoadResults();
    } else if (type == "exportClips") {
        HandleExportClips(json);
    } else if (type == "showInExplorer") {
        HandleShowInExplorer();
    } else if (type == "saveRoiPreset") {
        HandleSaveRoiPreset(json);
    } else if (type == "deleteRoiPreset") {
        HandleDeleteRoiPreset(json);
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
        m_currentLoadedVideo = filePath;
        SendFileSelected(target, filePath);
        if (target == "video") {
            HandleExtractFrameInfo(filePath);
        }
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
    params.outputDir = outDir;
    params.playerName = player;
    params.preSec = pre;
    params.postSec = post;
    params.minSimilarity = 0.45;
    params.relocateResults = false; // jangan pindahkan/hapus video asli; path video tetap valid untuk tab Hasil Scan
    m_outputFolder = outDir;
    if (roiW > 0 && roiH > 0) {
        params.roi.x = roiX;
        params.roi.y = roiY;
        params.roi.width = roiW;
        params.roi.height = roiH;
        params.hasCustomRoi = true;
    }

    ScanCallbacks callbacks;
    callbacks.onProgress = [this](int percent, int frame, int total, double fps, int etaSec) {
        SendProgress(percent, frame, total, fps, etaSec);
    };

    callbacks.onLog = [this](const std::string& line) {
        SendLog(line);
    };

    callbacks.onClip = [this](const FoundClip& clip) {
        SendClipFound(clip.start, clip.end, clip.score);
    };

    callbacks.onDone = [this](bool stopped, const std::vector<FoundClip>&) {
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

void WebHost::HandleLoadResults() {
    // 1. Dialog pilih file video
    char szVideo[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrTitle = "Pilih Video Rekaman";
    ofn.lpstrFilter = "Video Files (*.mp4;*.mkv)\0*.mp4;*.mkv\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = szVideo;
    ofn.nMaxFile = sizeof(szVideo);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn)) return;

    std::string videoPath(szVideo);
    fs::path vp(Utf8ToUtf16(videoPath));

    // 2. Cari file CSV pendamping (default: nama_video.csv)
    fs::path expectedCsv = vp;
    expectedCsv.replace_extension(".csv");

    std::string csvPath;
    if (fs::exists(expectedCsv)) {
        csvPath = Utf16ToUtf8(expectedCsv.wstring());
    } else {
        // Tampilkan dialog buka CSV jika tidak ditemukan otomatis
        char szCsv[MAX_PATH] = { 0 };
        OPENFILENAMEA ofnCsv = { 0 };
        ofnCsv.lStructSize = sizeof(ofnCsv);
        ofnCsv.hwndOwner = m_hWnd;
        ofnCsv.lpstrTitle = "Pilih File Hasil Scan (.csv)";
        ofnCsv.lpstrFilter = "CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
        ofnCsv.lpstrFile = szCsv;
        ofnCsv.nMaxFile = sizeof(szCsv);
        ofnCsv.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameA(&ofnCsv)) {
            csvPath = std::string(szCsv);
        }
    }

    if (csvPath.empty() || !fs::exists(Utf8ToUtf16(csvPath))) {
        SendError("File CSV hasil scan tidak ditemukan.");
        return;
    }

    // 3. Baca dan parse CSV
    std::vector<ExportClip> clips;
    std::ifstream file(Utf8ToUtf16(csvPath));
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();
        std::stringstream ss(line);
        std::string startStr, endStr, labelStr;
        if (std::getline(ss, startStr, ',') && std::getline(ss, endStr, ',')) {
            std::getline(ss, labelStr);
            if (!labelStr.empty() && labelStr.front() == '"') labelStr.erase(0, 1);
            if (!labelStr.empty() && labelStr.back() == '"') labelStr.pop_back();
            if (labelStr.empty()) labelStr = "kill";

            double s = 0.0, e = 0.0;
            try { s = std::stod(startStr); } catch (...) { s = 0.0; }
            try { e = std::stod(endStr); } catch (...) { e = 0.0; }
            if (e < s) e = s;

            clips.push_back({ s, e, labelStr });
        }
    }
    file.close();

    // 4. Hitung durasi video via OpenCV
    double duration = 0.0;
    try {
        cv::VideoCapture cap(videoPath);
        if (cap.isOpened()) {
            double fps = cap.get(cv::CAP_PROP_FPS);
            double totalFrames = cap.get(cv::CAP_PROP_FRAME_COUNT);
            if (fps > 0 && totalFrames > 0) {
                duration = totalFrames / fps;
            }
            cap.release();
        }
    } catch (...) {}

    if (duration <= 0.0 && !clips.empty()) {
        for (const auto& c : clips) {
            if (c.end > duration) duration = c.end;
        }
        duration += 10.0;
    }

    // 5. Map media virtual host ke parent folder
    MapMediaFolder(vp.parent_path().wstring());
    m_currentLoadedVideo = videoPath;

    // URL virtual host media.reelkill
    std::string filename = Utf16ToUtf8(vp.filename().wstring());
    std::string videoUrl = "https://media.reelkill/" + filename;

    SendResultsLoaded(videoUrl, duration, clips);
    SendToast("Berhasil memuat " + std::to_string(clips.size()) + " klip dari CSV");
}

static bool ExtractJsonBool(const std::string& json, const std::string& key, bool defaultVal) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + pattern.length());
    if (pos == std::string::npos) return defaultVal;
    pos++;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return defaultVal;
}

void WebHost::HandleExportClips(const std::string& json) {
    if (m_currentLoadedVideo.empty() || !fs::exists(Utf8ToUtf16(m_currentLoadedVideo))) {
        SendError("Belum ada video yang dimuat untuk diekspor!");
        return;
    }

    // Parse array clips: [{start,end}, ...]
    std::vector<ExportClip> clips;
    size_t pos = json.find("\"clips\"");
    if (pos != std::string::npos) {
        pos = json.find('[', pos);
        if (pos != std::string::npos) {
            size_t endArr = json.find(']', pos);
            if (endArr != std::string::npos) {
                std::string arrContent = json.substr(pos + 1, endArr - pos - 1);
                size_t objPos = 0;
                while ((objPos = arrContent.find('{', objPos)) != std::string::npos) {
                    size_t objEnd = arrContent.find('}', objPos);
                    if (objEnd == std::string::npos) break;
                    std::string objStr = arrContent.substr(objPos, objEnd - objPos + 1);

                    double s = ExtractJsonNumber(objStr, "start", 0.0);
                    double e = ExtractJsonNumber(objStr, "end", 0.0);
                    std::string label = ExtractJsonString(objStr, "label");
                    if (label.empty()) label = "kill";

                    if (e > s) {
                        clips.push_back({ s, e, label });
                    }
                    objPos = objEnd + 1;
                }
            }
        }
    }

    if (clips.empty()) {
        SendError("Tidak ada klip valid yang dipilih untuk diekspor!");
        return;
    }

    fs::path vp(Utf8ToUtf16(m_currentLoadedVideo));
    std::string outFolder = Utf16ToUtf8(vp.parent_path().wstring());
    if (!m_outputFolder.empty() && fs::exists(Utf8ToUtf16(m_outputFolder))) outFolder = m_outputFolder;
    m_lastExportPath = outFolder;
    bool mergeClips = ExtractJsonBool(json, "merge", true);

    SendToast("Memulai ekspor " + std::to_string(clips.size()) + " klip...");

    ExportCallbacks cb;
    cb.onProgress = [this](int current, int total, const std::string& status) {
        SendToast("Mengekspor: " + std::to_string(current) + " / " + std::to_string(total) + " (" + status + ")");
    };
    cb.onLog = [this](const std::string& line) {
        SendLog(line);
    };
    cb.onError = [this](const std::string& err) {
        SendError(err);
    };
    cb.onDone = [this](bool success, const std::string& msg) {
        if (success) {
            SendToast(msg);
        } else {
            SendError("Ekspor gagal: " + msg);
        }
    };

    // ExportBatch sudah berjalan async di thread miliknya sendiri (tanpa detach)
    if (!m_exporter.ExportBatch(m_currentLoadedVideo, outFolder, clips, mergeClips, cb)) {
        SendError("Ekspor sedang berjalan atau tidak bisa dimulai.");
    }
}

void WebHost::HandleShowInExplorer() {
    std::string target = m_lastExportPath;
    if (target.empty() && !m_currentLoadedVideo.empty()) {
        fs::path vp(Utf8ToUtf16(m_currentLoadedVideo));
        target = Utf16ToUtf8(vp.parent_path().wstring());
    }

    if (!target.empty() && fs::exists(Utf8ToUtf16(target))) {
        ShellExecuteA(NULL, "open", "explorer.exe", ("/select,\"" + target + "\"").c_str(), NULL, SW_SHOW);
    } else {
        SendError("Folder tujuan belum tersedia.");
    }
}

void WebHost::HandleExtractFrameInfo(const std::string& videoPath) {
    if (videoPath.empty() || !fs::exists(Utf8ToUtf16(videoPath))) return;

    try {
        cv::VideoCapture cap(videoPath);
        if (cap.isOpened()) {
            int w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
            int h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);

            std::string previewUrl = "";
            cv::Mat frame;
            // Baca frame pertama atau frame detik ke-1 untuk preview
            if (cap.read(frame) && !frame.empty()) {
                // Resize preview jika terlalu besar (maks width 1280)
                if (frame.cols > 1280) {
                    double scale = 1280.0 / frame.cols;
                    cv::resize(frame, frame, cv::Size(), scale, scale);
                }
                std::vector<uchar> buf;
                std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 80 };
                if (cv::imencode(".jpg", frame, buf, params)) {
                    static const char b64table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::string b64 = "";
                    int val = 0, valb = -6;
                    for (uchar c : buf) {
                        val = (val << 8) + c;
                        valb += 8;
                        while (valb >= 0) {
                            b64.push_back(b64table[(val >> valb) & 0x3F]);
                            valb -= 6;
                        }
                    }
                    if (valb > -6) b64.push_back(b64table[((val << 8) >> (valb + 8)) & 0x3F]);
                    while (b64.size() % 4) b64.push_back('=');
                    previewUrl = "data:image/jpeg;base64," + b64;
                }
            }
            cap.release();
            SendFrameInfo(w, h, previewUrl);
        }
    } catch (...) {}
}

void WebHost::HandleSaveRoiPreset(const std::string& json) {
    std::string name = ExtractJsonString(json, "name");
    if (name.empty()) return;

    int x = (int)ExtractJsonNumber(json, "x", 0);
    int y = (int)ExtractJsonNumber(json, "y", 0);
    int w = (int)ExtractJsonNumber(json, "w", 0);
    int h = (int)ExtractJsonNumber(json, "h", 0);

    // Baca preset lama dari file JSON
    std::string exeDir = GetExeDirW();
    fs::path presetJsonPath = fs::path(exeDir) / "roi_presets.json";

    // Format simpan JSON sederhana
    std::vector<std::string> entries;
    if (fs::exists(presetJsonPath)) {
        std::ifstream f(presetJsonPath);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("\"name\":\"" + name + "\"") == std::string::npos && line.find('{') != std::string::npos) {
                entries.push_back(line);
            }
        }
        f.close();
    }

    std::ostringstream ss;
    ss << "  {\"name\":\"" << EscapeJsonString(name) << "\",\"x\":" << x << ",\"y\":" << y << ",\"w\":" << w << ",\"h\":" << h << "}";
    entries.push_back(ss.str());

    std::ofstream out(presetJsonPath);
    out << "[\n";
    for (size_t i = 0; i < entries.size(); i++) {
        out << entries[i] << (i + 1 < entries.size() ? ",\n" : "\n");
    }
    out << "]\n";
    out.close();

    SendToast("Preset ROI '" + name + "' berhasil disimpan.");
}

void WebHost::HandleDeleteRoiPreset(const std::string& json) {
    std::string name = ExtractJsonString(json, "name");
    if (name.empty()) return;

    std::string exeDir = GetExeDirW();
    fs::path presetJsonPath = fs::path(exeDir) / "roi_presets.json";
    if (!fs::exists(presetJsonPath)) return;

    std::vector<std::string> entries;
    std::ifstream f(presetJsonPath);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("\"name\":\"" + name + "\"") == std::string::npos && line.find('{') != std::string::npos) {
            entries.push_back(line);
        }
    }
    f.close();

    std::ofstream out(presetJsonPath);
    out << "[\n";
    for (size_t i = 0; i < entries.size(); i++) {
        out << entries[i] << (i + 1 < entries.size() ? ",\n" : "\n");
    }
    out << "]\n";
    out.close();

    SendToast("Preset ROI '" + name + "' berhasil dihapus.");
}

} // namespace ReelKill
