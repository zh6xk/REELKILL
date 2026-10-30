/*
 * COMBINED APP: AUTO CUT PRO V48 (FINAL POLISH)
 * * NAVIGATION:
 * 1. DASHBOARD (Auto Cut UI)
 * 2. HASIL SCAN (Player/Editor)
 * 3. PANDUAN & TENTANG
 * * CHANGES V48:
 * - Fixed: Timeline cursor (Red Line) dragging/scrubbing implemented in WM_MOUSEMOVE
 * - Previous Fixes Included: Z-Order, Ghosting, ClipSiblings
 */

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h> 
#include <shlobj.h> 
#include <shellapi.h> 
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <algorithm> 
#include <chrono>
#include <cmath> 
#include <sstream>
#include <iomanip>

 // --- INCLUDES LIBRARY ---
#include "include/OcrLiteCApi.h" 
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <mpv/client.h> // Header MPV

using namespace std;
using namespace cv;
using namespace std::chrono;
namespace fs = std::filesystem;

// =========================================================
// GLOBAL VARIABLES & DEFINITIONS
// =========================================================

HWND hMainFrame;
HWND hSidebar;
HWND hContentArea;

// Views (Panel)
HWND hViewAutoCut;
HWND hViewLossCut;
HWND hViewGuide;
HWND hViewAbout;
HWND hLblPercent, hLblEta;

HINSTANCE hInst;
int currentViewID = -1;

// --- PALETTE WARNA ---
#define COL_BG_SIDEBAR   RGB(24, 25, 28)      
#define COL_BG_MAIN      RGB(35, 39, 42)      
#define COL_BTN_NORMAL   RGB(47, 49, 54)       
#define COL_BTN_HOVER    RGB(66, 70, 77)    
#define COL_BTN_BLUE     RGB(88, 101, 242)     
#define COL_BTN_RED      RGB(237, 66, 69)      
#define COL_INPUT_BG     RGB(30, 31, 34)       
#define COL_TEXT_WHITE   RGB(255, 255, 255)    
#define COL_LOG_PURPLE   RGB(150, 100, 255) 
#define COL_LOG_BG       RGB(15, 15, 18)     

// --- ID CONTROLS ---
#define ID_NAV_AUTOCUT    2001 
#define ID_NAV_LOSSCUT    2002 
#define ID_NAV_GUIDE      2003 
#define ID_NAV_ABOUT      2004 

// AutoCut IDs
#define ID_LBL_PERCENT    302
#define ID_LBL_ETA        303
#define ID_BTN_FILE       101
#define ID_BTN_OUTPUT     102
#define ID_BTN_ROI        103
#define ID_BTN_START      104
#define ID_BTN_CANCEL     106
#define ID_BTN_SAVE_PRESET 107
#define ID_BTN_DEL_PRESET 108
#define ID_EDIT_FILE      201
#define ID_EDIT_OUTPUT    205
#define ID_EDIT_NAME      202
#define ID_EDIT_LOG       203
#define ID_EDIT_PRESET    204 
#define ID_EDIT_PRE       206 
#define ID_EDIT_POST      207 
#define ID_SLIDER_THRESH  208
#define ID_LBL_THRESH_VAL 304
#define ID_PROGRESS       301
#define ID_COMBO_PRESET   401 
#define ID_STATIC_PREVIEW 601 

// LossCut IDs
#define ID_LC_LISTBOX     3101
#define ID_LC_BTN_EXPORT  3102
#define ID_LC_BTN_SHOW    3103
#define ID_LC_BTN_FOLDER  3104

// =========================================================
// BAGIAN 1: CODE PERTAMA (AUTO CUT LOGIC)
// =========================================================

const string PLAYER_PLACEHOLDER = "Masukkan nama player...";

struct RoiPreset {
    string name;
    int x, y, w, h;
    int baseW = 0;
    int baseH = 0;
};
struct CutSegment { double start; double end; string label; };
OCR_HANDLE hOcrEngine = NULL;
atomic<bool> isRunning(false);
atomic<bool> stopRequested(false);
vector<RoiPreset> g_presets;
Rect g_roiRect(0, 0, 0, 0);
bool g_roiSet = false;

// UI Handles AutoCut
HWND hLbl1, hLbl2, hLbl3, hLbl4, hLbl5, hLbl6, hLbl7, hLbl8;
HWND hEditFile, hEditOutput, hEditName, hEditLog, hProgress, hBtnStart, hBtnCancel;
HWND hComboPreset, hEditPresetName, hEditPre, hEditPost, hPreviewBox;
HWND hBtnFile, hBtnOutput, hBtnRoi, hBtnSave, hBtnDel;
HWND hSliderThresh, hLblThresh, hLblThreshVal;
int g_similarityThreshold = 50;

// --- UTILS ---
HFONT CreateSFPro(int size, int weight = FW_NORMAL) {
    return CreateFontA(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "SF Pro Display");
}

string GetExeDir() { char b[MAX_PATH]; GetModuleFileNameA(NULL, b, MAX_PATH); fs::path p(b); return p.parent_path().string(); }
string FormatTimeSafe(int s) {
    if (s < 0)s = 0; int h = s / 3600; int m = (s % 3600) / 60; int sc = s % 60;
    char b[128]; sprintf_s(b, 128, "%02d:%02d:%02d", h, m, sc); return string(b);
}
string NormalizeForMatch(string s) { string out = ""; for (char c : s) { char l = tolower(c); if (isalnum(l) || l == ' ')out += l; } return out; }

int LevenshteinDist(const string& s1, const string& s2) {
    int m = (int)s1.length(); int n = (int)s2.length();
    vector<vector<int>> dp(m + 1, vector<int>(n + 1));
    for (int i = 0; i <= m; i++)dp[i][0] = i; for (int j = 0; j <= n; j++)dp[0][j] = j;
    for (int i = 1; i <= m; i++) { for (int j = 1; j <= n; j++) { if (s1[i - 1] == s2[j - 1])dp[i][j] = dp[i - 1][j - 1]; else dp[i][j] = 1 + min({ dp[i - 1][j],dp[i][j - 1],dp[i - 1][j - 1] }); } }
    return dp[m][n];
}
double GetBestSimilarity(string text, string pattern) {
    if (pattern.empty() || pattern == PLAYER_PLACEHOLDER) return 0.0;
    string sT = text; string sP = pattern;
    if (sT.length() < sP.length()) { int dist = LevenshteinDist(sT, sP); return 1.0 - (double)dist / max(sT.length(), sP.length()); }
    double maxScore = 0.0; int pL = (int)sP.length();
    for (int i = 0; i <= sT.length() - pL; i++) { string sub = sT.substr(i, pL); int dist = LevenshteinDist(sub, sP); double sc = 1.0 - ((double)dist / pL); if (sc > maxScore)maxScore = sc; }
    return maxScore;
}

void LogToUI(const string& msg) {
    if (!hEditLog) return;
    string f = msg + "\r\n"; int len = GetWindowTextLengthA(hEditLog);
    SendMessageA(hEditLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(hEditLog, EM_REPLACESEL, 0, (LPARAM)f.c_str());
}

// =========================================================
// FUNGSI GAMBAR FINAL V2: ANTI-GEPENG & ANTI-MIRING (STRIDE FIX)
// Timpa fungsi DrawMatToHwnd yang lama dengan ini
// =========================================================
void DrawMatToHwnd(HWND hWnd, const cv::Mat& imgSrc) {
    if (imgSrc.empty() || !IsWindow(hWnd)) return;

    // --- STEP 1: FIX "MIRING" (BYTE ALIGNMENT) ---
    // Windows GDI mewajibkan setiap baris data harus kelipatan 4 byte.
    // Jika tidak, gambar akan terlihat miring/sheared.
    cv::Mat img;
    int srcW = imgSrc.cols;
    int srcH = imgSrc.rows;
    int channels = imgSrc.channels();

    // Hitung stride (lebar baris) yang dibutuhkan Windows (harus align 4 byte)
    int stepBytes = (srcW * channels + 3) & -4;

    // Jika stride OpenCV beda dengan stride Windows, kita harus padding
    if (imgSrc.step != stepBytes) {
        // Buat mat baru dengan stride yang sesuai
        // Kita gunakan copyMakeBorder untuk padding kanan jika perlu, 
        // tapi cara paling aman adalah clone dengan lebar yang pas.
        // Cara simpel: Convert ke format yang pasti aman (misal BGR 24-bit biasanya aman jika width genap)
        // Tapi untuk performa, kita copy manual atau biarkan OpenCV handle padding

        // Solusi paling stabil: Pastikan lebar adalah kelipatan 4 untuk Grayscale
        if (channels == 1 && (srcW % 4 != 0)) {
            int pad = 4 - (srcW % 4);
            cv::copyMakeBorder(imgSrc, img, 0, 0, 0, pad, cv::BORDER_CONSTANT, cv::Scalar(0));
            // Update lebar sumber (img sekarang lebih lebar sedikit karena padding hitam di kanan)
            // Tapi kita tetap display sesuai srcW asli di StretchDIBits nanti
        }
        else {
            img = imgSrc;
        }
    }
    else {
        img = imgSrc;
    }

    // Update pointer data & ukuran buffer yang akan dipakai
    int bufW = img.cols;
    int bufH = img.rows;

    // --- STEP 2: SIAPKAN KANVAS ---
    RECT rect;
    if (!GetClientRect(hWnd, &rect)) return;
    int destW = rect.right - rect.left;
    int destH = rect.bottom - rect.top;
    if (destW <= 0 || destH <= 0) return;

    HDC hdc = GetDC(hWnd);
    if (!hdc) return;

    // Bersihkan Background jadi Hitam (Letterbox)
    HBRUSH hBrBG = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rect, hBrBG);
    DeleteObject(hBrBG);

    // --- STEP 3: HITUNG SKALA (ASPEK RASIO / ANTI-GEPENG) ---
    // Kita gunakan srcW/srcH asli (bukan yang dipadding) untuk rasio
    float scaleX = (float)destW / srcW;
    float scaleY = (float)destH / srcH;
    float scale = (scaleX < scaleY) ? scaleX : scaleY; // Ambil skala terkecil

    int finalW = (int)(srcW * scale);
    int finalH = (int)(srcH * scale);

    // Posisi Tengah
    int finalX = (destW - finalW) / 2;
    int finalY = (destH - finalH) / 2;

    // --- STEP 4: HEADER BITMAP ---
    struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[256]; } bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bufW; // Gunakan lebar buffer (yang mungkin sudah dipadding)
    bmi.bmiHeader.biHeight = -bufH; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = (channels == 1) ? 8 : 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    if (channels == 1) {
        for (int i = 0; i < 256; i++) {
            bmi.bmiColors[i].rgbBlue = bmi.bmiColors[i].rgbGreen = bmi.bmiColors[i].rgbRed = (BYTE)i;
        }
    }

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);

    // --- STEP 5: GAMBAR ---
    // Perhatikan: srcWidth yang diambil dari buffer adalah srcW asli, padding diabaikan saat display
    StretchDIBits(hdc,
        finalX, finalY, finalW, finalH, // Tujuan di layar
        0, 0, srcW, srcH,               // Ambil dari buffer (abaikan padding kanan jika ada)
        img.data, (const BITMAPINFO*)&bmi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(hWnd, hdc);
}

