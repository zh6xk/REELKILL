#include "Scanner.h"
#include <windows.h>
#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>

#include "OcrLiteCApi.h"

namespace fs = std::filesystem;
using namespace cv;
using namespace std;
using namespace std::chrono;

namespace ReelKill {

static string NormalizeForMatch(const string& in) {
    string out;
    for (char c : in) {
        if (isalnum((unsigned char)c)) {
            out.push_back(tolower((unsigned char)c));
        }
    }
    return out;
}

static int LevenshteinDistance(const string& s1, const string& s2) {
    const size_t m = s1.size();
    const size_t n = s2.size();
    if (m == 0) return (int)n;
    if (n == 0) return (int)m;
    vector<int> costs(n + 1);
    for (size_t k = 0; k <= n; k++) costs[k] = (int)k;
    size_t i = 0;
    for (char c1 : s1) {
        costs[0] = (int)i + 1;
        int corner = (int)i;
        size_t j = 0;
        for (char c2 : s2) {
            int upper = costs[j + 1];
            if (c1 == c2) {
                costs[j + 1] = corner;
            } else {
                int t = (upper < corner) ? upper : corner;
                costs[j + 1] = ((costs[j] < t) ? costs[j] : t) + 1;
            }
            corner = upper;
            j++;
        }
        i++;
    }
    return costs[n];
}

static double GetBestSimilarity(const string& ocrText, const string& targetName) {
    if (ocrText.empty() || targetName.empty()) return 0.0;
    if (ocrText.find(targetName) != string::npos) return 1.0;

    int tLen = (int)targetName.length();
    int oLen = (int)ocrText.length();
    if (oLen < tLen) {
        int dist = LevenshteinDistance(ocrText, targetName);
        return 1.0 - ((double)dist / max(tLen, oLen));
    }

    double bestScore = 0.0;
    for (int len = max(1, tLen - 2); len <= tLen + 2; len++) {
        for (int i = 0; i <= oLen - len; i++) {
            string sub = ocrText.substr(i, len);
            int dist = LevenshteinDistance(sub, targetName);
            double score = 1.0 - ((double)dist / max((int)sub.length(), tLen));
            if (score > bestScore) bestScore = score;
        }
    }
    return bestScore;
}

static string FormatTimeSafe(int sec) {
    int m = sec / 60;
    int s = sec % 60;
    stringstream ss;
    ss << setfill('0') << setw(2) << m << ":" << setfill('0') << setw(2) << s;
    return ss.str();
}

static string GetModuleDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    string s(path);
    size_t pos = s.find_last_of("\\/");
    return (pos != string::npos) ? s.substr(0, pos) : "";
}

class Scanner::Impl {
public:
    std::atomic<bool> isRunning{false};
    std::atomic<bool> stopRequested{false};
    std::thread workerThread;
    OCR_HANDLE hOcrEngine{nullptr};

    ~Impl() {
        Stop();
        if (hOcrEngine) {
            OcrDestroy(hOcrEngine);
            hOcrEngine = nullptr;
        }
    }

    // Selalu join thread yang masih joinable, juga setelah scan selesai,
    // supaya destruktor tidak memanggil std::terminate.
    void Stop() {
        stopRequested.store(true);
        if (workerThread.joinable()) {
            workerThread.join();
        }
        isRunning.store(false);
    }

