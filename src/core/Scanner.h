#pragma once
#include <string>
#include <vector>
#include <functional>

namespace ReelKill {

struct RoiRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ScanParams {
    std::string videoPath;
    std::string outputDir;
    std::string playerName;
    double preSec = 4.0;
    double postSec = 3.0;
    RoiRect roi;
    bool hasCustomRoi = false;
    double minSimilarity = 0.45;
    // true  : (perilaku lama) salin video+CSV ke outputDir lalu HAPUS file asal.
    // false : video dan CSV tetap di folder asal; outputDir tidak dipakai scan.
    bool relocateResults = true;
};

struct FoundClip {
    double start = 0.0;
    double end = 0.0;
    std::string label = "kill";
    double score = 0.0;
};

struct ScanCallbacks {
    std::function<void(int percent, int frame, int total, double fps, int etaSec)> onProgress;
    std::function<void(const std::string& line)> onLog;
    std::function<void(const FoundClip& clip)> onClip;
    std::function<void(const std::string& message)> onError;
    std::function<void(bool stopped, const std::vector<FoundClip>& clips)> onDone;
    std::function<void(const void* matBgr)> onPreviewMat; // Optional for GUI preview
};

class Scanner {
public:
    Scanner();
    ~Scanner();

    bool Start(const ScanParams& params, const ScanCallbacks& callbacks);
    void Stop();
    bool IsRunning() const;

private:
    class Impl;
    Impl* pImpl;
};

} // namespace ReelKill