void WorkerThread(string vidPath, string outDir, string targetName, double preSec, double postSec, double minSimilarity) {
    fs::path tempPath;
    try {
        isRunning = true;
        fs::path exeDir(GetExeDir()); fs::path modelsDir = exeDir / "models";
        string det = (modelsDir / "ch_PP-OCRv4_det_infer.onnx").string();
        string cls = (modelsDir / "ch_ppocr_mobile_v2.0_cls_infer.onnx").string();
        string rec = (modelsDir / "ch_PP-OCRv3_rec_infer.onnx").string();
        string keys = (modelsDir / "ppocr_keys_v1.txt").string();
        replace(det.begin(), det.end(), '\\', '/'); replace(cls.begin(), cls.end(), '\\', '/');
        replace(rec.begin(), rec.end(), '\\', '/'); replace(keys.begin(), keys.end(), '\\', '/');

        if (!fs::exists(det)) { LogToUI("[FATAL] OCR Models missing!"); isRunning = false; EnableWindow(hBtnStart, TRUE); return; }
        int maxThreads = std::thread::hardware_concurrency(); if (maxThreads < 2)maxThreads = 4;
        if (hOcrEngine == NULL) hOcrEngine = OcrInit(det.c_str(), cls.c_str(), rec.c_str(), keys.c_str(), maxThreads);

        VideoCapture cap(vidPath);
        if (!cap.isOpened()) { LogToUI("[ERROR] Gagal baca video."); isRunning = false; EnableWindow(hBtnStart, TRUE); return; }

        int totalFrames = (int)cap.get(CAP_PROP_FRAME_COUNT);
        double fps = cap.get(CAP_PROP_FPS); if (fps <= 0) fps = 30.0;
        int vW = (int)cap.get(CAP_PROP_FRAME_WIDTH); int vH = (int)cap.get(CAP_PROP_FRAME_HEIGHT);
        Rect activeRoi = g_roiRect;
        int curSel = (int)SendMessageA(hComboPreset, CB_GETCURSEL, 0, 0);
        if (curSel >= 0 && curSel < (int)g_presets.size()) {
            const auto& pr = g_presets[curSel];
            if (pr.baseW > 0 && pr.baseH > 0 && (pr.baseW != vW || pr.baseH != vH)) {
                double sx = (double)vW / pr.baseW;
                double sy = (double)vH / pr.baseH;
                activeRoi.x = (int)(pr.x * sx);
                activeRoi.y = (int)(pr.y * sy);
                activeRoi.width = (int)(pr.w * sx);
                activeRoi.height = (int)(pr.h * sy);
                LogToUI("[INFO] Preset di-scale: " + to_string(pr.baseW) + "x" + to_string(pr.baseH) + " -> " + to_string(vW) + "x" + to_string(vH));
            }
        }
        if (!g_roiSet || activeRoi.width <= 0 || activeRoi.height <= 0) {
            activeRoi = Rect((int)(vW * 0.64), (int)(vH * 0.39), (int)(vW * 0.15), (int)(vH * 0.13));
        }
        if (activeRoi.x < 0) activeRoi.x = 0;
        if (activeRoi.y < 0) activeRoi.y = 0;
        if (activeRoi.x + activeRoi.width > vW) activeRoi.width = vW - activeRoi.x;
        if (activeRoi.y + activeRoi.height > vH) activeRoi.height = vH - activeRoi.y;

        // Optimized OCR_PARAM: doAngle=0, mostAngle=0 (Skip angle classifier for horizontal kill feed text)
        OCR_PARAM param = { 50, 1024, 0.5f, 0.3f, 1.6f, 0, 0 };
        std::error_code ecTemp;
        fs::path tempDir = fs::temp_directory_path(ecTemp);
        if (ecTemp || !fs::exists(tempDir)) {
            tempDir = exeDir;
        }
        tempPath = tempDir / ("reelkill_temp_" + to_string(GetCurrentProcessId()) + ".bmp");
        string fullPath = tempPath.string();
        replace(fullPath.begin(), fullPath.end(), '\\', '/');
        string safeTarget = NormalizeForMatch(targetName);
        LogToUI("[START] Analisis dimulai...");

        double checkInterval = 0.35; int frameStep = (int)(fps * checkInterval); if (frameStep < 1) frameStep = 1;
        vector<CutSegment> segments; Mat frame, crop, processedCrop, grayCrop, prevGrayCrop, diffMat;
        int processedFrames = 0; int hitCount = 0;
        auto startTime = high_resolution_clock::now(); double avgTimePerStep = 0.0;

        while (processedFrames < totalFrames) {
            if (stopRequested) break;
            auto stepStart = high_resolution_clock::now();
            bool ok = cap.read(frame); if (!ok) break;
            for (int k = 0; k < frameStep - 1; k++) { cap.grab(); processedFrames++; }
            processedFrames++;

            if (activeRoi.x + activeRoi.width > frame.cols) activeRoi.width = frame.cols - activeRoi.x;
            if (activeRoi.y + activeRoi.height > frame.rows) activeRoi.height = frame.rows - activeRoi.y;

            crop = frame(activeRoi); if (!crop.empty()) {
                // Motion / Pixel-change filter: Skip heavy OCR if ROI scene has not changed
                cvtColor(crop, grayCrop, COLOR_BGR2GRAY);
                bool sceneChanged = true;
                if (!prevGrayCrop.empty() && prevGrayCrop.size() == grayCrop.size()) {
                    absdiff(grayCrop, prevGrayCrop, diffMat);
                    double meanDiff = mean(diffMat)[0];
                    if (meanDiff < 3.5) {
                        sceneChanged = false; // ROI static, kill feed hasn't updated
                    }
                }
                grayCrop.copyTo(prevGrayCrop);

                if (sceneChanged) {
                    // Upscale 2x bicubic for small text clarity, preserve natural color contrast for RapidOcr
                    resize(crop, processedCrop, Size(), 2.0, 2.0, INTER_CUBIC);
                    DrawMatToHwnd(hPreviewBox, processedCrop);

                    if (imwrite(fullPath, processedCrop)) {
                        if (OcrDetect(hOcrEngine, fullPath.c_str(), "", &param)) {
                            int len = OcrGetLen(hOcrEngine);
                            if (len > 0) {
                                vector<char> buf(len + 1); OcrGetResult(hOcrEngine, buf.data(), len + 1);
                                string resRaw = string(buf.data()); string resSafe = NormalizeForMatch(resRaw);
                                double similarity = GetBestSimilarity(resSafe, safeTarget);
                                if (similarity >= minSimilarity) {
                                    double currentTime = processedFrames / fps;
                                    double idealStart = max(0.0, currentTime - preSec); double idealEnd = min((double)totalFrames / fps, currentTime + postSec);
                                    if (!segments.empty() && idealStart <= segments.back().end + 2.0) { segments.back().end = max(segments.back().end, idealEnd); }
                                    else {
                                        hitCount++;
                                        LogToUI("[HIT #" + to_string(hitCount) + "] Kill Detected at " + FormatTimeSafe((int)currentTime) + " | Teks: \"" + resRaw + "\" (Skor: " + to_string((int)(similarity * 100)) + "% >= " + to_string((int)(minSimilarity * 100)) + "%)");
                                        segments.push_back({ idealStart, idealEnd, "kill" });
                                    }
                                }
                                else if (similarity >= 0.25) {
                                    LogToUI("[SCAN] Teks: \"" + resRaw + "\" | Skor: " + to_string((int)(similarity * 100)) + "% (< Min " + to_string((int)(minSimilarity * 100)) + "%)");
                                }
                            }
                        }
                    }
                    else {
                        static bool s_warned = false;
                        if (!s_warned) {
                            LogToUI("[WARN] Gagal menulis temp image ke: " + fullPath);
                            s_warned = true;
                        }
                    }
                }
            }

            auto stepEnd = high_resolution_clock::now(); duration<double> stepDur = stepEnd - stepStart;
            if (avgTimePerStep == 0) avgTimePerStep = stepDur.count(); else avgTimePerStep = (avgTimePerStep * 0.9) + (stepDur.count() * 0.1);

            if (processedFrames % (frameStep * 2) == 0 || processedFrames >= totalFrames) {
                int percent = (int)((float)processedFrames / totalFrames * 100);
                int framesLeft = totalFrames - processedFrames;
                double secondsLeft = (framesLeft / frameStep) * avgTimePerStep;
                SendMessage(hProgress, PBM_SETPOS, percent, 0);
                string pctStr = to_string(percent) + "%";
                string etaStr = "Estimasi: " + FormatTimeSafe((int)secondsLeft);
                SetWindowTextA(hLblPercent, pctStr.c_str());
                SetWindowTextA(hLblEta, etaStr.c_str());
            }
        }
        cap.release();
        std::error_code ecRem;
        if (!tempPath.empty()) fs::remove(tempPath, ecRem);

        if (!segments.empty()) {
            fs::path csvPath = fs::path(vidPath).replace_extension(".csv");
            ofstream csv(csvPath);
            if (csv.is_open()) {
                for (const auto& seg : segments) csv << seg.start << "," << seg.end << ",\"" << seg.label << "\"" << endl;
                csv.close(); LogToUI("[SUCCESS] CSV Dibuat!");
            }
            if (!outDir.empty() && fs::exists(outDir)) {
                try {
                    std::error_code ecIn, ecOut;
                    fs::path normIn = fs::canonical(fs::path(vidPath).parent_path(), ecIn);
                    fs::path normOut = fs::canonical(outDir, ecOut);
                    if (!ecIn && !ecOut && normIn == normOut) {
                        LogToUI("[INFO] File video dan folder output sama, tidak perlu dipindahkan.");
                    } else {
                        fs::path targetVid = fs::path(outDir) / fs::path(vidPath).filename();
                        fs::path targetCsv = fs::path(outDir) / csvPath.filename();
                        LogToUI("[PROCESS] Memindahkan file...");
                        fs::copy(vidPath, targetVid, fs::copy_options::overwrite_existing);
                        fs::copy(csvPath, targetCsv, fs::copy_options::overwrite_existing);
                        fs::remove(vidPath); fs::remove(csvPath);
                        LogToUI("[DONE] File dipindahkan!");
                    }
                }
                catch (...) { LogToUI("[WARNING] Gagal pindah file."); }
            }
        }
        else { LogToUI("[INFO] Tidak ada momen."); }

        SetWindowTextA(hLblPercent, "100%");
        SetWindowTextA(hLblEta, "Selesai");
        LogToUI("=== SELESAI ===");
        MessageBeep(MB_OK);
    }
    catch (...) {
        std::error_code ecRem;
        if (!tempPath.empty()) fs::remove(tempPath, ecRem);
        LogToUI("[ERROR] Terjadi exception saat analisis video.");
    }
    isRunning = false; EnableWindow(hBtnStart, TRUE);
}

