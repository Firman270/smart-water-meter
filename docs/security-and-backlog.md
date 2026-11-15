# Keamanan dan ketahanan data

Dokumen ini mencatat batasan desain yang diketahui. Ini bukan hasil VAPT formal.

## Tujuan

- Kerahasiaan data konsumsi air per pelanggan.
- Keaslian perintah valve (perintah tutup punya dampak fisik).
- Ketersediaan aliran data dari node sampai dashboard.

## Perlindungan per segmen

| Segmen | Mekanisme | Melindungi | Tidak melindungi |
|---|---|---|---|
| Node ke Mosquitto | AES-128-CBC pada payload; MQTT 1883 tanpa TLS | Kerahasiaan isi | Integritas, keaslian, replay, metadata (topik memuat ID node, ukuran, waktu kirim) |
| Mosquitto ke bridge | localhost, tanpa TLS | - | - |
| Bridge ke ThingsBoard (MQTT) | TLS 8883, sertifikat tidak diverifikasi (`CERT_NONE`, dapat diubah lewat `TB_TLS_VERIFY`) | Penyadapan pasif | MITM aktif |
| Bridge ke ThingsBoard (REST) | HTTPS dengan verifikasi bawaan `requests` | Penyadapan, MITM | - |
| Node dan ThingsBoard (RPC, atribut, provisioning) | TLS 8883, `setInsecure()` | Penyadapan pasif | MITM aktif; RPC tidak dienkripsi di level aplikasi |
| Node ke gateway (LoRa) | AES-128-CBC jika `USE_AES 1` | Kerahasiaan isi | Belum diuji; batasan sama seperti baris pertama |
| Gateway ke ThingsBoard | TLS 8883, `setInsecure()` | Penyadapan pasif | MITM aktif |

TLS dan AES melindungi hal yang berbeda: TLS melindungi saluran, AES melindungi isi pesan. Data kembali berupa plaintext di memori bridge dan di database ThingsBoard.

## Temuan dan rencana perbaikan

| No | Temuan | Dampak | Perbaikan yang direncanakan |
|---|---|---|---|
| 1 | Mosquitto anonim (`allow_anonymous true`), port 1883 terbuka untuk semua sumber | Siapa pun bisa publish: banjir pesan, replay paket lama, menebak ID node dari topik | `password_file` dan ACL per node, batasi sumber di NSG, rate limit |
| 2 | AES-CBC tanpa MAC | Tidak ada jaminan integritas; IV dapat dimodifikasi sehingga blok plaintext pertama berubah terarah; replay tidak terdeteksi | Pindah ke AES-GCM atau encrypt-then-MAC (HMAC-SHA256), counter atau timestamp untuk menolak replay |
| 3 | Satu kunci AES untuk semua node | Kompromi satu node membuka semua | Kunci per node (turunan dari kunci induk dan `NODE_ID`) dan rotasi |
| 4 | Kunci tertanam di firmware; flash encryption dan secure boot tidak diaktifkan pada prototipe | Kunci dapat diekstrak dari flash | Aktifkan flash encryption dan secure boot; eksplorasi STM32U585 (TrustZone) sebagai penyimpan kunci |
| 5 | TLS tanpa verifikasi sertifikat (firmware `setInsecure()`, bridge `CERT_NONE`) | MITM aktif dimungkinkan | Pasang CA root di firmware (`setCACert`), `TB_TLS_VERIFY=true` di bridge |
| 6 | RPC valve dan buzzer hanya dilindungi TLS dan token | Pemegang token atau akun tenant dapat mengirim perintah; tidak ada tanda tangan perintah | Perintah bertanda tangan atau allow-list; audit log ThingsBoard sudah mencatat RPC |
| 7 | Bridge memakai akun tenant ThingsBoard untuk mencari token | Kredensial berhak luas tersimpan di server | Akun dengan hak minimal atau API key; `.env` izin 600 |
| 8 | `node_token_cache.json` berisi token plaintext | Kebocoran file membuka akses device | Izin file 600 (sudah diterapkan); simpan di secret store |
| 9 | Sumber entropi IV | IV yang dapat diprediksi melemahkan CBC | Verifikasi sumber `random()` pada core Arduino-ESP32, gunakan `esp_random()` langsung |

## Backlog dan ketahanan data

### Mekanisme backlog di node

- Snapshot volume akumulasi disimpan di flash (NVS) pada 11:59 dan 23:59.
- Kapasitas 20 entri, FIFO: entri tertua dibuang jika penuh.
- Saat koneksi tersedia, satu entri dikirim per siklus (interval idle), ditandai `backlog:1`, lalu dihapus dari flash.
- `node_ts` berisi jam snapshot, sedangkan timestamp di ThingsBoard adalah waktu terima. Grafik time-series karenanya tidak menampilkan waktu kejadian asli. Perbaikan: kirim `ts` epoch milidetik.

### Skenario kehilangan data

| Skenario | Perilaku saat ini | Perbaikan |
|---|---|---|
| Node offline | Volume tetap dihitung di node; hanya 2 snapshot per hari yang dikirim ulang (resolusi kasar) | Snapshot lebih sering, simpan seri debit ringkas |
| Bridge atau Mosquitto mati | Pesan QoS 0 tanpa sesi persisten hilang; node tidak membuat antrean | QoS 1 dan sesi persisten, antrean dengan retry di bridge, health check dan alert |
| ThingsBoard tidak terjangkau dari bridge | Publish gagal, data dibuang (tercatat `GAGAL` di log) | Antrean lokal di bridge |
| `node_token_cache.json` rusak | Dulu: service crash-loop. Sekarang: file diganti nama `.rusak` dan dibuat ulang | Monitoring status service |

### Insiden: bridge crash-loop

Setelah `node_token_cache.json` diedit manual, koma sisa di akhir objek membuat `json.load` gagal saat start. Service `systemd` dengan `Restart=always` dan `RestartSec=5` terus restart tanpa berhenti (counter mencapai sekitar 68.000, setara kira-kira 4 hari). Selama itu telemetri tidak diteruskan ke ThingsBoard. Perbaikan: `load_token_cache` menangani file rusak, log `journalctl` diperjelas, dan pengecekan status dimasukkan ke prosedur operasional.
