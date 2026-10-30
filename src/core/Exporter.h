#pragma once
#include <string>
#include <vector>
#include <functional>

namespace ReelKill {

struct ExportClip {
    double start = 0.0;
    double end = 0.0;
    std::string label;
};

struct ExportCallbacks {
    std::function<void(int current, int total, const std::string& status)> onProgress;
    std::function<void(const std::string& line)> onLog;
    std::function<void(const std::string& message)> onError;
    std::function<void(bool success, const std::string& message)> onDone;
};

class Exporter {
public:
    Exporter();
    ~Exporter();

    static bool CheckFfmpeg(const std::string& exeDir, std::string& outFfmpegPath);
    bool ExportSingle(const std::string& inputVideo, const std::string& outputClipPath,
                      double startSec, double endSec,
                      const std::function<void(bool ok)>& onDone = nullptr);
    bool ExportBatch(const std::string& inputVideo, const std::string& outputFolder,
                     const std::vector<ExportClip>& clips,
                     const ExportCallbacks& callbacks);
    bool ExportBatch(const std::string& inputVideo, const std::string& outputFolder,
                     const std::vector<ExportClip>& clips,
                     bool mergeToSingleFile,
                     const ExportCallbacks& callbacks);
    void Cancel();
    bool IsExporting() const;

private:
    class Impl;
    Impl* pImpl;
};

} // namespace ReelKill