void CreateUI_AutoCut(HWND hP) {
    HFONT hH = CreateSFPro(22, FW_BOLD); HFONT hL = CreateSFPro(16, FW_MEDIUM); HFONT hM = CreateSFPro(15);
    int y = 20; int xBase = 20;

    hLbl1 = CreateWindowExA(0, "STATIC", "SOURCE & OUTPUT", WS_CHILD | WS_VISIBLE, xBase, y, 300, 25, hP, NULL, hInst, NULL); SendMessage(hLbl1, WM_SETFONT, (WPARAM)hH, TRUE); y += 40;

    hLbl2 = CreateWindowExA(0, "STATIC", "Video File:", WS_CHILD | WS_VISIBLE, xBase, y + 3, 100, 20, hP, NULL, hInst, NULL);
    hEditFile = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, xBase + 100, y, 460, 28, hP, (HMENU)ID_EDIT_FILE, hInst, NULL);
    hBtnFile = CreateWindowExA(0, "BUTTON", "...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase + 570, y, 40, 28, hP, (HMENU)ID_BTN_FILE, hInst, NULL); y += 45;

    hLbl3 = CreateWindowExA(0, "STATIC", "Output Path:", WS_CHILD | WS_VISIBLE, xBase, y + 3, 100, 20, hP, NULL, hInst, NULL);
    hEditOutput = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, xBase + 100, y, 460, 28, hP, (HMENU)ID_EDIT_OUTPUT, hInst, NULL);
    hBtnOutput = CreateWindowExA(0, "BUTTON", "...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase + 570, y, 40, 28, hP, (HMENU)ID_BTN_OUTPUT, hInst, NULL); y += 55;

    hLbl4 = CreateWindowExA(0, "STATIC", "CONFIGURATION", WS_CHILD | WS_VISIBLE, xBase, y, 300, 25, hP, NULL, hInst, NULL); SendMessage(hLbl4, WM_SETFONT, (WPARAM)hH, TRUE); y += 40;
    hLbl5 = CreateWindowExA(0, "STATIC", "Player Name:", WS_CHILD | WS_VISIBLE, xBase, y + 3, 100, 20, hP, NULL, hInst, NULL);
    hEditName = CreateWindowExA(0, "EDIT", PLAYER_PLACEHOLDER.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, xBase + 100, y, 200, 28, hP, (HMENU)ID_EDIT_NAME, hInst, NULL);
    hLbl6 = CreateWindowExA(0, "STATIC", "Pre-Roll:", WS_CHILD | WS_VISIBLE, xBase + 320, y + 3, 80, 20, hP, NULL, hInst, NULL);
    hEditPre = CreateWindowExA(0, "EDIT", "8", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_NUMBER, xBase + 400, y, 50, 28, hP, (HMENU)ID_EDIT_PRE, hInst, NULL);
    hLbl7 = CreateWindowExA(0, "STATIC", "Post-Roll:", WS_CHILD | WS_VISIBLE, xBase + 470, y + 3, 80, 20, hP, NULL, hInst, NULL);
    hEditPost = CreateWindowExA(0, "EDIT", "8", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_NUMBER, xBase + 550, y, 50, 28, hP, (HMENU)ID_EDIT_POST, hInst, NULL);
    hLblThresh = CreateWindowExA(0, "STATIC", "Thresh:", WS_CHILD | WS_VISIBLE, xBase + 610, y + 3, 50, 20, hP, NULL, hInst, NULL);
    hSliderThresh = CreateWindowExA(0, TRACKBAR_CLASSA, "Trackbar", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS, xBase + 665, y, 90, 28, hP, (HMENU)ID_SLIDER_THRESH, hInst, NULL);
    SendMessage(hSliderThresh, TBM_SETRANGE, TRUE, MAKELPARAM(20, 95));
    SendMessage(hSliderThresh, TBM_SETPOS, TRUE, 50);
    hLblThreshVal = CreateWindowExA(0, "STATIC", "50%", WS_CHILD | WS_VISIBLE | SS_CENTER, xBase + 760, y + 3, 40, 20, hP, (HMENU)ID_LBL_THRESH_VAL, hInst, NULL); y += 55;

    hLbl8 = CreateWindowExA(0, "STATIC", "AREA DETECTION (ROI)", WS_CHILD | WS_VISIBLE, xBase, y, 300, 25, hP, NULL, hInst, NULL); SendMessage(hLbl8, WM_SETFONT, (WPARAM)hH, TRUE); y += 40;
    hComboPreset = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, xBase, y, 320, 150, hP, (HMENU)ID_COMBO_PRESET, hInst, NULL);
    hBtnRoi = CreateWindowExA(0, "BUTTON", "Set Area", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase, y + 35, 150, 35, hP, (HMENU)ID_BTN_ROI, hInst, NULL);
    hBtnDel = CreateWindowExA(0, "BUTTON", "Delete", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase + 170, y + 35, 150, 35, hP, (HMENU)ID_BTN_DEL_PRESET, hInst, NULL);
    hEditPresetName = CreateWindowExA(0, "EDIT", "NewPreset", WS_CHILD | WS_VISIBLE | WS_BORDER, xBase, y + 80, 200, 28, hP, (HMENU)ID_EDIT_PRESET, hInst, NULL);
    hBtnSave = CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase + 210, y + 80, 110, 28, hP, (HMENU)ID_BTN_SAVE_PRESET, hInst, NULL);
    // INI YANG BENAR:
    hPreviewBox = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER | SS_NOTIFY, // SS_BLACKRECT DIHAPUS
        xBase + 360, y, 250, 110, hP, (HMENU)ID_STATIC_PREVIEW, hInst, NULL);
    hProgress = CreateWindowExA(0, PROGRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE, xBase, y, 610, 12, hP, (HMENU)ID_PROGRESS, hInst, NULL);
    hLblPercent = CreateWindowExA(0, "STATIC", "0%", WS_CHILD | WS_VISIBLE, xBase, y + 15, 100, 20, hP, (HMENU)ID_LBL_PERCENT, hInst, NULL);
    hLblEta = CreateWindowExA(0, "STATIC", "ETA: -", WS_CHILD | WS_VISIBLE | SS_RIGHT, xBase + 410, y + 15, 200, 20, hP, (HMENU)ID_LBL_ETA, hInst, NULL);

    y += 40;
    hEditLog = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, xBase, y, 610, 160, hP, (HMENU)ID_EDIT_LOG, hInst, NULL); y += 175;
    hBtnStart = CreateWindowExA(0, "BUTTON", "MULAI ANALISIS", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase, y, 400, 55, hP, (HMENU)ID_BTN_START, hInst, NULL);
    hBtnCancel = CreateWindowExA(0, "BUTTON", "STOP", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, xBase + 420, y, 190, 55, hP, (HMENU)ID_BTN_CANCEL, hInst, NULL);

    for (int id : {ID_EDIT_FILE, ID_EDIT_OUTPUT, ID_EDIT_NAME, ID_EDIT_PRE, ID_EDIT_POST, ID_EDIT_PRESET, ID_COMBO_PRESET, ID_LBL_PERCENT, ID_LBL_ETA, ID_LBL_THRESH_VAL})
        SendMessage(GetDlgItem(hP, id), WM_SETFONT, (WPARAM)hL, TRUE);
    SendMessage(hEditLog, WM_SETFONT, (WPARAM)hM, TRUE);
}

