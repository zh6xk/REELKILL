# Perbaikan pada REELKILL_full_project_v2_0.zip

Perbaikan dibuat dari review statis (tanpa build MSVC). Lihat status uji di bagian bawah.

## Diperbaiki
1. WebHost.cpp: lambda `onClip` dan `onDone` disesuaikan dengan `ScanCallbacks` di Scanner.h (sebelumnya compile error).
2. Exporter.cpp: ditambah `<thread> <atomic> <mutex> <algorithm> <functional>`.
3. Scanner dan Exporter: `join()` dilakukan jika thread masih joinable, juga setelah scan/ekspor selesai (sebelumnya `std::terminate` saat objek dihancurkan).
4. Scanner::Start men-set `isRunning=true` dan `stopRequested=false` sebelum thread dibuat (sebelumnya Stop() tepat setelah Start() bisa hilang).
5. `ScanParams::relocateResults` (default `true` = perilaku lama UI lama). Jalur WebUI memakai `false`, jadi video asli tidak lagi disalin lalu dihapus dan path video tetap valid.
6. WebHost: `HandleExportClips` tidak lagi memakai `std::thread().detach()`, menghormati folder Output dari startScan, dan membaca opsi `"merge"` (default `true`).
7. Exporter: tanda `'` di path aman untuk daftar concat; exit code merge dicek dan kegagalan dilaporkan lewat `onError`; file klip parsial dihapus; kegagalan `CreateProcess` dilaporkan; akses `activePi` dilindungi mutex.
8. docs/AGENT_GUIDE.md: kontrak `exportClips` mendapat field opsional `merge`.

## Tidak diubah (sengaja)
- Versi tetap v2.0, folder `ui/` tidak disentuh.
- `minSimilarity` di jalur WebUI masih 0.45 (belum ada slider di UI web).
- UI web belum punya kontrol untuk `merge` (default tetap menggabung).

## Status uji
- Diuji: logika Exporter baru (selesai, cancel, destruktor saat berjalan, ekspor berulang, escape concat, ffmpeg hilang) dengan stub API Windows di Linux, termasuk ThreadSanitizer. Bersih.
- BELUM diuji: build MSVC seluruh proyek, Scanner.cpp dan WebHost.cpp (perubahan kecil, belum dikompilasi), perilaku saat berjalan di Windows (scan, STOP, tutup jendela, ekspor ffmpeg asli, perbandingan CSV).
