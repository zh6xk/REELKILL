# REELKILL: Panduan Upgrade UI dengan AI Agent

Tujuan: mengganti UI ImGui lama dengan UI web (WebView2) tanpa merusak mesin C++ (OCR, OpenCV, ffmpeg).

## 1. Struktur dan pemilik file

```
REELKILL/
  AutoCutCPP.cpp, include/, models/   <- BACKEND (C++). Jangan diubah oleh tugas UI.
  src/bridge/                         <- BARU: satu-satunya tempat C++ bicara dengan UI
  ui/                                 <- FRONTEND (index.html, styles.css, app.js, assets/)
  docs/AGENT_GUIDE.md                 <- panduan ini
```

Satu file, satu pemilik. Agent UI hanya boleh mengedit `ui/`. Agent backend hanya boleh mengedit `src/` dan kode C++ lama. `src/bridge/` diubah bersama, tapi hanya mengikuti kontrak di bawah.

## 2. Aturan anti-tabrakan (tempel di awal setiap sesi agent)

1. Satu tugas per sesi, satu branch git per tugas (`ui/phase-2-dashboard`). Commit sebelum mulai dan setelah selesai.
2. Komunikasi UI dan C++ hanya lewat pesan JSON (bagian 3). Dilarang: `ExecuteScript` untuk memanipulasi DOM, atau JS yang membaca memori/file langsung.
3. Jangan mengubah nama pesan, nama field, atau `id` elemen yang sudah ada. Kalau perlu field baru, tambahkan (opsional), jangan hapus atau ganti nama.
4. Field tak dikenal harus diabaikan di kedua sisi. Semua pesan membawa `"v":1`.
5. Operasi berat (scan, ekspor) jalan di worker thread. `PostWebMessageAsJson` hanya dari UI thread (kirim `PostMessage` ke window, lalu panggil dari handler-nya).
6. Teks dari backend (path, nama player, hasil OCR) selalu dimasukkan dengan `textContent`, tidak pernah `innerHTML`.
7. Tanpa framework, bundler, atau library baru kecuali diminta. UI harus tetap jalan dengan membuka `ui/index.html` di browser (Bridge otomatis memakai mock).
8. Jangan menulis ulang file dari nol. Edit seperlunya (diff kecil). Jangan memformat ulang file yang tidak disentuh.
9. Setelah selesai: jalankan checklist bagian 6, lalu laporkan file yang diubah dan alasannya.

## 3. Kontrak pesan (v1)

Bentuk: `{"v":1,"type":"...","payload":{...}}`

**UI → C++**

| type | payload | tindakan |
|---|---|---|
| `pickFile` / `pickFolder` | `{target:"video"\|"output"}` | dialog pilih file/folder |
| `startScan` | `{video,output,player,pre,post,roi:{x,y,w,h},roiPreset}` | mulai OCR scan |
| `stopScan` | `{}` | hentikan scan |
| `loadResults` | `{}` | pilih video + CSV hasil scan |
| `exportClips` | `{clips:[{start,end}]}` (detik) | ekspor lossless (ffmpeg) |
| `showInExplorer` | `{}` | buka folder output |
| `saveRoiPreset` / `deleteRoiPreset` | `{name,roi}` / `{name}` | kelola preset |

**C++ → UI**

| type | payload |
|---|---|
| `progress` | `{percent,frame,total,fps,etaSec}` |
| `log` | `{line}` |
| `clipFound` | `{start,end,score}` |
| `scanDone` | `{stopped?}` |
| `fileSelected` | `{target,path}` |
| `frameInfo` | `{width,height}` (resolusi video, untuk ROI) |
| `resultsLoaded` | `{videoUrl,duration,clips:[{start,end,label,score}]}` |
| `toast` / `error` | `{message}` |

## 4. Kerangka WebView2 (C++)

```cpp
// Setelah WebView2 siap:
webview->SetVirtualHostNameToFolderMapping(L"app.reelkill", L"ui",
    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
webview->SetVirtualHostNameToFolderMapping(L"media.reelkill", videoFolder,
    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);  // untuk <video>
webview->Navigate(L"https://app.reelkill/index.html");
webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
  [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) {
    wil::unique_cotaskmem_string json; a->get_WebMessageAsJson(&json);
    // parse -> kirim ke backend di worker thread
    return S_OK; }).Get(), &token);
// C++ -> UI: webview->PostWebMessageAsJson(L"{\"v\":1,\"type\":\"log\",\"payload\":{\"line\":\"...\"}}");
```