// --- FUNGSI BARU: MENGATUR UKURAN UI AGAR FIT WINDOW ---
void AutoCutResize(int w, int h) {
    if (!hEditFile) return; // Cegah crash jika UI belum siap

    int xBase = 20;
    int rMargin = 30; // Jarak dari pinggir kanan
    int fullW = w - xBase - rMargin;
    if (fullW < 400) fullW = 400; // Batas minimal lebar layout

    // 1. SECTION FILE INPUT
    // Label tetap di kiri, Tombol '...' di kanan, Edit box mengisi tengah
    int btnW = 40;
    int labelW = 100;
    int editW = fullW - labelW - btnW - 10; // 10 = spacing

    // Baris Video File (Y sekitar 43)
    MoveWindow(hEditFile, xBase + labelW, 43, editW, 28, TRUE);
    MoveWindow(hBtnFile, xBase + labelW + editW + 10, 43, btnW, 28, TRUE);

    // Baris Output Path (Y sekitar 98)
    MoveWindow(hEditOutput, xBase + labelW, 98, editW, 28, TRUE);
    MoveWindow(hBtnOutput, xBase + labelW + editW + 10, 98, btnW, 28, TRUE);

    // 2. SECTION CONFIG (Y sekitar 193)
    // Kita panjangkan Input Player Name untuk mengisi ruang
    // Pre-roll & Post-roll kita geser ke agak kanan agar rapi
    int smallInputW = 45;
    int lblSmallW = 70;

    int valW = 40;
    int sliderW = 95;
    int lblThreshW = 55;
    int valX = xBase + fullW - valW;
    int sliderX = valX - sliderW - 5;
    int lblThreshX = sliderX - lblThreshW - 5;

    int postX = lblThreshX - smallInputW - 15;
    int lblPostX = postX - lblSmallW;

    int preX = lblPostX - smallInputW - 15;
    int lblPreX = preX - lblSmallW;

    int nameX = xBase + 100;
    int nameW = lblPreX - nameX - 15;
    if (nameW < 120) nameW = 120;

    MoveWindow(hEditName, nameX, 193, nameW, 28, TRUE);
    MoveWindow(hLbl6, lblPreX, 196, lblSmallW, 20, TRUE);
    MoveWindow(hEditPre, preX, 193, smallInputW, 28, TRUE);
    MoveWindow(hLbl7, lblPostX, 196, lblSmallW, 20, TRUE);
    MoveWindow(hEditPost, postX, 193, smallInputW, 28, TRUE);
    if (hSliderThresh) {
        MoveWindow(hLblThresh, lblThreshX, 196, lblThreshW, 20, TRUE);
        MoveWindow(hSliderThresh, sliderX, 193, sliderW, 28, TRUE);
        MoveWindow(hLblThreshVal, valX, 196, valW, 20, TRUE);
    }

    // 3. SECTION ROI (Y sekitar 280)
    // Kolom kiri (Combo/Button) tetap, Preview Box memanjang ke kanan
    int leftColW = 320;
    int previewX = xBase + leftColW + 20;
    int previewW = fullW - (leftColW + 20);

    // Pastikan preview box tingginya proporsional atau fix (misal 110)
    MoveWindow(hPreviewBox, previewX, 280, previewW, 110, TRUE);

    // 4. PROGRESS BAR & LOG (Y sekitar 420 ke bawah)
    MoveWindow(hProgress, xBase, 420, fullW, 12, TRUE);

    // Label % di kiri, Label ETA di kanan. Y=438 memberi jarak aman dari progress bar
    MoveWindow(hLblPercent, xBase, 438, 100, 20, TRUE);
    MoveWindow(hLblEta, xBase + fullW - 200, 438, 200, 20, TRUE); // ETA Rata Kanan

    // Log Area (Mengisi sisa tinggi sampai tombol)
    // Y=475 memberi jarak aman dari label persen/ETA
    int logY = 475;
    int btnH = 55;
    int bottomMargin = 20;
    // Hitung sisa tinggi window untuk Log agar tombol tetap di bawah
    int logH = h - logY - btnH - bottomMargin - 20; // 20 spacing
    if (logH < 100) logH = 100; // Minimal tinggi log

    MoveWindow(hEditLog, xBase, logY, fullW, logH, TRUE);

    // 5. TOMBOL START / STOP (Di bagian paling bawah log)
    int btnY = logY + logH + 15;
    int startW = (fullW * 0.65); // Start ambil 65% lebar
    int stopW = fullW - startW - 10; // Stop ambil sisanya

    MoveWindow(hBtnStart, xBase, btnY, startW, btnH, TRUE);
    MoveWindow(hBtnCancel, xBase + startW + 10, btnY, stopW, btnH, TRUE);
}
void OpenRoiSelector(string v) {
    VideoCapture c(v);
    if (!c.isOpened()) {
        MessageBoxA(hMainFrame, "Gagal membuka video untuk ROI selector!", "Error", MB_ICONERROR);
        return;
    }
    int vw = (int)c.get(CAP_PROP_FRAME_WIDTH);
    int vh = (int)c.get(CAP_PROP_FRAME_HEIGHT);
    int x = g_roiRect.x, y = g_roiRect.y, w = g_roiRect.width, h = g_roiRect.height;
    if (w <= 0) {
        x = (int)(vw * 0.64);
        y = (int)(vh * 0.39);
        w = (int)(vw * 0.15);
        h = (int)(vh * 0.13);
    }
    if (x >= vw) x = 0;
    if (y >= vh) y = 0;
    if (x + w > vw) w = vw - x;
    if (y + h > vh) h = vh - y;

    string n = "SETTING AREA - ENTER TO SAVE";
    namedWindow(n, WINDOW_NORMAL);
    int winH = (vw > 0) ? (int)(1280.0 * vh / vw) : 720;
    resizeWindow(n, 1280, winH);
    createTrackbar("X", n, &x, vw);
    createTrackbar("Y", n, &y, vh);
    createTrackbar("W", n, &w, vw);
    createTrackbar("H", n, &h, vh);
    Mat f, p;
    c.read(f);
    while (1) {
        f.copyTo(p);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > vw) w = vw - x;
        if (y + h > vh) h = vh - y;
        rectangle(p, Rect(x, y, w, h), Scalar(0, 255, 0), 3);
        imshow(n, p);
        int key = waitKey(30);
        if (key == 13) {
            g_roiRect = Rect(x, y, w, h);
            g_roiSet = true;
            int curSel = (int)SendMessageA(hComboPreset, CB_GETCURSEL, 0, 0);
            if (curSel >= 0 && curSel < (int)g_presets.size()) {
                g_presets[curSel].baseW = vw;
                g_presets[curSel].baseH = vh;
                g_presets[curSel].x = x;
                g_presets[curSel].y = y;
                g_presets[curSel].w = w;
                g_presets[curSel].h = h;
            }
            destroyWindow(n);
            break;
        }
        if (getWindowProperty(n, WND_PROP_VISIBLE) < 1) break;
    }
    c.release();
}

void LoadPresetsFromFile(string selectName = "") {
    g_presets.clear();
    SendMessageA(hComboPreset, CB_RESETCONTENT, 0, 0);
    fs::path presetPath = fs::path(GetExeDir()) / "roi_presets.txt";
    ifstream file(presetPath);
    if (file.is_open()) {
        string line;
        while (getline(file, line)) {
            if (line.empty()) continue;
            char name[128] = { 0 };
            int x = 0, y = 0, w = 0, h = 0, bw = 0, bh = 0;
            int count = sscanf(line.c_str(), "%127[^|]|%d|%d|%d|%d|%d|%d", name, &x, &y, &w, &h, &bw, &bh);
            if (count >= 5) {
                if (count < 7) { bw = 0; bh = 0; }
                g_presets.push_back({ string(name), x, y, w, h, bw, bh });
                SendMessageA(hComboPreset, CB_ADDSTRING, 0, (LPARAM)name);
            }
        }
        file.close();
    }
    if (g_presets.empty()) {
        SendMessageA(hComboPreset, CB_ADDSTRING, 0, (LPARAM)"- Default -");
        g_roiRect = Rect(0, 0, 0, 0);
        g_roiSet = false;
    }
    else {
        int targetIdx = 0;
        if (!selectName.empty()) {
            for (size_t i = 0; i < g_presets.size(); i++) {
                if (g_presets[i].name == selectName) {
                    targetIdx = (int)i;
                    break;
                }
            }
        }
        SendMessageA(hComboPreset, CB_SETCURSEL, targetIdx, 0);
        g_roiRect = Rect(g_presets[targetIdx].x, g_presets[targetIdx].y, g_presets[targetIdx].w, g_presets[targetIdx].h);
        g_roiSet = true;
    }
}

void SavePresetToFile(string n, Rect r, int baseW = 0, int baseH = 0) {
    if (n.empty()) return;
    if (baseW <= 0 || baseH <= 0) {
        char vidPath[MAX_PATH] = { 0 };
        GetWindowTextA(hEditFile, vidPath, MAX_PATH);
        if (strlen(vidPath) > 0 && fs::exists(vidPath)) {
            VideoCapture cap(vidPath);
            if (cap.isOpened()) {
                baseW = (int)cap.get(CAP_PROP_FRAME_WIDTH);
                baseH = (int)cap.get(CAP_PROP_FRAME_HEIGHT);
                cap.release();
            }
        }
    }
    bool found = false;
    for (auto& p : g_presets) {
        if (p.name == n) {
            p.x = r.x; p.y = r.y; p.w = r.width; p.h = r.height;
            if (baseW > 0 && baseH > 0) { p.baseW = baseW; p.baseH = baseH; }
            found = true;
            break;
        }
    }
    if (!found) {
        g_presets.push_back({ n, r.x, r.y, r.width, r.height, baseW, baseH });
    }

    fs::path p = fs::path(GetExeDir()) / "roi_presets.txt";
    ofstream f(p);
    if (!f.is_open()) {
        MessageBoxA(hMainFrame, "Gagal menyimpan preset! Periksa hak akses file.", "Error", MB_ICONERROR);
        return;
    }
    for (const auto& pr : g_presets) {
        f << pr.name << "|" << pr.x << "|" << pr.y << "|" << pr.w << "|" << pr.h;
        if (pr.baseW > 0 && pr.baseH > 0) {
            f << "|" << pr.baseW << "|" << pr.baseH;
        }
        f << "\n";
    }
    f.close();
    MessageBoxA(hMainFrame, "Preset berhasil disimpan!", "Success", MB_OK);
    LoadPresetsFromFile(n);
}

