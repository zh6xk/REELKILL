#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include "../src/core/Scanner.h"

using namespace std;
using namespace ReelKill;

int main(int argc, char* argv[]) {
    cout << "========================================" << endl;
    cout << "   REELKILL - Standalone Scan CLI Test   " << endl;
    cout << "========================================" << endl;

    if (argc < 4) {
        cout << "Usage: scan_cli.exe <videoPath> <outputDir> <playerName> [minSimilarity] [preSec] [postSec]" << endl;
        cout << "Example: scan_cli.exe \"D:\\match.mp4\" \"D:\\output\" \"proplayer\" 0.45 4.0 3.0" << endl;
        return 1;
    }

    ScanParams params;
    params.videoPath = argv[1];
    params.outputDir = argv[2];
    params.playerName = argv[3];

    if (argc >= 5) params.minSimilarity = stod(argv[4]);
    if (argc >= 6) params.preSec = stod(argv[5]);
    if (argc >= 7) params.postSec = stod(argv[6]);

    cout << "Video      : " << params.videoPath << endl;
    cout << "Output Dir : " << params.outputDir << endl;
    cout << "Player     : " << params.playerName << endl;
    cout << "Threshold  : " << (int)(params.minSimilarity * 100) << "%" << endl;
    cout << "----------------------------------------" << endl;

    Scanner scanner;
    ScanCallbacks callbacks;

    callbacks.onLog = [](const string& line) {
        cout << line << endl;
    };

    callbacks.onProgress = [](int percent, int frame, int total, double fps, int etaSec) {
        int m = etaSec / 60;
        int s = etaSec % 60;
        cout << "\r[PROGRESS] " << percent << "% (" << frame << "/" << total 
             << ") | FPS: " << fixed << setprecision(1) << fps 
             << " | ETA: " << setfill('0') << setw(2) << m << ":" << setfill('0') << setw(2) << s << flush;
    };

    callbacks.onClip = [](const FoundClip& clip) {
        cout << "\n>>> [CLIP FOUND] " << clip.start << "s -> " << clip.end 
             << "s (Skor: " << (int)(clip.score * 100) << "%)" << endl;
    };

    callbacks.onError = [](const string& err) {
        cerr << "\n[ERROR] " << err << endl;
    };

    callbacks.onDone = [](bool stopped, const vector<FoundClip>& clips) {
        cout << "\n----------------------------------------" << endl;
        cout << "[DONE] Scan selesai! Total klip: " << clips.size() 
             << " (Status: " << (stopped ? "Dihentikan" : "Sukses") << ")" << endl;
    };

    bool started = scanner.Start(params, callbacks);
    if (!started) {
        cerr << "[FATAL] Gagal memulai scanning (scanner busy / invalid params)" << endl;
        return 1;
    }

    // Keep CLI alive until scanner finishes
    while (scanner.IsRunning()) {
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    return 0;
}
