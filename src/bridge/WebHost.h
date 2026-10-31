#pragma once

#include <windows.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include "../core/Scanner.h"
#include "../core/Exporter.h"

// Forward declare WebView2 COM interfaces
struct ICoreWebView2Environment;
struct ICoreWebView2Controller;
struct ICoreWebView2;

namespace ReelKill {

class WebHost {
public:
    WebHost();
    ~WebHost();

    // Inisialisasi window host dan WebView2 runtime
    bool Initialize(HINSTANCE hInstance, int nCmdShow, int width = 1280, int height = 760);

    // Jalankan Win32 message loop
    int RunMessageLoop();

    // Kirim pesan JSON ke UI (thread-safe, diposting ke UI thread)
    void SendJsonToUI(const std::string& jsonString);

    // Helper kirim pesan terstruktur ke UI
    void SendLog(const std::string& line);
    void SendProgress(double percent, int64_t frame, int64_t total, double fps, int etaSec);
    void SendClipFound(double start, double end, double score);
    void SendScanDone(bool stopped);
    void SendFileSelected(const std::string& target, const std::string& path);
    void SendToast(const std::string& message);
    void SendError(const std::string& message);
    void SendFrameInfo(int width, int height, const std::string& previewUrl = "");
    void SendResultsLoaded(const std::string& videoUrl, double duration,
                           const std::vector<ExportClip>& clips);

    HWND GetHwnd() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnResize();
    void InitWebView2();

    // Dipanggil saat ICoreWebView2 siap
    void OnWebViewReady();

    // Dispatcher pesan JSON v1 dari UI
    void HandleWebMessage(const std::string& json);

    // Handler pesan individual UI -> C++
    void HandlePickFile(const std::string& target);
    void HandlePickFolder(const std::string& target);
    void HandleStartScan(const std::string& video, const std::string& output,
                         const std::string& player, double pre, double post,
                         int roiX, int roiY, int roiW, int roiH,
                         const std::string& roiPreset);
    void HandleStopScan();
    void HandleLoadResults();
    void HandleExportClips(const std::string& json);
    void HandleShowInExplorer();
    void HandleSaveRoiPreset(const std::string& json);
    void HandleDeleteRoiPreset(const std::string& json);
    void HandleExtractFrameInfo(const std::string& videoPath);

    // Mapping virtual host media.reelkill secara dinamis ke folder video
    void MapMediaFolder(const std::wstring& folderPath);

private:
    HWND m_hWnd;
    HINSTANCE m_hInstance;
    ICoreWebView2Environment* m_env;
    ICoreWebView2Controller* m_controller;
    ICoreWebView2* m_webview;
    bool m_isWebViewReady;

    std::wstring m_currentMediaFolder;
    std::string m_currentLoadedVideo;
    std::string m_lastExportPath;
    std::string m_outputFolder;

    // Backend Core Engine
    Scanner m_scanner;
    Exporter m_exporter;
};

} // namespace ReelKill