void DeleteCurrentPreset() {
    int i = (int)SendMessageA(hComboPreset, CB_GETCURSEL, 0, 0);
    if (i < 0 || g_presets.empty()) return;
    if (MessageBoxA(hMainFrame, ("Hapus preset '" + g_presets[i].name + "'?").c_str(), "Konfirmasi", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        g_presets.erase(g_presets.begin() + i);
        fs::path p = fs::path(GetExeDir()) / "roi_presets.txt";
        ofstream f(p);
        if (f.is_open()) {
            for (const auto& pr : g_presets) {
                f << pr.name << "|" << pr.x << "|" << pr.y << "|" << pr.w << "|" << pr.h;
                if (pr.baseW > 0 && pr.baseH > 0) f << "|" << pr.baseW << "|" << pr.baseH;
                f << "\n";
            }
            f.close();
        }
        LoadPresetsFromFile();
    }
}
string OpenFileDialog(HWND h) { char f[MAX_PATH] = ""; OPENFILENAMEA o = { 0 }; o.lStructSize = sizeof(o); o.hwndOwner = h; o.lpstrFilter = "Video\0*.mp4;*.mkv\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; GetOpenFileNameA(&o); return string(f); }
string SelectFolderDialog(HWND h) { char p[MAX_PATH]; BROWSEINFOA b = { 0 }; b.hwndOwner = h; LPITEMIDLIST l = SHBrowseForFolderA(&b); if (l) { SHGetPathFromIDListA(l, p); return string(p); } return ""; }

// =========================================================
// BAGIAN 2: CODE KEDUA (LOSSCUT LOGIC - HASIL SCAN)
// =========================================================

struct MarkerRange { double startTime; double endTime; string label; };
vector<MarkerRange> markers;

mpv_handle* mpv = NULL;
HWND hLcVideoWnd, hLcListWnd, hLcBtnFolder, hLcBtnExport, hLcBtnShow;
string lcCurrentFile = "", lcLastExport = "", lcUserFolder = "";
double lcDuration = 0.0, lcCurrentPos = 0.0;
bool lcIsPaused = false, lcIsDragging = false, lcIsWaiting = false;
double lcLastTarget = -1.0;
atomic<bool> lcIsExporting(false);
string lcExportStatus = "";
const int LC_TIMELINE_H = 80;
const int LC_SIDE_W = 250;

string GetSettingsFilePath() {
    return (fs::path(GetExeDir()) / "settings.ini").string();
}

void LoadSettings() {
    string ini = GetSettingsFilePath();
    if (!fs::exists(ini)) return;

    char outBuf[MAX_PATH] = { 0 };
    GetPrivateProfileStringA("Settings", "OutputPath", "", outBuf, MAX_PATH, ini.c_str());
    if (strlen(outBuf) > 0) {
        SetWindowTextA(hEditOutput, outBuf);
        lcUserFolder = string(outBuf);
    }

    char preBuf[32] = { 0 };
    GetPrivateProfileStringA("Settings", "PreRoll", "8", preBuf, 32, ini.c_str());
    if (strlen(preBuf) > 0) {
        SetWindowTextA(hEditPre, preBuf);
    }

    char postBuf[32] = { 0 };
    GetPrivateProfileStringA("Settings", "PostRoll", "8", postBuf, 32, ini.c_str());
    if (strlen(postBuf) > 0) {
        SetWindowTextA(hEditPost, postBuf);
    }

    char nameBuf[128] = { 0 };
    GetPrivateProfileStringA("Settings", "PlayerName", "", nameBuf, 128, ini.c_str());
    if (strlen(nameBuf) > 0 && string(nameBuf) != PLAYER_PLACEHOLDER) {
        SetWindowTextA(hEditName, nameBuf);
    }

    char thBuf[32] = { 0 };
    GetPrivateProfileStringA("Settings", "Threshold", "50", thBuf, 32, ini.c_str());
    if (strlen(thBuf) > 0) {
        int th = atoi(thBuf);
        if (th >= 20 && th <= 95) {
            g_similarityThreshold = th;
            if (hSliderThresh) SendMessage(hSliderThresh, TBM_SETPOS, TRUE, th);
            if (hLblThreshVal) SetWindowTextA(hLblThreshVal, (to_string(th) + "%").c_str());
        }
    }
}

void SaveSettings() {
    string ini = GetSettingsFilePath();
    char outBuf[MAX_PATH] = { 0 };
    GetWindowTextA(hEditOutput, outBuf, MAX_PATH);
    WritePrivateProfileStringA("Settings", "OutputPath", outBuf, ini.c_str());

    char preBuf[32] = { 0 };
    GetWindowTextA(hEditPre, preBuf, 32);
    WritePrivateProfileStringA("Settings", "PreRoll", preBuf, ini.c_str());

    char postBuf[32] = { 0 };
    GetWindowTextA(hEditPost, postBuf, 32);
    WritePrivateProfileStringA("Settings", "PostRoll", postBuf, ini.c_str());

    char nameBuf[128] = { 0 };
    GetWindowTextA(hEditName, nameBuf, 128);
    if (string(nameBuf) != PLAYER_PLACEHOLDER) {
        WritePrivateProfileStringA("Settings", "PlayerName", nameBuf, ini.c_str());
    }

    WritePrivateProfileStringA("Settings", "Threshold", to_string(g_similarityThreshold).c_str(), ini.c_str());
}

// Utils
string ToDotString(double val) { stringstream ss; ss.imbue(locale("C")); ss << fixed << setprecision(3) << val; return ss.str(); }
string FormatAdaptive(double seconds, double maxDuration) { int totalSec = (int)seconds; int h = totalSec / 3600; int m = (totalSec % 3600) / 60; int s = totalSec % 60; stringstream ss; if (maxDuration >= 3600.0) { ss << h << ":" << setfill('0') << setw(2) << m << ":" << setfill('0') << setw(2) << s; } else { ss << setfill('0') << setw(2) << m << ":" << setfill('0') << setw(2) << s; } return ss.str(); }
string FormatShort(double sec) { int totalSec = (int)sec; int m = totalSec / 60; int s = totalSec % 60; stringstream ss; ss << setfill('0') << setw(2) << m << ":" << setfill('0') << setw(2) << s; return ss.str(); }
double ParseTime(string str) { double result = 0.0; if (str.find(':') != string::npos) { int m = 0, s = 0; char sep; stringstream ss(str); ss >> m >> sep >> s; result = (m * 60) + s; } else { try { result = stod(str); } catch (...) { result = 0.0; } } return result; }

void RunExportThread(string inputPath, string outputPath, double startSec, double endSec) {
    lcIsExporting = true; lcExportStatus = "Mencari FFmpeg...";
    string exeDir = GetExeDir(); string ffmpegPath = exeDir + "\\ffmpeg.exe";
    ifstream f(ffmpegPath.c_str());
    if (!f.good()) { MessageBoxA(NULL, "ffmpeg.exe missing!", "Error", MB_ICONERROR); lcIsExporting = false; return; }
    f.close(); lcExportStatus = "Encoding...";
    double duration = endSec - startSec; if (duration < 0.1) duration = 0.1;
    stringstream args; args << "\"" << ffmpegPath << "\" -y -ss " << ToDotString(startSec) << " -i \"" << inputPath << "\" -t " << ToDotString(duration) << " -c copy -avoid_negative_ts 1 \"" << outputPath << "\"";
    string cmdStr = args.str(); vector<char> cmdBuffer(cmdStr.begin(), cmdStr.end()); cmdBuffer.push_back(0);
    STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessA(NULL, &cmdBuffer[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        lcIsExporting = false; lcExportStatus = "Selesai!"; MessageBeep(MB_OK);
    }
    else { lcIsExporting = false; lcExportStatus = "Gagal Start"; }
}

void LcInitMPV(HWND hVideoTarget) {
    if (mpv) return;
    mpv = mpv_create(); if (!mpv) return;
    int64_t wid = (int64_t)hVideoTarget;
    mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);
    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "vo", "gpu");
    mpv_set_option_string(mpv, "keep-open", "yes");
    mpv_initialize(mpv);
}

void LcOpenVideo(string path) {
    if (!mpv) return;
    const char* cmd[] = { "loadfile", path.c_str(), NULL }; mpv_command(mpv, cmd);
    lcCurrentFile = path; int pause = 0; mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &pause);
    lcLastTarget = -1.0; lcIsWaiting = false;
}

void LcLoadCSV(string path) {
    markers.clear(); SendMessage(hLcListWnd, LB_RESETCONTENT, 0, 0);
    ifstream file(path); string line;
    while (getline(file, line)) {
        if (line.empty()) continue; stringstream ss(line); string startStr, endStr, labelStr;
        if (getline(ss, startStr, ',') && getline(ss, endStr, ',')) {
            getline(ss, labelStr); if (!labelStr.empty() && labelStr.back() == '\r') labelStr.pop_back(); if (labelStr.empty()) labelStr = "Event";
            double s = ParseTime(startStr); double e = ParseTime(endStr); if (e < s) e = s;
            markers.push_back({ s, e, labelStr });
            string uiText = "[" + FormatShort(s) + "-" + FormatShort(e) + "] " + labelStr;
            SendMessageA(hLcListWnd, LB_ADDSTRING, 0, (LPARAM)uiText.c_str());
        }
    }
}