`videoUrl` untuk `resultsLoaded` berbentuk `https://media.reelkill/namafile.mp4`. Jika rekaman HEVC tidak bisa diputar, tampilkan pesan lewat `error` (jangan crash).

## 5. Tahapan dan prompt siap salin

Kerjakan berurutan, satu sesi agent per tahap.

**Tahap 0: pisahkan backend dari UI lama** (agent backend)
> Baca AutoCutCPP.cpp. Ekstrak logika scan dan ekspor ke modul terpisah (src/core/Scanner.*, src/core/Exporter.*) dengan antarmuka: Start(params, callbacks), Stop(), Export(clips). Callback: onProgress, onLog, onClip, onDone. Jangan ubah perilaku scan. UI ImGui lama harus tetap bisa dipakai lewat modul baru. Jangan sentuh folder ui/. Build harus lolos.

**Tahap 1: host WebView2** (agent backend)
> Buat src/bridge/WebHost.* yang membuka jendela WebView2 memuat ui/ lewat virtual host (bagian 4 docs/AGENT_GUIDE.md). Implementasikan parser pesan JSON v1 dan dispatcher ke modul core. Mulai dengan pesan: pickFile, pickFolder, startScan, stopScan, progress, log, clipFound, scanDone. Pertahankan UI ImGui lama sebagai opsi (flag `--legacy-ui`).

**Tahap 2: Dashboard** (agent UI, hanya ui/)
> Hubungkan Dashboard di ui/app.js ke pesan asli. Jangan ubah styles.css kecuali perlu, dan jangan ubah kontrak pesan. Pastikan tombol Mulai/Stop menyesuaikan status, error ditampilkan di status bar, dan UI tetap jalan dengan mock di browser.

**Tahap 3: Hasil Scan** (backend: loadResults, exportClips, resultsLoaded; lalu UI)
> Backend: implement loadResults (baca CSV, kirim resultsLoaded dengan videoUrl lewat virtual host media) dan exportClips (ffmpeg lossless, kirim toast/error). UI: pastikan klip terpilih, timeline, dan player sinkron. Batas: jangan ubah format CSV yang sudah ada.

**Tahap 4: ROI selector**
> Backend: kirim `frameInfo` dan satu frame preview (data URL JPEG, field baru opsional `previewUrl`) saat dialog ROI dibuka; simpan/hapus preset di file JSON. UI: tampilkan frame itu sebagai latar #roiStage dan hubungkan saveRoiPreset/deleteRoiPreset. Koordinat selalu dalam piksel video asli.

**Tahap 5: polish**
> Tanpa mengubah kontrak: rapikan transisi, state kosong/error, fokus keyboard, dan ukuran jendela minimum. Jangan menambah dependensi.

## 6. Checklist anti-bug (wajib sebelum merge)

- [ ] `ui/index.html` dibuka langsung di browser: tidak ada error di console, mock jalan.
- [ ] Tidak ada `innerHTML` dengan data dari backend; tidak ada `eval`.
- [ ] Nama pesan dan field sesuai tabel; tidak ada yang diganti nama.
- [ ] Scan dan ekspor tidak memblokir UI thread; Stop selalu bisa dipakai.
- [ ] Path dengan spasi dan karakter non-ASCII (mis. `D:\Rekaman Final\é.mp4`) tetap jalan.
- [ ] Menutup jendela saat scan berjalan menghentikan worker dengan bersih.
- [ ] Build C++ dan `git diff --stat` hanya menyentuh file milik tahap tersebut.

## 7. Prompt master (tempel pertama di setiap sesi)

> Kamu mengerjakan satu tahap dari upgrade UI REELKILL. Baca docs/AGENT_GUIDE.md dulu dan patuhi bagian 2 (aturan) dan 3 (kontrak pesan). Edit hanya file milik tahap ini. Jangan refactor di luar tugas, jangan ganti nama pesan/field/id yang ada, jangan tambah dependensi. Kerjakan perubahan sekecil mungkin, jelaskan rencana dulu sebelum menulis kode, lalu jalankan checklist bagian 6 dan laporkan file yang berubah. Kalau ada hal yang butuh mengubah kontrak, berhenti dan tanyakan, jangan diubah sendiri.
