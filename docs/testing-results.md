# Hasil pengujian

Dokumen ini mencatat apa yang sudah dan belum teruji.

## Lingkungan

| Item | Nilai |
|---|---|
| Node | ESP32 DevKit, arduino-esp32 3.3.7 |
| Cloud | ThingsBoard Cloud |
| VPS | Azure B2ats_v2 (2 vCPU, 1 GiB), Ubuntu 24.04 |
| Bridge | Python 3.12, paho-mqtt 2.x, PyCryptodome |

## Ringkasan

| No | Pengujian | Hasil | Catatan |
|---|---|---|---|
| T1 | Multi-node WiFi (3 node) tanpa tabrakan data | Lulus | Data masuk ke device terpisah, selisih antar node sekitar 1 detik. Diuji pada arsitektur UDP dan gateway sebelum migrasi ke WiFi langsung |
| T2 | Provisioning otomatis | Lulus | Device baru terbentuk, token tersimpan di NVS |
| T3 | Jadwal valve dan override | Lulus setelah 2 perbaikan | Atribut `enabled` bertipe Server tidak sampai ke device; update parsial gagal diterapkan. Override mengalahkan jadwal |
| T4 | Sinkron waktu NTP setelah reboot | Lulus setelah perbaikan | NTP tidak dicek hasilnya, jam jatuh ke 07:00 (epoch 0 + UTC+7). Sekarang retry 10x dan hasilnya dicatat |
| T5 | Telemetri AES end-to-end (WiFi) | Lulus | Lihat bagian "Pengamatan T5" |
| T6 | Bridge sebagai service systemd | Lulus | Insiden crash-loop dicatat di dokumen keamanan |
| T7 | Backlog volume | Diuji pada pemasangan sekitar 1 hari | Snapshot tersimpan di flash; uji offline terkontrol belum didokumentasikan |
| T8 | Interval adaptif (5 detik saat ada aliran) | Terimplementasi, belum terverifikasi | Hasil uji lapangan belum didokumentasikan |
| T9 | Buzzer pemborosan | Sebagian | Backend berhasil mengirim RPC `soundBuzzer` (tercatat di audit log ThingsBoard); handler firmware terimplementasi; uji dengan buzzer fisik belum |
| T10 | Kalibrasi 3 titik di semua node | Belum | Fitur tersedia, belum diterapkan ke semua node |
| T11 | LoRa: multi-node, tabrakan, AES | Belum diuji | Menunggu PCB dan power supply stabil |
| T12 | Pengujian keamanan oleh pihak lain | Belum ada hasil | Dijadwalkan atau berjalan |

## Pengamatan T5: telemetri terenkripsi

Dua cara melihat momen "terenkripsi lalu terdekripsi", di VPS:

```bash
# Isi pesan di broker (Base64 acak)
mosquitto_sub -h localhost -t 'sensor/encrypted/#' -v

# Log bridge: payload terenkripsi, hasil decrypt, hasil forward
sudo journalctl -u pamsimas-bridge -f
sudo journalctl -u pamsimas-bridge --since "10 minutes ago" --no-pager | grep -E "TERIMA payload|HASIL decrypt"
```

Hasil dari satu cuplikan log (bukan benchmark):

- Isi JSON hasil dekripsi hampir identik antar pesan, tetapi ciphertext berbeda total sejak karakter pertama. Ini menunjukkan IV acak per pesan.
- `node_ts` 21:32:26 (WIB) tercatat bridge pada 14:32:27 (UTC) dan diteruskan ke ThingsBoard pada 14:32:28, jadi sekitar 1 sampai 2 detik dari node ke ThingsBoard. Selisih 7 jam hanya perbedaan zona waktu antara node (WIB) dan journal VPS (UTC).
- Pesan dari lebih dari satu node (misalnya Node 01 dan Node 06) berhasil didekripsi dan diteruskan, termasuk pencarian token otomatis.

## Masalah yang ditemui dan solusinya

| Masalah | Penyebab | Solusi |
|---|---|---|
| Bootloop `Detected size(4096k) smaller than ... (8192k)` | Flash Size di Arduino IDE tidak sesuai chip | Set Flash Size 4MB |
| `Failed to provision device!` | Nama device sudah ada dan bentrok | Gunakan nama baru atau hapus device lama |
| Jam node 07:00 setelah reboot | Hasil NTP tidak dicek | Retry 10x dan log hasilnya |
| Jadwal valve tidak menutup | Atribut bertipe Server; update parsial diabaikan | Ubah ke Shared; gabungkan update parsial |
| Dekripsi AES di Rule Engine gagal | Sandbox TBEL memblokir `javax.crypto` | Mosquitto dan bridge Python |
| Linker error `AES_init_ctx_iv` tidak ditemukan | `aes.c` (C) dipanggil dari C++ tanpa `extern "C"` | Bungkus `#include "aes.h"` dengan `extern "C"` |
| `base64_encode was not declared` | API library Base64 berbeda antar versi | Fungsi base64 ditulis sendiri |
| Bridge crash-loop | Cache token JSON rusak | Fallback pada `load_token_cache`, pengecekan status service |

## Bukti yang perlu ditambahkan

Simpan di folder `images/` dengan data sensitif sudah disamarkan (IP, token, email, ID device, nama pelanggan):

- `images/architecture/` : diagram arsitektur hasil draw.io
- `images/dashboard/` : dashboard ThingsBoard (grafik, tabel, kontrol valve, form jadwal)
- `images/hardware/` : foto node dan rangkaian
- Cuplikan log bridge dan `mosquitto_sub` dari T5