void LcDrawTimeline(HWND hwnd) {
    HDC hdc = GetDC(hwnd); RECT r; GetClientRect(hwnd, &r);
    int winW = r.right - LC_SIDE_W; if (winW < 0) winW = 0;
    int winH = r.bottom; int yPos = winH - LC_TIMELINE_H;
    HDC hMemDC = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, winW, LC_TIMELINE_H);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);
    RECT bgRect = { 0, 0, winW, LC_TIMELINE_H };
    HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hMemDC, &bgRect, bgBrush);
    DeleteObject(bgBrush);

    int graphTop = 35; int graphBot = LC_TIMELINE_H - 10; int midY = (graphTop + graphBot) / 2;
    if (lcIsExporting) {
        RECT txtRect = { 0, 10, winW, 30 }; SetBkMode(hMemDC, TRANSPARENT); SetTextColor(hMemDC, RGB(0, 255, 255));
        DrawTextA(hMemDC, lcExportStatus.c_str(), -1, &txtRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else {
        if (lcDuration <= 0.1 || lcCurrentFile.empty()) {
            SetBkMode(hMemDC, TRANSPARENT); SetTextColor(hMemDC, RGB(100, 100, 100));
            RECT txtRect = { 0, 0, winW, LC_TIMELINE_H };
            DrawTextA(hMemDC, "NO VIDEO LOADED", -1, &txtRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else {
            RECT lineRect = { 10, midY - 1, winW - 10, midY + 1 };
            HBRUSH lineBrush = CreateSolidBrush(RGB(100, 100, 100));
            FillRect(hMemDC, &lineRect, lineBrush);
            DeleteObject(lineBrush);

            if (lcDuration > 0 && !markers.empty()) {
                HPEN hYelPen = CreatePen(PS_SOLID, 1, RGB(255, 200, 0));
                HBRUSH hYelBrush = CreateSolidBrush(RGB(255, 200, 0));
                HPEN hOldPen = (HPEN)SelectObject(hMemDC, hYelPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hMemDC, hYelBrush);
                int availableW = winW - 20;
                for (const auto& mk : markers) {
                    double s = (mk.startTime < 0) ? 0.0 : mk.startTime;
                    double e = (mk.endTime > lcDuration) ? lcDuration : mk.endTime;
                    if (s >= lcDuration) continue;
                    int x1 = 10 + (int)((s / lcDuration) * availableW);
                    int x2 = 10 + (int)((e / lcDuration) * availableW);
                    if (x2 - x1 < 4) x2 = x1 + 4;
                    RoundRect(hMemDC, x1, graphTop + 5, x2, graphBot - 5, 4, 4);
                }
                SelectObject(hMemDC, hOldPen); SelectObject(hMemDC, hOldBrush);
                DeleteObject(hYelPen); DeleteObject(hYelBrush);
            }

            if (lcDuration > 0) {
                int x = 10 + (int)((lcCurrentPos / lcDuration) * (winW - 20));
                HPEN hPen = CreatePen(PS_SOLID, 1, RGB(255, 50, 50));
                HBRUSH hBrush = CreateSolidBrush(RGB(255, 50, 50));
                HPEN hOldPen = (HPEN)SelectObject(hMemDC, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hMemDC, hBrush);
                RoundRect(hMemDC, x - 4, graphTop, x + 4, graphBot, 8, 8);
                SelectObject(hMemDC, hOldPen); SelectObject(hMemDC, hOldBrush);
                DeleteObject(hPen); DeleteObject(hBrush);
            }
            string txt = FormatAdaptive(lcCurrentPos, lcDuration) + " / " + FormatAdaptive(lcDuration, lcDuration);
            SetBkMode(hMemDC, TRANSPARENT); SetTextColor(hMemDC, RGB(255, 255, 255));
            RECT txRect = { 15, 5, winW, 25 };
            DrawTextA(hMemDC, txt.c_str(), -1, &txRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }
    BitBlt(hdc, 0, yPos, winW, LC_TIMELINE_H, hMemDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOldBitmap); DeleteObject(hBitmap); DeleteDC(hMemDC); ReleaseDC(hwnd, hdc);
}

void LcCreateUI(HWND hParent) {
    hLcVideoWnd = CreateWindowEx(0, "STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_BLACKRECT, 0, 0, 0, 0, hParent, NULL, hInst, NULL);
    LcInitMPV(hLcVideoWnd);
    hLcListWnd = CreateWindowEx(0, "LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | WS_BORDER, 0, 0, 0, 0, hParent, (HMENU)ID_LC_LISTBOX, hInst, NULL);
    hLcBtnFolder = CreateWindow("BUTTON", "Set Output Folder...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hParent, (HMENU)ID_LC_BTN_FOLDER, hInst, NULL);
    hLcBtnExport = CreateWindow("BUTTON", "EXPORT CLIP", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hParent, (HMENU)ID_LC_BTN_EXPORT, hInst, NULL);
    hLcBtnShow = CreateWindow("BUTTON", "Show Result", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hParent, (HMENU)ID_LC_BTN_SHOW, hInst, NULL);
    HFONT hL = CreateSFPro(16, FW_MEDIUM);
    SendMessage(hLcListWnd, WM_SETFONT, (WPARAM)hL, TRUE);
    SendMessage(hLcBtnFolder, WM_SETFONT, (WPARAM)hL, TRUE);
    SendMessage(hLcBtnExport, WM_SETFONT, (WPARAM)hL, TRUE);
    SendMessage(hLcBtnShow, WM_SETFONT, (WPARAM)hL, TRUE);
}

void LcResize(int w, int h) {
    if (!hLcVideoWnd) return;
    int listX = w - LC_SIDE_W; if (listX < 0) listX = 0;
    int btnH = 35; int margin = 5; int listHeight = h - (btnH * 3) - (margin * 4);
    MoveWindow(hLcListWnd, listX, 0, LC_SIDE_W, listHeight, TRUE);
    int yBtn = listHeight + margin;
    MoveWindow(hLcBtnFolder, listX, yBtn, LC_SIDE_W, btnH, TRUE); yBtn += btnH + margin;
    MoveWindow(hLcBtnExport, listX, yBtn, LC_SIDE_W, btnH, TRUE); yBtn += btnH + margin;
    MoveWindow(hLcBtnShow, listX, yBtn, LC_SIDE_W, btnH, TRUE);
    int vidW = w - LC_SIDE_W; int vidH = h - LC_TIMELINE_H;
    if (vidW < 0) vidW = 0; if (vidH < 0) vidH = 0;
    MoveWindow(hLcVideoWnd, 0, 0, vidW, vidH, TRUE);
}

// =========================================================
// MAIN FRAME LOGIC
// =========================================================

void ShowView(int id) {
    if (currentViewID == id && IsWindowVisible(hContentArea)) return;

    currentViewID = id;
    ShowWindow(hViewAutoCut, (id == 0) ? SW_SHOW : SW_HIDE);
    ShowWindow(hViewLossCut, (id == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(hViewGuide, (id == 2) ? SW_SHOW : SW_HIDE);
    ShowWindow(hViewAbout, (id == 3) ? SW_SHOW : SW_HIDE);

    InvalidateRect(hContentArea, NULL, TRUE);
    UpdateWindow(hContentArea);

    if (id == 1) { RECT r; GetClientRect(hContentArea, &r); LcResize(r.right, r.bottom); }
}

LRESULT CALLBACK AutoCutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        // --- PANGGIL FUNGSI RESIZE SAAT WINDOW BERUBAH UKURAN ---
        AutoCutResize(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_COMMAND:
        // Handle Tombol Auto Cut (Sama seperti sebelumnya)
        if (LOWORD(wParam) == ID_BTN_FILE) { string f = OpenFileDialog(hwnd); if (!f.empty()) SetWindowTextA(hEditFile, f.c_str()); }
        else if (LOWORD(wParam) == ID_BTN_OUTPUT) {
            string f = SelectFolderDialog(hwnd);
            if (!f.empty()) {
                SetWindowTextA(hEditOutput, f.c_str());
                lcUserFolder = f;
                SaveSettings();
            }
        }
        else if (LOWORD(wParam) == ID_BTN_ROI) { char f[MAX_PATH]; GetWindowTextA(hEditFile, f, MAX_PATH); if (strlen(f) > 0) OpenRoiSelector(f); else MessageBoxA(hwnd, "Pilih video!", "Info", MB_OK); }
        else if (LOWORD(wParam) == ID_BTN_SAVE_PRESET) {
            char n[100]; GetWindowTextA(hEditPresetName, n, 100);
            if (strlen(n) > 0 && g_roiSet) {
                int bw = 0, bh = 0;
                char vid[MAX_PATH] = { 0 };
                GetWindowTextA(hEditFile, vid, MAX_PATH);
                if (strlen(vid) > 0 && fs::exists(vid)) {
                    VideoCapture cap(vid);
                    if (cap.isOpened()) {
                        bw = (int)cap.get(CAP_PROP_FRAME_WIDTH);
                        bh = (int)cap.get(CAP_PROP_FRAME_HEIGHT);
                        cap.release();
                    }
                }
                SavePresetToFile(n, g_roiRect, bw, bh);
            }
        }
        else if (LOWORD(wParam) == ID_BTN_DEL_PRESET) DeleteCurrentPreset();
        else if (LOWORD(wParam) == ID_COMBO_PRESET && HIWORD(wParam) == CBN_SELCHANGE) { int i = (int)SendMessageA(hComboPreset, CB_GETCURSEL, 0, 0); if (i >= 0 && i < g_presets.size()) { g_roiRect = Rect(g_presets[i].x, g_presets[i].y, g_presets[i].w, g_presets[i].h); g_roiSet = true; LogToUI("Preset Loaded: " + g_presets[i].name); } }
        else if (LOWORD(wParam) == ID_BTN_START) {
            if (!isRunning) {
                char f[MAX_PATH] = { 0 }, o[MAX_PATH] = { 0 }, n[100] = { 0 }, pre[20] = { 0 }, post[20] = { 0 };
                GetWindowTextA(hEditFile, f, MAX_PATH); GetWindowTextA(hEditOutput, o, MAX_PATH); GetWindowTextA(hEditName, n, 100);
                GetWindowTextA(hEditPre, pre, 20); GetWindowTextA(hEditPost, post, 20);
                if (strlen(f) == 0 || string(n) == PLAYER_PLACEHOLDER) MessageBoxA(hwnd, "Input belum lengkap!", "Error", MB_ICONERROR);
                else {
                    SaveSettings();
                    double pr = atof(pre); double po = atof(post);
                    double threshVal = (double)g_similarityThreshold / 100.0;
                    EnableWindow(hBtnStart, FALSE); stopRequested = false;
                    thread(WorkerThread, string(f), string(o), string(n), pr, po, threshVal).detach();
                }
            }
        }
        else if (LOWORD(wParam) == ID_BTN_CANCEL) { if (isRunning) stopRequested = true; }

        // Placeholder Logic
        if (HIWORD(wParam) == EN_SETFOCUS && LOWORD(wParam) == ID_EDIT_NAME) { char cur[100]; GetWindowTextA(hEditName, cur, 100); if (string(cur) == PLAYER_PLACEHOLDER) SetWindowTextA(hEditName, ""); }
        else if (HIWORD(wParam) == EN_KILLFOCUS && LOWORD(wParam) == ID_EDIT_NAME) { char cur[100]; GetWindowTextA(hEditName, cur, 100); if (strlen(cur) == 0) SetWindowTextA(hEditName, PLAYER_PLACEHOLDER.c_str()); }
        break;

    case WM_HSCROLL: {
        if ((HWND)lParam == hSliderThresh) {
            int pos = (int)SendMessage(hSliderThresh, TBM_GETPOS, 0, 0);
            g_similarityThreshold = pos;
            string s = to_string(pos) + "%";
            SetWindowTextA(hLblThreshVal, s.c_str());
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam; HWND hCtl = (HWND)lParam;
        if (hCtl == hEditLog) { SetTextColor(hdc, COL_LOG_PURPLE); SetBkColor(hdc, COL_LOG_BG); static HBRUSH hbLog = CreateSolidBrush(COL_LOG_BG); return (LRESULT)hbLog; }
        SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_BG_MAIN); return (LRESULT)CreateSolidBrush(COL_BG_MAIN);
    }
    case WM_CTLCOLOREDIT: { HDC hdc = (HDC)wParam; SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_INPUT_BG); static HBRUSH hbIn = CreateSolidBrush(COL_INPUT_BG); return (LRESULT)hbIn; }

                        // AutoCut Custom Draw Buttons
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pD = (LPDRAWITEMSTRUCT)lParam;
        if (pD->CtlType == ODT_BUTTON) {
            HDC hdc = pD->hDC; RECT r = pD->rcItem; COLORREF bg, tx = COL_TEXT_WHITE; bool sel = (pD->itemState & ODS_SELECTED); bool disabled = (pD->itemState & ODS_DISABLED);

            if (disabled) bg = RGB(80, 80, 80);
            else if (pD->CtlID == ID_BTN_START) bg = sel ? RGB(60, 80, 200) : COL_BTN_BLUE;
            else if (pD->CtlID == ID_BTN_CANCEL) bg = sel ? RGB(200, 40, 50) : COL_BTN_RED;
            else bg = sel ? COL_BTN_HOVER : COL_BTN_NORMAL;

            HBRUSH hBr = CreateSolidBrush(bg); FillRect(hdc, &r, hBr); DeleteObject(hBr);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, disabled ? RGB(150, 150, 150) : tx); char b[128]; GetWindowTextA(pD->hwndItem, b, 128); DrawTextA(hdc, b, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return TRUE;
        }
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK LossCutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: LcResize(LOWORD(lParam), HIWORD(lParam)); break;
    case WM_PAINT: ValidateRect(hwnd, NULL); LcDrawTimeline(hwnd); return 0;
    case WM_CTLCOLORLISTBOX: { HDC hdc = (HDC)wParam; SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_INPUT_BG); static HBRUSH hbInput = CreateSolidBrush(COL_INPUT_BG); return (LRESULT)hbInput; }
    case WM_CTLCOLORSTATIC: { HDC hdc = (HDC)wParam; SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_BG_MAIN); static HBRUSH hbMain = CreateSolidBrush(COL_BG_MAIN); return (LRESULT)hbMain; }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pD = (LPDRAWITEMSTRUCT)lParam;
        if (pD->CtlType == ODT_BUTTON) {
            HDC hdc = pD->hDC; RECT r = pD->rcItem; COLORREF bg, tx = COL_TEXT_WHITE; bool sel = (pD->itemState & ODS_SELECTED);
            if (pD->CtlID == ID_LC_BTN_EXPORT) bg = sel ? RGB(60, 80, 200) : COL_BTN_BLUE; else bg = sel ? COL_BTN_HOVER : COL_BTN_NORMAL;
            HBRUSH hBr = CreateSolidBrush(bg); FillRect(hdc, &r, hBr); DeleteObject(hBr);
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, tx); char b[128]; GetWindowTextA(pD->hwndItem, b, 128); DrawTextA(hdc, b, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return TRUE;
        } break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_LC_BTN_FOLDER) {
            string f = SelectFolderDialog(hwnd);
            if (!f.empty()) {
                lcUserFolder = f;
                SetWindowTextA(hEditOutput, f.c_str());
                SaveSettings();
                MessageBoxA(hwnd, ("Folder diset ke:\n" + f).c_str(), "Info", MB_OK);
            }
        }
        else if (id == ID_LC_BTN_EXPORT) {
            if (lcIsExporting) MessageBox(hwnd, "Tunggu...", "Busy", MB_OK);
            else if (lcCurrentFile.empty()) MessageBox(hwnd, "Load video dulu!", "Error", MB_ICONERROR);
            else {
                int idx = (int)SendMessage(hLcListWnd, LB_GETCURSEL, 0, 0);
                if (idx != LB_ERR && idx < markers.size()) {
                    MarkerRange& mk = markers[idx]; string safeLabel = mk.label; for (char& c : safeLabel) if (string("\\/:?\"<>|* ").find(c) != string::npos) c = '_';
                    string targetFolder = lcUserFolder.empty() ? lcCurrentFile.substr(0, lcCurrentFile.find_last_of("\\/")) : lcUserFolder;
                    string nameOnly = lcCurrentFile.substr(lcCurrentFile.find_last_of("\\/") + 1); nameOnly = nameOnly.substr(0, nameOnly.find_last_of('.'));
                    string outPath = targetFolder + "\\" + nameOnly + "_" + safeLabel + ".mp4"; lcLastExport = outPath;
                    thread(RunExportThread, lcCurrentFile, outPath, mk.startTime, mk.endTime).detach();
                }
                else MessageBox(hwnd, "Pilih list!", "Info", MB_OK);
            }
        }
        else if (id == ID_LC_BTN_SHOW) { if (!lcLastExport.empty()) ShellExecuteA(NULL, "open", "explorer.exe", ("/select,\"" + lcLastExport + "\"").c_str(), NULL, SW_SHOW); }
        else if (id == ID_LC_LISTBOX && HIWORD(wParam) == LBN_SELCHANGE) {
            int idx = (int)SendMessage(hLcListWnd, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR && idx < markers.size() && mpv) {
                double t = markers[idx].startTime; string val = ToDotString(t); const char* args[] = { "seek", val.c_str(), "absolute", "exact", NULL };
                mpv_command_async(mpv, 0, args); lcIsWaiting = true; lcLastTarget = t; lcCurrentPos = t;
            }
        } break;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam; char filename[MAX_PATH];
        if (DragQueryFileA(hDrop, 0, filename, MAX_PATH)) {
            string path = filename; string ext = path.substr(path.find_last_of(".") + 1); for (auto& c : ext) c = toupper(c);
            if (ext == "CSV") { LcLoadCSV(path); }
            else { markers.clear(); SendMessage(hLcListWnd, LB_RESETCONTENT, 0, 0); LcOpenVideo(path); MessageBoxA(hwnd, "Video berhasil dimuat.\n\nIngin melihat hasil scanningnya? Wajib import/drag n drop file .csv nya!", "Reminder", MB_ICONINFORMATION); }
        } DragFinish(hDrop); break;
    }
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(hwnd, &r); int y = HIWORD(lParam); int x = LOWORD(lParam);
        if (y >= (r.bottom - LC_TIMELINE_H) && x < (r.right - LC_SIDE_W)) {
            lcIsDragging = true; SetCapture(hwnd); double ratio = (double)(x - 10) / (double)((r.right - LC_SIDE_W) - 20);
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1; lcCurrentPos = ratio * lcDuration; lcLastTarget = lcCurrentPos; lcIsWaiting = true;
            if (mpv) { string val = ToDotString(lcCurrentPos); const char* args[] = { "seek", val.c_str(), "absolute", "exact", NULL }; mpv_command_async(mpv, 0, args); }
            LcDrawTimeline(hwnd);
        }
        else if (x < (r.right - LC_SIDE_W) && mpv) { int pause = 0; mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &pause); pause = !pause; mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &pause); }
        break;
    }
    case WM_MOUSEMOVE:
        if (lcIsDragging) {
            RECT r; GetClientRect(hwnd, &r);
            int x = LOWORD(lParam);
            // Hitung ulang posisi saat drag
            double ratio = (double)(x - 10) / (double)((r.right - LC_SIDE_W) - 20);
            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;

            lcCurrentPos = ratio * lcDuration;
            lcLastTarget = lcCurrentPos;
            lcIsWaiting = true;

            if (mpv) {
                string val = ToDotString(lcCurrentPos);
                const char* args[] = { "seek", val.c_str(), "absolute", "exact", NULL };
                mpv_command_async(mpv, 0, args);
            }
            LcDrawTimeline(hwnd);
        }
        break;
    case WM_LBUTTONUP: if (lcIsDragging) { lcIsDragging = false; ReleaseCapture(); } break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

#define ID_STATIC_LOGO 999 

LRESULT CALLBACK MainProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        hInst = ((LPCREATESTRUCT)lParam)->hInstance;

        hSidebar = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 250, 900, hwnd, NULL, hInst, NULL);

        HWND hLogo = CreateWindowExA(0, "STATIC", "REELKILL\nV1.5", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 40, 250, 60, hwnd, (HMENU)ID_STATIC_LOGO, hInst, NULL);
        SendMessage(hLogo, WM_SETFONT, (WPARAM)CreateSFPro(24, FW_BOLD), TRUE);

        int btnY = 140; int btnH = 50;
        CreateWindow("BUTTON", "  DASHBOARD", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, btnY, 250, btnH, hwnd, (HMENU)ID_NAV_AUTOCUT, hInst, NULL); btnY += btnH;
        CreateWindow("BUTTON", "  HASIL SCAN", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, btnY, 250, btnH, hwnd, (HMENU)ID_NAV_LOSSCUT, hInst, NULL); btnY += btnH;
        CreateWindow("BUTTON", "  PANDUAN", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, btnY, 250, btnH, hwnd, (HMENU)ID_NAV_GUIDE, hInst, NULL); btnY += btnH;
        CreateWindow("BUTTON", "  TENTANG", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, btnY, 250, btnH, hwnd, (HMENU)ID_NAV_ABOUT, hInst, NULL);

        hContentArea = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            250, 0, 1030, 720, hwnd, NULL, hInst, NULL);

        WNDCLASSA wc1 = { 0 }; wc1.lpfnWndProc = AutoCutProc; wc1.hInstance = hInst; wc1.lpszClassName = "PanelAutoCut"; wc1.hbrBackground = CreateSolidBrush(COL_BG_MAIN); wc1.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc1);
        hViewAutoCut = CreateWindowExA(0, "PanelAutoCut", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1030, 720, hContentArea, NULL, hInst, NULL);
        CreateUI_AutoCut(hViewAutoCut); LoadPresetsFromFile(); LoadSettings();

        WNDCLASSA wc2 = { 0 }; wc2.lpfnWndProc = LossCutProc; wc2.hInstance = hInst; wc2.lpszClassName = "PanelLossCut"; wc2.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wc2.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc2);
        hViewLossCut = CreateWindowExA(0, "PanelLossCut", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1030, 720, hContentArea, NULL, hInst, NULL);
        LcCreateUI(hViewLossCut); DragAcceptFiles(hViewLossCut, TRUE);

        WNDCLASSA wcTxt = { 0 }; wcTxt.lpfnWndProc = DefWindowProc; wcTxt.hInstance = hInst; wcTxt.lpszClassName = "PanelTextDark"; wcTxt.hbrBackground = CreateSolidBrush(COL_BG_MAIN); wcTxt.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wcTxt);
        hViewGuide = CreateWindowExA(0, "PanelTextDark", NULL, WS_CHILD, 40, 40, 600, 400, hContentArea, NULL, hInst, NULL);
        hViewAbout = CreateWindowExA(0, "PanelTextDark", NULL, WS_CHILD, 40, 40, 600, 400, hContentArea, NULL, hInst, NULL);

        HWND hTxtG = CreateWindowExA(0, "STATIC", "PANDUAN:\n1. Dashboard: Lakukan Scan kill feed di sini.\n2. Hasil Scan: Load Video & CSV hasil scan untuk export.\n\nJangan lupa set ffmpeg.exe di folder aplikasi.", WS_CHILD | WS_VISIBLE, 0, 0, 600, 400, hViewGuide, NULL, hInst, NULL);
        HWND hTxtA = CreateWindowExA(0, "STATIC", "REELKILL V1.5\nPUBG Mobile Auto-Cut Analysis.\n\nHighlighting Engine with Fast OCR & Lossless Cut.", WS_CHILD | WS_VISIBLE, 0, 0, 600, 400, hViewAbout, NULL, hInst, NULL);
        SendMessage(hTxtG, WM_SETFONT, (WPARAM)CreateSFPro(16), TRUE); SendMessage(hTxtA, WM_SETFONT, (WPARAM)CreateSFPro(16), TRUE);

        ShowView(0); break;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam); int h = HIWORD(lParam);
        MoveWindow(hSidebar, 0, 0, 250, h, TRUE); MoveWindow(hContentArea, 250, 0, w - 250, h, TRUE);
        MoveWindow(hViewAutoCut, 0, 0, w - 250, h, TRUE); MoveWindow(hViewLossCut, 0, 0, w - 250, h, TRUE);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_NAV_AUTOCUT || id == ID_NAV_LOSSCUT || id == ID_NAV_GUIDE || id == ID_NAV_ABOUT) {
            int viewIndex = 0;
            if (id == ID_NAV_LOSSCUT) viewIndex = 1;
            else if (id == ID_NAV_GUIDE) viewIndex = 2;
            else if (id == ID_NAV_ABOUT) viewIndex = 3;

            ShowView(viewIndex);

            RECT rSidebar = { 0, 0, 250, 5000 };
            InvalidateRect(hwnd, &rSidebar, FALSE);
        }
        break;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam; HWND hCtl = (HWND)lParam; int id = GetDlgCtrlID(hCtl);
        if (hCtl == hSidebar || id == ID_STATIC_LOGO) { SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_BG_SIDEBAR); static HBRUSH hbS = CreateSolidBrush(COL_BG_SIDEBAR); return (LRESULT)hbS; }
        if (hCtl == hContentArea || GetParent(hCtl) == hViewGuide || GetParent(hCtl) == hViewAbout) { SetTextColor(hdc, COL_TEXT_WHITE); SetBkColor(hdc, COL_BG_MAIN); static HBRUSH hbM = CreateSolidBrush(COL_BG_MAIN); return (LRESULT)hbM; }
        break;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pD = (LPDRAWITEMSTRUCT)lParam;
        if (pD->CtlType == ODT_BUTTON && pD->CtlID >= 2000) {
            HDC hdc = pD->hDC; RECT r = pD->rcItem; bool isActive = false;
            if (pD->CtlID == ID_NAV_AUTOCUT && currentViewID == 0) isActive = true;
            if (pD->CtlID == ID_NAV_LOSSCUT && currentViewID == 1) isActive = true;
            if (pD->CtlID == ID_NAV_GUIDE && currentViewID == 2) isActive = true;
            if (pD->CtlID == ID_NAV_ABOUT && currentViewID == 3) isActive = true;
            COLORREF bg; if (isActive) bg = RGB(40, 42, 48); else if (pD->itemState & ODS_SELECTED) bg = COL_BTN_HOVER; else bg = COL_BG_SIDEBAR;
            HBRUSH hBr = CreateSolidBrush(bg); FillRect(hdc, &r, hBr); DeleteObject(hBr);
            if (isActive) { RECT bar = { r.left, r.top + 8, r.left + 4, r.bottom - 8 }; HBRUSH hAccent = CreateSolidBrush(COL_BTN_BLUE); FillRect(hdc, &bar, hAccent); DeleteObject(hAccent); }
            SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, isActive ? RGB(255, 255, 255) : RGB(180, 180, 180));
            RECT txtR = r; txtR.left += 20; char b[128]; GetWindowTextA(pD->hwndItem, b, 128); DrawTextA(hdc, b, -1, &txtR, DT_LEFT | DT_VCENTER | DT_SINGLELINE); return TRUE;
        } break;
    }
    case WM_DESTROY: SaveSettings(); if (mpv) mpv_terminate_destroy(mpv); if (hOcrEngine) OcrDestroy(hOcrEngine); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main() {
    CoInitialize(NULL);
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = MainProc; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "ReelKillMainWindow";
    HICON hAppIcon = (HICON)LoadImageA(NULL, "app.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hAppIcon) hAppIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIcon = hAppIcon;
    wc.hbrBackground = CreateSolidBrush(COL_BG_MAIN); wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc);
    hMainFrame = CreateWindowExA(0, "ReelKillMainWindow", "REELKILL - 1.5", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760, NULL, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(hMainFrame, WM_SETICON, ICON_BIG, (LPARAM)hAppIcon);
    SendMessage(hMainFrame, WM_SETICON, ICON_SMALL, (LPARAM)hAppIcon);
    ShowWindow(hMainFrame, SW_SHOW);

    MSG msg = { 0 };
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        else {
            if (mpv && currentViewID == 1) {
                mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &lcDuration);
                double realPos = 0.0; mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &realPos);
                if (!lcIsDragging) {
                    if (lcIsWaiting) { if (abs(realPos - lcLastTarget) < 1.0) lcIsWaiting = false; else lcCurrentPos = lcLastTarget; }
                    else lcCurrentPos = realPos;
                }
                while (1) { mpv_event* e = mpv_wait_event(mpv, 0); if (e->event_id == MPV_EVENT_NONE) break; }
                LcDrawTimeline(hViewLossCut);
            }
            MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
        }
    }
    CoUninitialize(); return 0;
}