# Setup CI (GitHub Actions)

## 1. Pasang file
Salin ke root repo (satu level dengan `AutoCutCPP.cpp`): `CMakeLists.txt`, `.github/workflows/build-windows.yml`, dan folder `docs/`.

## 2. Job pertama langsung jalan
`core-msvc-check` mengompilasi `Exporter.cpp` dengan MSVC asli tanpa dependensi apa pun. Ini bukti build nyata pertama.

## 3. Bundle dependensi untuk build penuh
Zip tidak berisi library pihak ketiga, jadi buat `third_party.zip` dari PC lo (folder library project Visual Studio yang sudah jalan):

```
third_party.zip
  rapidocr/   RapidOcrOnnx.lib, RapidOcrOnnx.dll, onnxruntime.dll (+ DLL pendamping lain)
  mpv/        include/mpv/client.h (dan header mpv lain), mpv.lib (atau libmpv.lib), libmpv-2.dll
```
Lalu di GitHub: Releases -> Draft new release -> tag `deps-v1` -> lampirkan `third_party.zip` -> Publish.

Peringatan: di repo publik, asset release ikut publik. Pakai repo privat, atau pastikan lisensi library yang dibundel (mis. libmpv GPL/LGPL, onnxruntime, OpenCV) mengizinkan redistribusi.

## 4. Sesuaikan
- `OPENCV_VERSION` di workflow harus sama dengan versi di Visual Studio lo (nama file `opencv_world4xx.lib`).
- OpenCV, WebView2 SDK diunduh otomatis. URL OpenCV dan perilaku installer-nya belum diverifikasi.
- Subsystem diasumsikan Windows dengan entry `main` (lihat `CMakeLists.txt`). Ubah jika project lo console.

## 5. Baca hasil
Tab Actions -> run terbaru. Job `build-full` mencetak 150 baris terakhir log build, dan artefak `build-log` berisi log lengkap. Salin error MSVC persisnya ke gue atau agent.
Hasil build ada di artefak `REELKILL-win-x64` (exe, DLL, `ui/`, `models/`). `ffmpeg.exe` tidak disertakan: letakkan sendiri di folder exe.