    void Worker(ScanParams params, ScanCallbacks callbacks) {
        fs::path tempPath;
        vector<FoundClip> foundClips;
        bool stoppedByUser = false;

        auto log = [&](const string& msg) {
            if (callbacks.onLog) callbacks.onLog(msg);
        };

        try {
            fs::path exeDir(GetModuleDir());
            fs::path modelsDir = exeDir / "models";
            string det = (modelsDir / "ch_PP-OCRv4_det_infer.onnx").string();
            string cls = (modelsDir / "ch_ppocr_mobile_v2.0_cls_infer.onnx").string();
            string rec = (modelsDir / "ch_PP-OCRv3_rec_infer.onnx").string();
            string keys = (modelsDir / "ppocr_keys_v1.txt").string();
            replace(det.begin(), det.end(), '\\', '/');
            replace(cls.begin(), cls.end(), '\\', '/');
            replace(rec.begin(), rec.end(), '\\', '/');
            replace(keys.begin(), keys.end(), '\\', '/');

            if (!fs::exists(det)) {
                log("[FATAL] OCR Models missing di folder models/!");
                if (callbacks.onError) callbacks.onError("OCR Models missing di folder models/!");
                isRunning.store(false);
                if (callbacks.onDone) callbacks.onDone(false, foundClips);
                return;
            }

            int maxThreads = (int)std::thread::hardware_concurrency();
            if (maxThreads < 2) maxThreads = 4;
            if (!hOcrEngine) {
                hOcrEngine = OcrInit(det.c_str(), cls.c_str(), rec.c_str(), keys.c_str(), maxThreads);
            }
            if (!hOcrEngine) {
                log("[FATAL] Gagal menginisialisasi engine RapidOCR.");
                if (callbacks.onError) callbacks.onError("Gagal menginisialisasi engine RapidOCR.");
                isRunning.store(false);
                if (callbacks.onDone) callbacks.onDone(false, foundClips);
                return;
            }

            VideoCapture cap(params.videoPath);
            if (!cap.isOpened()) {
                log("[ERROR] Gagal membuka video: " + params.videoPath);
                if (callbacks.onError) callbacks.onError("Gagal membuka video: " + params.videoPath);
                isRunning.store(false);
                if (callbacks.onDone) callbacks.onDone(false, foundClips);
                return;
            }

            int totalFrames = (int)cap.get(CAP_PROP_FRAME_COUNT);
            double fps = cap.get(CAP_PROP_FPS);
            if (fps <= 0) fps = 30.0;
            int vW = (int)cap.get(CAP_PROP_FRAME_WIDTH);
            int vH = (int)cap.get(CAP_PROP_FRAME_HEIGHT);

            Rect activeRoi;
            if (params.hasCustomRoi && params.roi.width > 0 && params.roi.height > 0) {
                activeRoi = Rect(params.roi.x, params.roi.y, params.roi.width, params.roi.height);
            } else {
                activeRoi = Rect((int)(vW * 0.64), (int)(vH * 0.39), (int)(vW * 0.15), (int)(vH * 0.13));
            }
            if (activeRoi.x < 0) activeRoi.x = 0;
            if (activeRoi.y < 0) activeRoi.y = 0;
            if (activeRoi.x + activeRoi.width > vW) activeRoi.width = vW - activeRoi.x;
            if (activeRoi.y + activeRoi.height > vH) activeRoi.height = vH - activeRoi.y;

            // Optimized OCR_PARAM: doAngle=0, mostAngle=0 (Skip angle classifier)
            OCR_PARAM ocrParam = { 50, 1024, 0.5f, 0.3f, 1.6f, 0, 0 };

            std::error_code ecTemp;
            fs::path tempDir = fs::temp_directory_path(ecTemp);
            if (ecTemp || !fs::exists(tempDir)) {
                tempDir = exeDir;
            }
            tempPath = tempDir / ("reelkill_temp_" + to_string(GetCurrentProcessId()) + ".bmp");
            string fullPath = tempPath.string();
            replace(fullPath.begin(), fullPath.end(), '\\', '/');

            string safeTarget = NormalizeForMatch(params.playerName);
            log("[START] Analisis dimulai untuk player: " + params.playerName);

            double checkInterval = 0.35;
            int frameStep = (int)(fps * checkInterval);
            if (frameStep < 1) frameStep = 1;

            Mat frame, crop, processedCrop, grayCrop, prevGrayCrop, diffMat;
            int processedFrames = 0;
            int hitCount = 0;
            auto startTime = high_resolution_clock::now();
            double avgTimePerStep = 0.0;

            while (processedFrames < totalFrames) {
                if (stopRequested.load()) {
                    stoppedByUser = true;
                    log("[STOP] Scan dihentikan oleh pengguna.");
                    break;
                }

                auto stepStart = high_resolution_clock::now();
                bool ok = cap.read(frame);
                if (!ok) break;

                for (int k = 0; k < frameStep - 1; k++) {
                    cap.grab();
                    processedFrames++;
                }
                processedFrames++;

                if (activeRoi.x + activeRoi.width > frame.cols) activeRoi.width = frame.cols - activeRoi.x;
                if (activeRoi.y + activeRoi.height > frame.rows) activeRoi.height = frame.rows - activeRoi.y;

                crop = frame(activeRoi);
                if (!crop.empty()) {
                    // Motion Gate: Check ROI difference
                    cvtColor(crop, grayCrop, COLOR_BGR2GRAY);
                    bool sceneChanged = true;
                    if (!prevGrayCrop.empty() && prevGrayCrop.size() == grayCrop.size()) {
                        absdiff(grayCrop, prevGrayCrop, diffMat);
                        double meanDiff = mean(diffMat)[0];
                        if (meanDiff < 3.5) {
                            sceneChanged = false; // ROI static, kill feed hasn't changed
                        }
                    }
                    grayCrop.copyTo(prevGrayCrop);

                    if (sceneChanged) {
                        resize(crop, processedCrop, Size(), 2.0, 2.0, INTER_CUBIC);
                        if (callbacks.onPreviewMat) {
                            callbacks.onPreviewMat(&processedCrop);
                        }

                        if (imwrite(fullPath, processedCrop)) {
                            if (OcrDetect(hOcrEngine, fullPath.c_str(), "", &ocrParam)) {
                                int len = OcrGetLen(hOcrEngine);
                                if (len > 0) {
                                    vector<char> buf(len + 1);
                                    OcrGetResult(hOcrEngine, buf.data(), len + 1);
                                    string resRaw = string(buf.data());
                                    string resSafe = NormalizeForMatch(resRaw);
                                    double similarity = GetBestSimilarity(resSafe, safeTarget);

                                    if (similarity >= params.minSimilarity) {
                                        double currentTime = (double)processedFrames / fps;
                                        double idealStart = max(0.0, currentTime - params.preSec);
                                        double idealEnd = min((double)totalFrames / fps, currentTime + params.postSec);

                                        if (!foundClips.empty() && idealStart <= foundClips.back().end + 2.0) {
                                            foundClips.back().end = max(foundClips.back().end, idealEnd);
                                        } else {
                                            hitCount++;
                                            FoundClip clip = { idealStart, idealEnd, "kill", similarity };
                                            foundClips.push_back(clip);
                                            log("[HIT #" + to_string(hitCount) + "] Kill Detected at " +
                                                FormatTimeSafe((int)currentTime) + " | Teks: \"" + resRaw +
                                                "\" (Skor: " + to_string((int)(similarity * 100)) + "%)");
                                            if (callbacks.onClip) callbacks.onClip(clip);
                                        }
                                    } else if (similarity >= 0.25) {
                                        log("[SCAN] Teks: \"" + resRaw + "\" | Skor: " +
                                            to_string((int)(similarity * 100)) + "% (< Min " +
                                            to_string((int)(params.minSimilarity * 100)) + "%)");
                                    }
                                }
                            }
                        }
                    }
                }

                auto stepEnd = high_resolution_clock::now();
                duration<double> stepDur = stepEnd - stepStart;
                if (avgTimePerStep == 0.0) avgTimePerStep = stepDur.count();
                else avgTimePerStep = (avgTimePerStep * 0.9) + (stepDur.count() * 0.1);

                if (processedFrames % (frameStep * 2) == 0 || processedFrames >= totalFrames) {
                    int percent = (int)((float)processedFrames / totalFrames * 100);
                    int framesLeft = totalFrames - processedFrames;
                    int etaSec = (int)((framesLeft / (double)frameStep) * avgTimePerStep);
                    if (callbacks.onProgress) {
                        callbacks.onProgress(percent, processedFrames, totalFrames, fps, etaSec);
                    }
                }
            }

            cap.release();
            std::error_code ecRem;
            if (!tempPath.empty()) fs::remove(tempPath, ecRem);

            if (!foundClips.empty()) {
                fs::path csvPath = fs::path(params.videoPath).replace_extension(".csv");
                ofstream csv(csvPath);
                if (csv.is_open()) {
                    for (const auto& seg : foundClips) {
                        csv << seg.start << "," << seg.end << ",\"" << seg.label << "\"" << endl;
                    }
                    csv.close();
                    log("[SUCCESS] CSV Dibuat: " + csvPath.string());
                }

                if (params.relocateResults && !params.outputDir.empty() && fs::exists(params.outputDir)) {
                    try {
                        std::error_code ecIn, ecOut;
                        fs::path normIn = fs::canonical(fs::path(params.videoPath).parent_path(), ecIn);
                        fs::path normOut = fs::canonical(params.outputDir, ecOut);
                        if (!ecIn && !ecOut && normIn == normOut) {
                            log("[INFO] File video dan folder output sama, tidak perlu dipindahkan.");
                        } else {
                            fs::path targetVid = fs::path(params.outputDir) / fs::path(params.videoPath).filename();
                            fs::path targetCsv = fs::path(params.outputDir) / csvPath.filename();
                            log("[PROCESS] Memindahkan file hasil ke folder output...");
                            fs::copy(params.videoPath, targetVid, fs::copy_options::overwrite_existing);
                            fs::copy(csvPath, targetCsv, fs::copy_options::overwrite_existing);
                            fs::remove(params.videoPath);
                            fs::remove(csvPath);
                            log("[DONE] File video dan CSV berhasil dipindahkan!");
                        }
                    } catch (...) {
                        log("[WARNING] Gagal memindahkan file ke folder output.");
                    }
                }
            } else {
                log("[INFO] Tidak ada momen kill yang terdeteksi.");
            }

            log("=== SELESAI ===");
        } catch (...) {
            std::error_code ecRem;
            if (!tempPath.empty()) fs::remove(tempPath, ecRem);
            log("[ERROR] Terjadi exception tak terduga saat scanning.");
            if (callbacks.onError) callbacks.onError("Terjadi exception tak terduga saat scanning.");
        }

        isRunning.store(false);
        if (callbacks.onDone) {
            callbacks.onDone(stoppedByUser, foundClips);
        }
    }
};

Scanner::Scanner() : pImpl(new Impl()) {}
Scanner::~Scanner() { delete pImpl; }

bool Scanner::Start(const ScanParams& params, const ScanCallbacks& callbacks) {
    if (pImpl->isRunning.load()) return false;
    if (pImpl->workerThread.joinable()) pImpl->workerThread.join();
    // Flag di-set sebelum thread dibuat, supaya Stop() langsung setelah Start() tidak hilang.
    pImpl->stopRequested.store(false);
    pImpl->isRunning.store(true);
    pImpl->workerThread = std::thread(&Scanner::Impl::Worker, pImpl, params, callbacks);
    return true;
}

void Scanner::Stop() {
    pImpl->Stop();
}

bool Scanner::IsRunning() const {
    return pImpl->isRunning.load();
}

} // namespace ReelKill
