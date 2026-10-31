#include "Exporter.h"
#include <windows.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <functional>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;
using namespace std;

namespace ReelKill {

static string ToDotString(double val) {
    stringstream ss;
    ss.imbue(locale("C"));
    ss << fixed << setprecision(3) << val;
    return ss.str();
}

static string GetModuleDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    string s(path);
    size_t pos = s.find_last_of("\\/");
    return (pos != string::npos) ? s.substr(0, pos) : "";
}

// Format baris daftar concat ffmpeg: file 'path' (tanda ' di path di-escape sebagai '\'' )
static string ConcatListLine(const string& path) {
    string p = path;
    replace(p.begin(), p.end(), '\\', '/');
    string out = "file '";
    for (char c : p) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

class Exporter::Impl {
public:
    std::atomic<bool> isExporting{false};
    std::atomic<bool> cancelRequested{false};
    std::thread exportThread;
    std::mutex piMutex;
    PROCESS_INFORMATION activePi{0};

    ~Impl() {
        Cancel();
    }

    // Selalu join thread yang masih joinable (juga setelah ekspor selesai),
    // supaya destruktor tidak memanggil std::terminate.
    void Cancel() {
        cancelRequested.store(true);
        {
            std::lock_guard<std::mutex> lk(piMutex);
            if (activePi.hProcess) {
                TerminateProcess(activePi.hProcess, 1);
            }
        }
        if (exportThread.joinable()) {
            exportThread.join();
        }
        isExporting.store(false);
    }

    // Jalankan satu proses ffmpeg dan tunggu. Return false jika proses gagal dibuat.
    bool Run(const string& cmd, DWORD& exitCode) {
        vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back(0);

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        if (!CreateProcessA(NULL, &buf[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(piMutex);
            activePi = pi;
            if (cancelRequested.load()) TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        {
            std::lock_guard<std::mutex> lk(piMutex);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            activePi.hProcess = NULL;
            activePi.hThread = NULL;
        }
        return true;
    }
};

Exporter::Exporter() : pImpl(new Impl()) {}
Exporter::~Exporter() { delete pImpl; }

bool Exporter::CheckFfmpeg(const string& exeDir, string& outFfmpegPath) {
    fs::path dir = exeDir.empty() ? fs::path(GetModuleDir()) : fs::path(exeDir);
    fs::path ffmpeg = dir / "ffmpeg.exe";
    if (fs::exists(ffmpeg)) {
        outFfmpegPath = ffmpeg.string();
        return true;
    }
    return false;
}

bool Exporter::ExportSingle(const string& inputVideo, const string& outputClipPath,
                            double startSec, double endSec,
                            const std::function<void(bool ok)>& onDone) {
    string ffmpegPath;
    if (!CheckFfmpeg("", ffmpegPath)) {
        if (onDone) onDone(false);
        return false;
    }

    double duration = endSec - startSec;
    if (duration < 0.1) duration = 0.1;

    stringstream args;
    args << "\"" << ffmpegPath << "\" -y -ss " << ToDotString(startSec)
         << " -i \"" << inputVideo << "\" -t " << ToDotString(duration)
         << " -c copy -avoid_negative_ts 1 \"" << outputClipPath << "\"";

    DWORD exitCode = 1;
    bool ran = pImpl->Run(args.str(), exitCode);
    bool ok = ran && exitCode == 0;
    if (onDone) onDone(ok);
    return ok;
}

bool Exporter::ExportBatch(const string& inputVideo, const string& outputFolder,
                           const vector<ExportClip>& clips,
                           const ExportCallbacks& callbacks) {
    return ExportBatch(inputVideo, outputFolder, clips, false, callbacks);
}

bool Exporter::ExportBatch(const string& inputVideo, const string& outputFolder,
                           const vector<ExportClip>& clips,
                           bool mergeToSingleFile,
                           const ExportCallbacks& callbacks) {
    if (pImpl->isExporting.load()) return false;
    if (clips.empty()) {
        if (callbacks.onError) callbacks.onError("Daftar klip kosong.");
        if (callbacks.onDone) callbacks.onDone(false, "Tidak ada klip untuk diekspor.");
        return false;
    }

    string ffmpegPath;
    if (!CheckFfmpeg("", ffmpegPath)) {
        if (callbacks.onError) callbacks.onError("ffmpeg.exe tidak ditemukan di folder aplikasi.");
        if (callbacks.onDone) callbacks.onDone(false, "ffmpeg.exe tidak ditemukan di folder aplikasi.");
        return false;
    }

    if (pImpl->exportThread.joinable()) pImpl->exportThread.join();
    pImpl->cancelRequested.store(false);
    pImpl->isExporting.store(true);

    pImpl->exportThread = std::thread([this, inputVideo, outputFolder, clips, mergeToSingleFile, callbacks, ffmpegPath]() {
        int total = (int)clips.size();
        int successCount = 0;
        fs::path inVid(inputVideo);
        string baseName = inVid.stem().string();
        vector<string> exportedClipPaths;
        bool mergeOk = true;
        string mergedName;

        for (int i = 0; i < total; i++) {
            if (pImpl->cancelRequested.load()) {
                if (callbacks.onLog) callbacks.onLog("[CANCEL] Ekspor dibatalkan.");
                break;
            }

            const auto& clip = clips[i];
            double duration = clip.end - clip.start;
            if (duration < 0.1) duration = 0.1;

            string outClipName = baseName + "_clip_" + to_string(i + 1) + inVid.extension().string();
            fs::path outPath = fs::path(outputFolder) / outClipName;

            if (callbacks.onProgress) {
                callbacks.onProgress(i + 1, total, "Mengekspor klip #" + to_string(i + 1));
            }
            if (callbacks.onLog) {
                callbacks.onLog("[EXPORT] Memotong: " + outClipName + " (" + ToDotString(clip.start) + "s -> " + ToDotString(clip.end) + "s)");
            }

            stringstream args;
            args << "\"" << ffmpegPath << "\" -y -ss " << ToDotString(clip.start)
                 << " -i \"" << inputVideo << "\" -t " << ToDotString(duration)
                 << " -c copy -avoid_negative_ts 1 \"" << outPath.string() << "\"";

            DWORD exitCode = 1;
            if (!pImpl->Run(args.str(), exitCode)) {
                if (callbacks.onError) callbacks.onError("Gagal menjalankan ffmpeg untuk klip #" + to_string(i + 1));
                continue;
            }
            if (exitCode == 0) {
                successCount++;
                exportedClipPaths.push_back(outPath.string());
            } else {
                // Hapus file parsial (mis. karena dibatalkan atau ffmpeg error)
                std::error_code ec;
                fs::remove(outPath, ec);
                if (!pImpl->cancelRequested.load() && callbacks.onError) {
                    callbacks.onError("ffmpeg gagal untuk klip #" + to_string(i + 1));
                }
            }
        }

        // Gabungkan semua klip (FFmpeg concat demuxer) bila diminta
        if (mergeToSingleFile && successCount > 1 && !pImpl->cancelRequested.load()) {
            if (callbacks.onLog) callbacks.onLog("[MERGE] Menggabungkan semua klip menjadi satu file...");
            mergeOk = false;
            fs::path listTxtPath = fs::path(outputFolder) / (baseName + "_concat_list.txt");
            ofstream listFile(listTxtPath);
            if (listFile.is_open()) {
                for (const auto& cp : exportedClipPaths) {
                    listFile << ConcatListLine(cp) << "\n";
                }
                listFile.close();

                mergedName = baseName + "_merged" + inVid.extension().string();
                fs::path mergedPath = fs::path(outputFolder) / mergedName;
                stringstream concatArgs;
                concatArgs << "\"" << ffmpegPath << "\" -y -f concat -safe 0 -i \"" << listTxtPath.string()
                           << "\" -c copy \"" << mergedPath.string() << "\"";

                DWORD exitCode = 1;
                if (pImpl->Run(concatArgs.str(), exitCode) && exitCode == 0) {
                    mergeOk = true;
                } else {
                    std::error_code ec;
                    fs::remove(mergedPath, ec);
                    if (callbacks.onError) callbacks.onError("Gagal menggabungkan klip. Klip terpisah tetap tersimpan.");
                }
            } else if (callbacks.onError) {
                callbacks.onError("Gagal membuat daftar concat di folder output.");
            }
            std::error_code ec;
            fs::remove(listTxtPath, ec);
        }

        bool cancelled = pImpl->cancelRequested.load();
        bool fullyDone = (successCount == total && !cancelled && mergeOk);
        pImpl->isExporting.store(false);

        if (callbacks.onDone) {
            if (fullyDone) {
                string msg = "Berhasil mengekspor " + to_string(successCount) + " klip!";
                if (!mergedName.empty()) msg += " Gabungan: " + mergedName;
                callbacks.onDone(true, msg);
            } else {
                callbacks.onDone(false, "Selesai dengan " + to_string(successCount) + "/" + to_string(total) + " klip berhasil.");
            }
        }
    });

    return true;
}

void Exporter::Cancel() {
    pImpl->Cancel();
}

bool Exporter::IsExporting() const {
    return pImpl->isExporting.load();
}

} // namespace ReelKill
