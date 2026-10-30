#include "Exporter.h"
#include <windows.h>
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

class Exporter::Impl {
public:
    std::atomic<bool> isExporting{false};
    std::atomic<bool> cancelRequested{false};
    std::thread exportThread;
    PROCESS_INFORMATION activePi{0};

    ~Impl() {
        Cancel();
    }

    void Cancel() {
        if (isExporting.load()) {
            cancelRequested.store(true);
            if (activePi.hProcess) {
                TerminateProcess(activePi.hProcess, 1);
            }
            if (exportThread.joinable()) {
                exportThread.join();
            }
            isExporting.store(false);
        }
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

    string cmdStr = args.str();
    vector<char> cmdBuffer(cmdStr.begin(), cmdStr.end());
    cmdBuffer.push_back(0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(NULL, &cmdBuffer[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        bool ok = (exitCode == 0);
        if (onDone) onDone(ok);
        return ok;
    }

    if (onDone) onDone(false);
    return false;
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

    pImpl->isExporting.store(true);
    pImpl->cancelRequested.store(false);

    if (pImpl->exportThread.joinable()) pImpl->exportThread.join();

    pImpl->exportThread = std::thread([this, inputVideo, outputFolder, clips, mergeToSingleFile, callbacks, ffmpegPath]() {
        int total = (int)clips.size();
        int successCount = 0;
        fs::path inVid(inputVideo);
        string baseName = inVid.stem().string();
        vector<string> exportedClipPaths;

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

            string cmdStr = args.str();
            vector<char> cmdBuffer(cmdStr.begin(), cmdStr.end());
            cmdBuffer.push_back(0);

            STARTUPINFOA si;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            ZeroMemory(&pImpl->activePi, sizeof(pImpl->activePi));

            if (CreateProcessA(NULL, &cmdBuffer[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pImpl->activePi)) {
                WaitForSingleObject(pImpl->activePi.hProcess, INFINITE);
                DWORD exitCode = 0;
                GetExitCodeProcess(pImpl->activePi.hProcess, &exitCode);
                CloseHandle(pImpl->activePi.hProcess);
                CloseHandle(pImpl->activePi.hThread);
                pImpl->activePi.hProcess = NULL;

                if (exitCode == 0) {
                    successCount++;
                    exportedClipPaths.push_back(outPath.string());
                }
            }
        }

        // Merge logic if requested (FFmpeg concat demuxer)
        if (mergeToSingleFile && successCount > 1 && !pImpl->cancelRequested.load()) {
            if (callbacks.onLog) callbacks.onLog("[MERGE] Menggabungkan semua klip menjadi satu file...");
            fs::path listTxtPath = fs::path(outputFolder) / (baseName + "_concat_list.txt");
            ofstream listFile(listTxtPath);
            if (listFile.is_open()) {
                for (const auto& cp : exportedClipPaths) {
                    string esc = cp;
                    replace(esc.begin(), esc.end(), '\\', '/');
                    listFile << "file '" << esc << "'" << endl;
                }
                listFile.close();

                fs::path mergedPath = fs::path(outputFolder) / (baseName + "_merged" + inVid.extension().string());
                stringstream concatArgs;
                concatArgs << "\"" << ffmpegPath << "\" -y -f concat -safe 0 -i \"" << listTxtPath.string()
                           << "\" -c copy \"" << mergedPath.string() << "\"";

                string concatCmd = concatArgs.str();
                vector<char> concatBuf(concatCmd.begin(), concatCmd.end());
                concatBuf.push_back(0);

                STARTUPINFOA csi;
                ZeroMemory(&csi, sizeof(csi));
                csi.cb = sizeof(csi);
                csi.dwFlags = STARTF_USESHOWWINDOW;
                csi.wShowWindow = SW_HIDE;
                ZeroMemory(&pImpl->activePi, sizeof(pImpl->activePi));

                if (CreateProcessA(NULL, &concatBuf[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &csi, &pImpl->activePi)) {
                    WaitForSingleObject(pImpl->activePi.hProcess, INFINITE);
                    CloseHandle(pImpl->activePi.hProcess);
                    CloseHandle(pImpl->activePi.hThread);
                    pImpl->activePi.hProcess = NULL;
                }
                std::error_code ec;
                fs::remove(listTxtPath, ec);
            }
        }

        bool fullyDone = (successCount == total && !pImpl->cancelRequested.load());
        pImpl->isExporting.store(false);

        if (callbacks.onDone) {
            if (fullyDone) {
                callbacks.onDone(true, "Berhasil mengekspor " + to_string(successCount) + " klip!");
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
