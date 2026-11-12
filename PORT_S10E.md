# Port beyond0lte — Galaxy S10e (SM-G970F, Exynos 9820, 6/128)

Dokumen ini menjelaskan port proyek UEFI ini dari **S10+ (`beyond2lte`)** ke
**S10e (`beyond0lte`)**. Ditulis untuk pembaca yang sudah paham `DEVIR.md`;
di sini hanya **perbedaan** dan **apa yang belum terbukti**.

> Status jujur: ini **port tingkat sumber**. Belum ada satu pun baris di sini
> yang sudah diuji di perangkat S10e. Yang bisa diturunkan dari SoC/dokumen
> sudah diturunkan, dan DTB asli dari firmware stok sudah dipasang. Sisanya
> (terutama `"Display Reserved"`) masih **VERIFIKASI** dan hanya bisa ditutup
> dengan satu kali flash + satu kali boot.

---

## 1. Ringkasan

S10e, S10, dan S10+ **memakai SoC yang sama** (Exynos 9820 / S5E9820) dan
**S-Boot satu keluarga**. Konsekuensinya mayoritas proyek tidak berubah:

| Lapisan | Untuk S10e |
|---|---|
| `Silicon/Samsung/S5E9820Pkg` (UFS, Gpio, Pmic, Usi, ResetSystem) | **sama**, dipakai apa adanya |
| `Silicon/Samsung/SamsungPkg` (UfsDxe, GpioDxe, KeypadDxe, …) | **sama** |
| ACPI `silicium-acpi-overlay/SoCs/Samsung/S5E9820/` | **sama** (level SoC, bukan level board) |
| Driver diagnostik `B2L*` (Goster, BootTani, RtHook, Seh, GicSus, Touch) | **sama**, disalin ke paket baru |
| `MemoryMapLib` | **BERBEDA** (6 GiB vs 8 GiB) |
| `PcdFrameBuffer*` | **BERBEDA** (1080×2280 vs 1440×3040) |
| `Resources/DTBs/beyond0lte.*` | **BERBEDA** — DTB asli S10e (dari firmware stok), lihat §5 |
| `KeypadDeviceLib` GPIO | **sama** — terverifikasi vs DTB asli (§6) |

Pembagian build Mu-Silicium tidak memakai daftar pusat: `build_uefi.py`
mencari `Platforms/*/<nama>Pkg` dan membaca `Resources/Configs/<nama>.toml`.
Jadi port ini cukup menambah dua direktori.

---

## 2. Yang berbeda, dan alasannya

### 2.1 Memori — 6 GiB, bukan 8 GiB

`RamManagerLib` (upstream, kita tidak mengubahnya) menentukan ukuran RAM dari
register S-Boot, bukan dari PCD dan bukan dari DTB:

```c
MemorySize = MmioRead64 (NS_IRAM + 0x848);      // S-Boot'un yazdigi boyut
MemoryEnd  = BASE_32GB + MemorySize;            // 0x800000000 + RAM
```

Untuk S10+ 8 GiB: `MemoryEnd = 0xA00000000` → bank `0x980000000` ada.
Untuk S10e 6 GiB: `MemoryEnd = 0x980000000` → **bank `0x980000000` tidak ada.**

`MemoryMapLib.c` milik `beyond0ltePkg` karena itu **menghapus** satu descriptor
`Upper bank guard` (`0x980000000`, 2 GiB) milik versi S10+. Memetakan DRAM yang
tidak ada sebagai `Reserved` akan memberi UEFI wilayah hantu.

Peta akhir (bandingkan `beyond2ltePkg/Library/MemoryMapLib/MemoryMapLib.c`):

```
0x80000000  .. 0x100000000   (2 GiB)  rendah, reserv + guard   (sama)
0x880000000 .. 0x900000000   (2 GiB)  guard                     (sama)
0x900000000 .. 0x980000000   (2 GiB)  ← satu-satunya RAM Windows
0x980000000 ..              TIDAK ADA  (di S10+ ini bank keempat)
```

Dengan begitu Windows/WinPE tetap mendapat **2 GiB**, sama seperti konfigurasi
protektif di S10+. Jumlah bank yang dibaca S-Boot otomatis 6 GiB, jadi tidak ada
PCD yang perlu ditimpa (lihat komentar di `beyond0lte.dsc`).

> **VERIFIKASI (1):** di EFI Shell, `memmap` harus menunjukkan total ≈ 6 GiB
> dan **tidak** boleh ada baris di `0x980000000` ke atas. `dmem 208 8`
> (NS_IRAM+0x848) harus memberi `0x180000000` (6 GiB).

### 2.2 Layar — 1080×2280, bukan 1440×3040

`SimpleFbDxe` tidak menyetel ulang panel; ia hanya menulis ke region
`"Display Reserved"` yang sudah diinisialisasi S-Boot. Yang kita berikan
hanyalah resolusi dan kedalaman warna:

```dsc
gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2280
```

Angka ini bukan tebakan — ada di DTB stok yang sudah kita punya. Base DTB
Samsung bersifat universal (memuat node panel ketiga varian), lihat:

```dts
s6e3fa7_beyond0 {          /* S10e */
        resolution = <0x438 0x8e8>;   /* 1080 x 2280 */
};
```

Resolusi S10+ (`s6e3ha9*`) juga ada di file yang sama, dan `beyond2lte`
memakai 1440×3040. Jadi satu-satunya perubahan adalah dua PCD di atas.

> **VERIFIKASI (2):** apakah region `"Display Reserved"` S10e benar-benar
> `0xCA000000` sebesar 20 MiB (`0x1400000`)? Nilai ini SAMA di `beyond1lte`
> (upstream) dan `beyond2lte`, jadi sangat mungkin sama juga di S10e — tetapi
> S10e adalah satu-satunya varian dengan panel 1080 wide, dan stride DECON
> yang berbeda bisa menggeser kebutuhan ukuran. Gejala kalau salah: gambar
> bergeser/terpotong diagonal, atau sebagian ekran berisi sisa data lama.

### 2.3 FD / stack / heap

Tidak berubah dari S10+:

| Region | Alamat | Ukuran |
|---|---|---|
| UEFI FD (BootShim menyalin ke sini) | `0xC9000000` | 4 MiB |
| UEFI Stack | `0xC9400000` | 256 KiB |
| Display Reserved | `0xCA000000` | 20 MiB |
| DXE Heap | `0xCB400000` | 60 MiB |
| B2L EL2 / B2L RT Log | `0xCF000000` / `0xCF040000` | – |

`FD_BASE` adalah **pilihan kita**, bukan alamat yang dipaksakan S-Boot:
BootShim yang memindahkan FD ke sana. `0xC9000000` dipakai dua port S10 di
atas, dan `tools_uefi_layout_check.py` memverifikasi FD tidak menabrak stack
maupun framebuffer pada setiap build.

### 2.4 UFS, ACPI, SMP, PSCI

Tidak ada yang perlu diport. UFS berada di `S5E9820Pkg/Library/PlatformUfsLib`
(level SoC, termasuk patch `TimerLib` dan kalibrasi CAL) dan ACPI di
`silicium-acpi-overlay/SoCs/Samsung/S5E9820/` (juga level SoC). Keduanya
dipakai bersama oleh `beyond0ltePkg`.

---

## 3. File yang ditambahkan / diubah

**Ditambahkan**

```
mu-silicium-overlay/Platforms/Samsung/beyond0ltePkg/   (salinan beyond2ltePkg + edit di §2)
mu-silicium-overlay/Resources/Configs/beyond0lte.toml
mu-silicium-overlay/Resources/DTBs/beyond0lte.dts/.dtb  ← DTB asli S10e, lihat §5
tools_dtb_s10e.sh                                        ← pembuat DTB asli
tools_dtbo_extract.py                                    ← pengurai dtbo.img/dt.img
PORT_S10E.md                                             ← dokumen ini
```

**Diubah (generik, tidak mengubah perilaku beyond2lte)**

| File | Perubahan |
|---|---|
| `yap.sh` | Opsi `-d/--device` (default `beyond2lte`), semua jalur/keluaran memakai `$DEV` |
| `tools_uefi_layout_check.py` | Opsi `--device` |
| `tools_reserved_memory_audit.py` | Opsi `--device` |
| `cache_esp.sh`, `tools_windisk.sh`, `tools_odin_winpe_bundle.sh` | Opsi `-d/--device`; nama berkas `$DEV-winpe-ufs.tar` dan folder Desktop `$DEV-flash` |
| `tools_b2lfb.sh`, `tools_b2lufs.sh` | `-d/--device`; nama tampilan tanda tangan & `DisplayName` ikut `$DEV` (sertifikat tetap sama) |
| `tests/test_boot_tani_safety.sh` | Memeriksa semua paket `*Pkg` (atau satu cihaz sebagai argumen) |

Pemakaian lama tetap jalan tanpa argumen:

```bash
bash yap.sh                       # beyond2lte (S10+)
bash yap.sh -d beyond0lte --clean # beyond0lte (S10e)
```

---

## 4. Build dan flash

```bash
# 0) Sumber: lihat BUILD.md (WSL) atau BUILD_LINUX.md (Linux native,
#    tanpa sudo). Overlay disalin oleh yap.sh sendiri.

# 1) (opsional) perbarui DTB asli. Repo sudah memuat DTB asli S10e
#    (EUR OPEN 25); langkah ini hanya bila Anda ingin regenerasi:
bash tools_dtb_s10e.sh --tar AP_G970F....tar.md5
python3 tools_reserved_memory_audit.py --device beyond0lte --check

# 2) Build
bash yap.sh -d beyond0lte --acpi --clean
```

Status: **build ini sudah terbukti berhasil** di Linux native (clang 23 /
`CLANGPDB`, target `aarch64-unknown-windows-msvc`). Hasil: `Mu-beyond0lte.img`
(4,4 MB), `uefi-boot.tar`, disalin ke `out/`. Isi boot image dan catatan
kompiler: `BUILD_LINUX.md`.

### 4.1 Bundel flash siap kirim

```bash
bash tools_flash_bundle.sh -d beyond0lte
```

Menghasilkan `out/beyond0lte-flash/` (dan `.zip`) berisi:

| Berkas | Untuk |
|---|---|
| `uefi-boot.tar` | Odin, slot **AP** (hanya berisi `boot.img`) |
| `Mu-beyond0lte.img` | `heimdall flash --BOOT ...` di Linux |
| `BACA-INI.txt` | langkah Odin/heimdall, verifikasi, rollback |
| `SHA256SUMS` | `sha256sum -c SHA256SUMS` |

Skrip memverifikasi bahwa `tar` hanya memuat `boot.img` **dan** byte-nya sama
persis dengan `Mu-beyond0lte.img`, jadi tidak mungkin tertukar dengan image
`beyond2lte`.

Flash = sama seperti S10+ (BUILD.md §6):

```
Download Mode:  (mati) Bixby + Volume Down + USB
Recovery:       (mati) Bixby + Volume Up   + USB
Odin: AP = uefi-boot.tar,  Re-Partition TIDAK dicentang
```

**Peringatan PIT:** `BEYOND2LTE_EUR_OPEN.pit` di repo ini **milik S10+**.
Nama-nama partisi identik, tetapi offset/ukuran berbeda. Jangan pakai
Re-Partition dengan PIT itu. `BOOT` S10e harus cukup besar (S10+ = 55 MiB);
UEFI kita jauh di bawahnya.

---

## 5. DTB: sekarang DTB asli S10e (sudah terpasang)

`build_uefi.py` **mengharuskan** `Resources/DTBs/beyond0lte.dtb` ada
(`append_dtb = true`) dan akan menghentikan build kalau tidak ada.

Yang duduk di repo sekarang **bukan lagi placeholder**, melainkan DTB asli S10e
yang diekstrak dari firmware stok `G970FXXUHHXJ1` (CSC XID, Android 12):

- `base` = entri di `dt.img` (Samsung menyimpannya sebagai tabel DTBO 1 entri).
- `overlay` = entri `dtbo.img` yang cocok dengan `beyond0lte`.
- Digabung dengan `fdtoverlay`; `model` dari overlay **tidak ditimpa**.

Hasilnya: `model = "Samsung BEYOND0LTE EUR OPEN 25 board based on EXYNOS9820"`,
3 bank memori (total 6.00 GiB), `reserved-memory`, `sec_rdx_bootdev`, `seclog`,
`modem_spare` semua ada.

`dtbo.img` S10e memuat **7 entri**, semuanya `BEYOND0LTE` dan hanya berbeda
revisi board (`EUR OPEN 17..25`). Skrip memilih **revisi tertinggi** sebagai
default; kalau ingin entri lain, pakai `--index`.

Cara menghasilkan/memperbarui DTB asli (tidak perlu `mkdtimg`):

```bash
# 1) cara termudah, dari firmware stok (otomatis base + overlay):
bash tools_dtb_s10e.sh --tar AP_G970F....tar.md5

# 2) atau dari perangkat yang sudah boot:
adb shell su -c 'cat /sys/firmware/fdt' > s10e-fdt.dtb
bash tools_dtb_s10e.sh --fdt s10e-fdt.dtb

# 3) menelusuri manual bila perlu:
python3 tools_dtbo_extract.py --dtbo dtbo.img --list      # lihat 7 entri
python3 tools_dtbo_extract.py --dtbo dtbo.img --index 6 --out overlay.dtb
bash tools_dtb_s10e.sh --base base.dtb --overlay overlay.dtb
```

Setelah DTB berubah, **jalankan ulang audit**:

```bash
python3 tools_reserved_memory_audit.py --device beyond0lte --check
```

Setiap region yang muncul `ACIK` harus ditambahkan ke `MemoryMapLib.c` sebagai
`Reserv`. Audit DTB asli S10e **lulus**: semua 23 region `reg` tetap sudah
terlindungi, **tidak ada region baru** yang perlu ditambahkan. Perbedaan
dibanding DTB S10+ hanya ukuran region `camera` (S10e 228 MiB vs S10+ 450 MiB),
dan keduanya sudah tertutup "Memori dinamis bawah" yang ada.

---

## 6. Yang paling rapuh pada port ini

Urut dari yang paling mungkin membuat flash pertama gagal:

1. **Region `"Display Reserved"` S10e** (§2.2). Masih **asumsi** (diambil dari
   S10+/`beyond1lte`: `0xCA000000`, 20 MiB) karena region ini disetel oleh
   S-Boot, bukan oleh DTB. Lihat gejala di atas.
2. **PIT/UH S10e.** Region `Samsung U-H EL2` (`0x87000000`, 2 MiB) dipertahankan
   dari S10+; verifikasi ukuran `UH` di PIT S10e.
3. **Revisi board fisik.** DTB memakai `EUR OPEN 25` (revisi tertinggi yang
   tersedia). Kalau perangkat Anda rev. lebih lama, ganti dengan `--index N`;
   perbedaannya kecil, tetapi ini yang paling benar.
4. ~~Perangkat belum diuji.~~ **Sudah diuji boot di S10e**: firmware dijalankan
   S-Boot dan mencapai BDS; layar self-test `PORT_S10E.md` §6.2 muncul dan
   **semua nilai sesuai**: `1080x2280` (GOP mode 0), `6144 MB DRAM / 2080 MB
   free`, `57 desc, max end 0x980000000`, `RAM >=0x980000000 yok (dogru)`,
   UFS `122016 MB blk=4096`, `FAT/FS 0 volume`, `AbsolutePointer var`, uji
   tombol live jalan (Vol Down → `SCAN_DOWN`). Menu **Boot Manager** bawaan
   bisa dibuka dengan menahan **Volume Up** saat boot. Firmware lolos
   validasi PE (`Mu-beyond0lte.img` 4,4 MB, FD tepat 4 MiB di `0xC9000000`,
   67 image). Sisa catatan khusus unit uji: tombol power fisiknya mati
   (§6.1).

Yang **sudah terverifikasi dari DTB asli** (bukan lagi dugaan):

- **GPIO tombol** cocok persis dengan `KeypadDeviceLib`: power `gpa2-4`
  (Bank 2/A pin 4), vol up `gpa0-3`, vol down `gpa0-4`, tombol khusus
  (Bixby) `gpa0-6` — semuanya `BankId = A`, sama seperti S10+.
- **Panel** `s6e3fa7_beyond0` melaporkan `mres_width = 0x438` (1080) dan
  `mres_height = 0x8e8` (2280), sesuai PCD framebuffer.
- **Memori** 6 GiB dalam 3 bank; **tidak ada** baris `0x980000000`.

Yang **tidak** diharapkan bermasalah: UFS, ACPI, PSCI/SMP, EFI Shell, dan
mekanisme boot secara keseluruhan — semuanya level SoC.

### 6.1 Tombol power mati → digantikan Bixby

Unit S10e uji ini tombol power fisiknya **mati**, sehingga `Enter`,
`Left`, dan `Right` (yang semuanya berasal dari tombol power) hilang. Di
`beyond0ltePkg/Library/KeypadDeviceLib/KeypadDeviceLib.c`, tombol Bixby
(`gpa0-6`) kini dipetakan dengan `UnicodeChar = CHAR_CARRIAGE_RETURN`
(bukan lagi `SCAN_ESC`). Logika long-press/combo yang sudah ada di
`LibKeyUpdateKeyStatus` otomatis memberi:

| Aksi | Hasil |
| --- | --- |
| Bixby, tekan singkat | `Enter` |
| Bixby, tahan ≥ 0,5 dtk | `Esc` |
| Bixby tahan + Volume Up | `Left` |
| Bixby tahan + Volume Down | `Right` |
| Volume Up / Volume Down | `Up` / `Down` (tidak berubah) |

Kombinasi `Left`/`Right` hanya terpicu bila tombol volume sudah ditahan
sebelum Bixby melewati 0,5 dtk (perilaku sama seperti tombol power asli).

Perubahan ini **hanya** di `beyond0ltePkg`; salinan di `beyond2ltePkg`
tidak disentuh, jadi perilaku S10+ tetap seperti semula.

### 6.2 Verifikasi tanpa shell: layar self-test "no OS"

Kenyataan di perangkat: **tidak ada** partisi ESP/FAT, dan keypad hanya
menghasilkan `Up`/`Down`/`Enter`/`Esc` — **tidak ada huruf**. Jadi EFI Shell
tidak bisa dipakai mengetik `memmap` atau perintah lain. BDS pun berhenti
dengan "No Operating System Found!" lalu mematikan perangkat.

Karena itu `beyond0ltePkg` meng-override `DeviceBootManagerLib` (default
`SiliciumNullPkg`, isinya kosong) dengan
`Library/PlatformBootManagerLib/B2LPlatformBootManager.c`. Fungsi
`DeviceBootManagerUnableToBoot()` — dipanggil BDS tepat sebelum mencetak
logo/no-OS — kini menggambar **layar self-test** dan menahannya sampai
pengguna keluar. Verifikasi cukup dengan memotret layar; tidak butuh alat,
ESP, maupun mengetik.

Yang ditampilkan (dan nilai yang benar):

| Baris | Arti | Nilai benar |
| --- | --- | --- |
| `Display` | mode GOP aktif | 1080×2280 |
| `RAM` | total DRAM terdeskripsi + memori bebas | ≈ `6144 MB` (6 GiB); free ≠ 0 |
| `Memmap` | jumlah descriptor + alamat akhir tertinggi | `max end 0x980000000` |
| `RAM >=0x980000000` | sanity DRAM S10e | `none (ok)` |
| `Block IO` | perangkat block terpasang | UFS ≈ 122 GB |
| `FAT/FS` | volume FAT terpasang | `0 volume` (memang belum ada ESP) |
| `Touch` | `EFI_ABSOLUTE_POINTER_PROTOCOL` | `AbsolutePointer present` |
| `Key` / `Touch` (bawah) | uji tombol/sentuh live | berubah saat ditekan |

Kalau `RAM` menunjukkan **8192 MB**, itu tanda bank S10+ (`0x980000000..0xA00000000`)
masih ikut terpetakan — berarti `MemoryMapLib` salah memakai salinan S10+.
`6144 MB` adalah angka yang benar untuk 6/128.

Keluar/matikan: **tahan Bixby, lalu tekan Volume Up/Down** (menghasilkan
`Left`/`Right`). Setelah keluar, firmware melanjutkan jalur "no OS" dan
mematikan perangkat (BDS menampilkan "No Operating System Found!", jeda
~10 dtk, lalu mati).

Override ini hanya terpasang di `beyond0lte.dsc`. Salinan
`beyond2ltePkg` tidak disentuh, jadi jalur "no OS" S10+ tetap seperti semula.

#### Hasil uji

Di-flash **2026-11-11** dengan `uefi-boot.tar` SHA-256
`39c74756e2019de1cd1b6cb888816287ce906fe884a3b4606e74acb5a96e1a60`.

| Baris | Hasil di perangkat | Status |
| --- | --- | --- |
| `Display` | `1080x2280 (GOP mode 0)` | OK |
| `RAM` | `6144 MB DRAM, 2080 MB free` | OK — 6 GiB pas, bukan 8 GiB |
| `Memmap` | `57 desc, max end 0x980000000` | OK — tidak ada di atas batas |
| `RAM >=0x980000000` | `yok (dogru)` | OK |
| `Block IO` | `37 device`; `#1 122016 MB blk=4096 fixed` | OK — UFS 122 GB, blok 4 KiB |
| `FAT/FS` | `0 volume` | sesuai — memang belum ada ESP |
| `Touch` | `AbsolutePointer present` (dari `ConSplitter`) | **belum** — perlu lihat `Touch.entry`; lihat di bawah |
| `Key` (live) | Volume Down → `SCAN_DOWN (0x02)` | OK |

Kesimpulan: memory map, UFS, dan GOP semuanya sesuai perkiraan; **tidak
ada** bank DRAM hantu di atas `0x980000000` dan total DRAM tepat 6 GiB.
Uji tombol live juga jalan: Volume Down → `SCAN_DOWN (0x02)`, Bixby singkat
→ `ENTER (0x0D)`, Bixby tahan → `SCAN_ESC (0x17)`, dan Bixby-tahan + Volume
berhasil keluar (lanjut ke "No Operating System Found!" lalu mati). Sentuh
masih dalam penyelidikan — lihat bagian di bawah.

#### Uji sentuh: masalah dan perbaikannya

Uji sentuh pertama **tidak bereaksi** (baris `Touch` tetap `(henuz yok)`).
Akar masalahnya bukan panel: `AbsolutePointer var` sudah muncul, artinya
driver berhasil mem-probe `sec_ts` (`DeviceId` + firmware integrity + boot
status `APP_MODE 0x20`). Penyebab sebenarnya ada di alur event:

- Loop self-test menunggu dengan `gBS->Stall`, yang di DXE core hanya
  metronome busy-wait — **tidak** memompa event loop. Akibatnya timer
  `EVT_TIMER | EVT_NOTIFY_SIGNAL` (TPL_CALLBACK) milik driver sentuh tidak
  pernah di-dispatch, sehingga `PollEvents` tak pernah jalan dan kuyur
  state selalu kosong.
- Sebaliknya keypad tetap jalan karena `KeypadTextIn` mem-poll tombol
  langsung di dalam `ReadKeyStroke`, bukan lewat timer.

Perbaikannya dua sisi, keduanya hanya di `beyond0ltePkg`:

1. **Pompa event loop di UI** (`B2LPlatformBootManager.c`): setiap iterasi
   memanggil `gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL))`.
   `CoreRestoreTpl` men-dispatch semua notify dengan TPL di atas TPL
   aplikasi, jadi timer `TPL_CALLBACK` ikut terpicu.
2. **Polling on-demand di driver** (`B2LTouchDxe.c`): kalau `GetState`
   melihat kuyu kosong dan sudah lewat ≥ 20 ms sejak poll terakhir, driver
   sendiri membaca denetil secara langsung. Jadi UI tetap dapat data tanpa
   bergantung pada timer. `TouchNowNs` memakai `TimerLib`.

Sekaligus baris diagnostik baru `Touch.dx` ditambahkan supaya satu kali
flash bisa memastikan di mana masalahnya: `st` = hasil `GetState`
(`Not Ready` = FIFO kosong/memang belum disentuh, `Device Error` = I2C
gagal), `poll` = berapa kali `GetState` dipanggil, `hit` = berapa kali ada
state. Kalau `poll` naik dan `hit` naik saat layar disentuh, jalur sentuh
sudah hidup.

Muncul di build `59e8177`; bundle `uefi-boot.tar` SHA-256
`3280baacfe50988e5f08b49517c11515f4d4364d3e5e52e168a160c1f2581e17`.

Setelah build itu di-flash, `poll` memang naik tetapi `hit` tetap 0 dan
`st=Not Ready`. Artinya event loop sudah beres, tetapi `sec_ts` sendiri
belum menghasilkan event: kontroler belum masuk mode scan.

Perbaikan kedua (build `6b683c1`) menambahkan *bring-up* ala
`sec_ts_reinit`/`start_device` Linux:

1. tulis *touch function* `0x30` (16-bit LE: `TOUCH|PALM|WET` = `0x61`),
   lalu baca kembali nilai sebelum/sesudahnya;
2. baca *power mode* `0xE4`; kalau masih LPM (`0x01`), tulis `0x00` (scan);
3. `CLEAR_EVENT_STACK` (`0x62`), lalu `SENSE_ON` (`0x10`).

Sebagai alat diagnosis, driver menerbitkan protokol lokal (GUID tetap,
tanpa mengubah INF modul BDS) dan layar self-test mencetak:

| Baris | Isi |
| --- | --- |
| `Touch.fn` | `be=xxxx af=xxxx pm=xx ok=n` — nilai `0x30` sebelum/sesudah, `0xE4`, dan apakah bring-up sukses |
| `Touch.ev` | `poll empty err` lalu `st co ig` — jumlah baca, FIFO kosong, error I2C, event status, event koordinat, event yang diabaikan |
| `Touch.raw` | 8 byte terakhir yang dibaca dari FIFO |

Interpretasi: `co` naik dan `hit` naik saat layar disentuh = jalur sentuh
hidup; `poll` naik tetapi `empty` ikut naik dan `co=0` = kontroler masih
belum mengirim event; `err` naik = masalah I2C.

Setelah bring-up `6b683c1` di-flash, hasilnya masih negatif dan justru
membongkar kesalahan diagnosis sebelumnya:

- `Touch.m : 65536 x 65536` — nilai ini **bukan** dari driver kita (driver
  menyetel `4095`). Itu mode default `ConSplitterDxe` (`0x10000`), jadi
  `AbsolutePointer` yang dipegang UI adalah protokol **agregat**, bukan
  milik `B2LTouchDxe`.
- `Touch.fn: no diag protocol` — protokol teshis kita tidak terpasang,
  artinya `B2LTouchEntry` gagal **sebelum** baris instalasi di akhir.

Jadi klaim lama "driver berhasil mem-probe `sec_ts`" keliru: yang terdeteksi
hanya `ConSplitter`. Urutan dispatch juga menjelaskan sebabnya —
`B2LTouchDxe` ada di `APRIORI` sebelum `PmicDxe`, sehingga `entry`-nya
berjalan sangat awal.

Build `c97d78f` memperbaiki instrumentasi supaya satu kali flash menunjukkan
titik gagalnya:

1. Protokol teshis sekarang dipasang **pertama kali** di `B2LTouchEntry`,
   bukan di akhir.
2. `B2L_TOUCH_DIAG` diperluas dengan `Stage` (1=i2c, 2=events,
   3=install, 4=probe, 5=reset, 6=timer, 7=selesai), `EntryStatus`,
   `AbsPointer`, `DeviceId[5]`, `Integrity`, dan `BootStatus`.
3. Protokol `AbsolutePointer` dipasang **sebelum** probe, sehingga kalau
   probe/bring-up awal gagal, UI masih bisa memanggil `Reset(TRUE)` untuk
   mencoba lagi setelah `PmicDxe` dkk. termuat. Karena DXE core membuang
   imaji driver yang `entry`-nya mengembalikan error, `B2LTouchEntry`
   sekarang selalu mengembalikan `EFI_SUCCESS` setelah protokol diag
   terpasang; status sebenarnya ada di `Touch.entry`.
4. UI mencari protokol teshis lebih dulu; kalau `AbsPointer` terisi, UI
   memakai protokol itu **langsung** (melewati `ConSplitter`).
5. Baris baru: `Touch.entry` (stage + status) dan `Touch.id`
   (DeviceId + integrity + boot status).

Interpretasi hasil berikutnya:

| Baris | Arti |
| --- | --- |
| `Touch.entry: stage=1` | gagal di `B2LHsI2cInitialize` (LocateProtocol/MapMemoryRegion/InitBus) |
| `Touch.entry: stage=2` | gagal membuat event konteks |
| `Touch.entry: stage=3` | gagal memasang `AbsolutePointer` |
| `Touch.entry: stage=4` | I2C jalan tetapi probe `sec_ts` gagal (lihat `st` dan `Touch.id`) |
| `Touch.entry: stage=5` | I2C+probe OK, bring-up `0x30/0xE4/0x62/0x10` gagal |
| `Touch.entry: stage=7` | driver selesai; fokus ke `Touch.ev`/`Touch.dx` |
| `Touch.id: 00 00 ...` | Device ID tidak terbaca / bus tidak menjawab |
| `Touch.id: xx ... i=80 b=20` | chip menjawab; firmware OK dan mode APP |

Bundle build `c97d78f`: `uefi-boot.tar` SHA-256
`7387668e1081813b4cd4355918fb1afaf6972369c59438ebb68fe3a1040722fe`,
`Mu-beyond0lte.img` SHA-256
`1ee095232376bad8ec53aefb55ee984f04ccd27cde82d11bd62d871b53014240`.
Teks layar self-test berbahasa **Inggris** (komentar kode tetap Turki) dan
dicetak **3x lebih besar**: glif diambil dari HII font lalu tiap piksel
digambar sebagai blok 3x3 ke framebuffer (cara yang sama dipakai
`B2LGoster`), supaya mudah difoto.
**Status: menunggu hasil flash `c97d78f`** (hasil akan dicatat di sini).

#### Perbaikan transport I2C + daya rail (build ini)

Hasil `c97d78f` di perangkat: `Touch.entry: stage=4 st=Warning Reset Require`,
`Touch.id` semua nol, `Touch.fn be=0000`, `Touch.dx st=Device Error poll=8
hit=0`, `Touch.ev poll=8 empty=0 err=8`, dan log `B2LTouch: HSI2C10 transfer
failed: Time out`. Artinya I2C sudah jalan sampai `sec_ts`, tetapi chip
**tidak pernah menjawab** (NACK/timeout), bukan masalah event loop.

Akar masalahnya urutan APRIORI: `B2LTouchDxe` berjalan **sebelum**
`PmicDxe`, padahal rail daya sentuh dipegang S2MPS19 lewat `PmicDxe`.
Selama `tsp_io`/`tsp_avdd` mati, panel tidak memberi ACK sehingga setiap
transfer time-out.

Perubahan pada build ini (tetap hanya `beyond0ltePkg`):

1. **APRIORI**: `PmicDxe` dipindah **sebelum** `B2LTouchDxe`
   (`Include/APRIORI.inc`). Urutan di dalam FV terverifikasi: PMIC (indeks
   14) mendahului Touch (indeks 15).
2. **Transport memakai protokol resmi**: `B2LHsI2c.c` tidak lagi menulis
   register HSI2C secara manual, tetapi memakai
   `EFI_HSI2C_PROTOCOL` (`ReadBuffer`/`WriteBuffer`). Jalur ini sudah
   teruji dan membedakan NACK (`EFI_NO_RESPONSE`) dari timeout
   (`EFI_TIMEOUT`), sesuatu yang hilang pada implementasi manual lama.
3. **Driver menyalakan rail sendiri**: `B2LTouchPowerOn()` (di `TouchReset`
   dan sebelum probe pada `entry`) memakai
   `EFI_PMIC_REGULATOR_PROTOCOL`: `tsp_io` = S2MPS19 LDO19 @1.8 V,
   `tsp_avdd` = S2MPS19 LDO27 @3.0 V, mode `NORMAL`, lalu baca balik
   tegangannya. Jadi rail benar-benar menyala walau bootloader melewatinya.
4. **Diagnostik diperluas**: `B2L_TOUCH_DIAG` kini 128 byte dengan
   `Signature`/`Size` (dua modul harus sama; kalau tidak, layar mencetak
   `Touch.diag: no protocol/layout`). Ditambah: `PmicLocate`, `LdoUv[2]`,
   `LdoSetOk[2]`, `PowerOk`, `LastStatus`, snapshot `IntStatus`/
   `TransStatus` HSI2C, dan peta ACK `ScanAck` (scan alamat `0x08..0x77`).
5. **Layar self-test** mencetak baris diagnostik dinamis baru.

Baris baru dan artinya:

| Baris | Arti |
| --- | --- |
| `Touch.ent: stage` | tahap terakhir yang dicoba (7 = seluruh entry selesai) |
| `Touch.ent: st` | `EFI_STATUS` tahap tersebut |
| `Touch.id` | Device ID `sec_ts` (`00 00 ...` = bus tetap tidak menjawab) |
| `Touch.id: iv/bs/rc/pc` | firmware integrity, boot status, jumlah Reset/Probe |
| `Touch.fn` | isi register `0x30` sebelum/sesudah bring-up + sukses |
| `Touch.pm: loc` | 0 = belum dicoba, 1 = protokol regulator ketemu, lain = kode error |
| `Touch.pm: set/pwr` | hasil `SetLdo` untuk LDO19/LDO27 dan status daya gabungan |
| `Touch.pm: io/av` | tegangan terbaca LDO19/LDO27 (µV) |
| `Touch.err` | `EFI_STATUS` transfer gagal terakhir (`Success` = belum ada yang gagal) |
| `Touch.di: int/tr` | register `INT_STATUS`/`TRANS_STATUS` HSI2C10 saat gagal |
| `Touch.sc` | bitmask alamat `0x08..0x77` yang memberi ACK (bit j = alamat j) |
| `Touch.ev: p/e/x` | jumlah poll, FIFO kosong, error I2C |
| `Touch.ev: s/c/i` | event status, event koordinat, event diabaikan |
| `Touch.raw` | 8 byte terakhir dari FIFO |

Cara membaca cepat:

- `Touch.diag: no protocol/layout` → dua definisi struct tidak sinkron
  (bug build), bukan masalah perangkat keras.
- `Touch.pm: pwr=0` → rail tidak menyala; lihat `loc`/`set`/`io`/`av`.
- `Touch.pm: pwr=1` tetapi `Touch.err` masih `Timeout`/`No Response` dan
  `Touch.sc` tidak menyalakan bit `48` → bus benar tetapi panel tetap diam
  (kemungkinan perlu reset GPIO / urutan power yang berbeda).
- `Touch.sc` menyalakan bit selain `48` → alamat/registrasi kontroler salah.
- `Touch.err: Success` + `Touch.ev: c` naik saat disentuh → jalur sentuh hidup.

Bundle build ini: `uefi-boot.tar` SHA-256
`8be69d0aa7d710428103a2019ea4cd2eeeb1682c47f6886d9116392958c5d609`,
`Mu-beyond0lte.img` SHA-256
`27fe60a9fd6eb1371fb1b13b8d4e4fcb5c67492be344c303d8b51aae8a027d26`
(zip `3a9be4733282dc7ede0cf51f00fdd39ee962e2386415bba98b6f9239d7f71fb9`).

##### Hasil di perangkat — sentuh hidup

Bundle di atas di-flash; hasilnya:

| Baris | Hasil | Arti |
| --- | --- | --- |
| `Touch.ent` | `stage=7 st=Success` | seluruh entry selesai |
| `Touch.id` | `53 45 37 61 00` | Device ID `"SE7a"` — chip menjawab |
| `Touch.id` | `iv=80 bs=20 rc=2 pc=2` | firmware OK, mode `APP`; reset/probe 2x |
| `Touch.fn` | `be=6100 af=6100 ok=1` | touch function `0x61` benar |
| `Touch.pm` | `loc=1 set=11 pwr=1` | protokol regulator ketemu, dua LDO di-set |
| `Touch.pm` | `io=1800000 av=3000000 uV` | `tsp_io` 1,8 V, `tsp_avdd` 3,0 V — tepat |
| `Touch.err` | `Success` | tidak ada transfer gagal |
| `Touch.di` | `int=00000000 tr=00000000` | tidak ada NACK/timeout |
| `Touch.sc` | semua nol | tarama tidak perlu (tidak ada kegagalan) |
| `Touch.ev` | `p=6232 e=0 x=0` | ribuan poll, nol error I2C |
| `Touch.ev` | `s=815 c=6878 i=5980` | 6878 event koordinat diproses |
| `Touch.raw` | `C4 C8 89 0C 08 08 20 00` | event nyata; berubah mengikuti sentuhan |
| `Touch.m` | `4095 x 4095` | mode milik driver kita (bukan ConSplitter) |

Contoh event `C4 C8 89 0C 08 08 20 00` = `RELEASE`, `touchId=1`,
`X=0xC80=3200`, `Y=0x89C=2204` — nilai masuk akal untuk panel 4095.

Kesimpulan: **I2C/panel/touch benar-benar hidup.** Akar masalah sebelumnya
(rail daya belum menyala karena urutan APRIORI) sudah teratasi. `ev/raw`
berubah mengikuti sentuhan di layar, sesuai yang diharapkan.

##### Perbaikan tata letak layar

Dengan 39 baris (61 px/baris) layar 1080×2280 meluber, sehingga baris
`Touch` (koordinat hidup) dan `Touch.dx` (hitungan `hit`) terpotong di bawah
`Key`. Perbaikan tata letak (isi tetap sama):

1. `Block IO` menampilkan maksimal 2 perangkat (bukan 6).
2. `Touch.ent` digabung jadi satu baris (`stage` + `st`).
3. `Touch.di`/`Touch.sc` hanya dicetak bila memang ada kegagalan
   (`LastStatus != 0` atau `ScanDone`), jadi tidak menyita ruang saat normal.

Sekarang total ~31 baris; `Touch` dan `Touch.dx` terlihat penuh.

Bundle tata letak baru: `uefi-boot.tar` SHA-256
`9c9f800056b9ce00280a3615ecea7bffd730f87751f218271909484557621662`,
`Mu-beyond0lte.img` SHA-256
`1465739896e681c9ee61b409fc2a84194c6edfa45d25892c637b177c5c90c3b2`
(zip `11a26d6253a8bdea5d4efb5373f1d897335ff5f787aee9b85bace711a199a07e`).

##### Verifikasi penuh: koordinat sampai ke `GetState`

Setelah tata letak dirapikan, seluruh baris terlihat dan jalur koordinat
terbukti utuh:

| Baris | Hasil | Arti |
| --- | --- | --- |
| `Touch` | `X=3844 Y=2256 Z=32 btn=0x1` | koordinat + tombol aktif sampai ke `GetState` |
| `Touch.dx` | `st=Success poll=4730 hit=2368` | separuh poll mengembalikan state |
| `Touch.raw` | `84 F0 8D 40 08 08 20 00` | `MOVE`, `X=0xF04=3844`, `Y=0x8D0=2256` — cocok |

Jadi sentuh bukan cuma "event terbaca", tetapi koordinat benar-benar
diteruskan sebagai `EFI_ABSOLUTE_POINTER_STATE`.

Satu cacat logika kecil ikut ketahuan: saat **tidak** disentuh,
`Touch.dx st` sempat menampilkan `Device Error`. Penyebabnya `EFI_ERROR`
juga menganggap `EFI_NOT_READY` sebagai error, sehingga kondisi "FIFO
kosong / poll ditunda" salah dilabeli. Perbaikan di `TouchGetState`:
hanya kode error selain `EFI_NOT_READY` yang dipetakan ke `EFI_DEVICE_ERROR`;
sisanya `EFI_NOT_READY` (nilai benar saat diam). Setelah flash, saat tidak
disentuh `Touch.dx st` harus `Not Ready`, bukan `Device Error`.

Bundle final: `uefi-boot.tar` SHA-256
`4f73b397e24801fa1d20c02aaccd3171da34e42b698c32b194d236169e6dca58`,
`Mu-beyond0lte.img` SHA-256
`bc1303dfb1c7fe3695b9aa5219b528da2f6032bdc5d4c2b2efaee88f795db38c`
(zip `f981ffc85a1343a62acd42b72a0c096e0db1498ef699ce8cbae9807792249060`).

Konfirmasi setelah flash bundle final:

| Baris | Hasil | Arti |
| --- | --- | --- |
| `Touch.dx` | `st=Not Ready poll=1464 hit=719` | saat diam kini `Not Ready` (perbaikan benar) |
| `Touch` | `X=3184 Y=3648 Z=0 btn=0x0` | state `RELEASE` (tombol lepas) |
| `Touch.raw` | `C4 C7 E4 00 08 08 20 00` | `RELEASE`, X=0xC70=3184, Y=0xE40=3648 — cocok |

**Status: sentuh terverifikasi penuh** — koordinat, tombol, dan pelabelan
status semuanya benar; `beyond2ltePkg` tidak disentuh.

### 6.3 Probe USB (Fase 0): mengukur dulu, belum mengubah

Sentuh selesai. Arah berikutnya yang dipilih adalah **dukungan USB** — masih
❌ di `README.md` dan di `DEVIR.md` §4.6. Sebelum menulis driver apa pun,
langkah paling murah dan paling menentukan adalah **mengukur** apakah blok
USB (DWC3 + PHY) di perangkat ini masih hidup setelah S-Boot menyerahkan
kendali — S-Boot memang baru saja memakai USB untuk Download Mode.

Layar self-test kini menambah **4 baris baca-saja** (`USB.*`). Tidak ada satu
register pun yang ditulis; hanya `MmioRead32`.

| Baris | Isi | Alamat |
| --- | --- | --- |
| `USB.pmu` | PHY ctrl USBDRD / USBDP | `0x15860000+0x72C`, `+0x704` |
| `USB.con` | `CTRL_VER`, `CLKRST` USBCON | `0x10B00000+0x00`, `+0x20` |
| `USB.dw3` | `GSNPSID`, `GCTL` DWC3 | `0x10C00000+0xC120`, `+0xC110` |
| `USB.xhc` | `CAPLENGTH/HCIVERSION`, `HCSPARAMS1` xHCI | `0x10C00000+0x00`, `+0x04` |

Alamat diambil dari DTB stok S10e di dalam repo
(`Resources/DTBs/beyond0lte.dts`: `usb@10C00000`, `phy@10B00000`,
`pmu_offset=0x72c`, `pmu_offset_dp=0x704`) dan konsisten dengan
`S5E7885Pkg`/`S5E9830Pkg` `PlatformUsbfnLib` yang memakai peta register
USBCON yang sama. Offset DWC3 (`GSNPSID`/`GCTL`/`GUSB2PHYCFG`) mengikuti
`SamsungPkg/Include/Device/Dwc3.h`.

Cara membacanya:

- `USB.dw3` `GSNPSID` berawalan `5533` (hijau) → kontroler terjangkau dan
  ter-clock. Ini indikator tunggal terkuat.
- `USB.xhc` `CAPLENGTH != 0` (hijau) → blok register xHCI hidup, artinya jalur
  **host** (keyboard/flashdisk) realistis tanpa kerja clock tambahan.
- Bila keduanya `0` atau `ffffffff` → blok belum ter-clock/ter-power; langkah
  berikutnya adalah menyalakan PHY + gate CMU dulu, baru menyentuh DWC3/xHCI.

Untuk menghemat ruang layar, dua baris petunjuk tombol digabung menjadi satu
(`VolUp/Down=Up/Down Bixby=Enter Bixby+Vol=Exit`); perilaku tombol tidak
berubah, hanya labelnya.

Bundle probe (build ini):

- `uefi-boot.tar` SHA-256
  `1fbbdf6f98a299b30e5f2c650dd25944324889c35b282c5f16799139a92c7512`
- `Mu-beyond0lte.img` SHA-256
  `ab242841028d94122124d13bd8525eba922103177e74f0b6366edb4ab72aad9d`
- zip SHA-256
  `332d2c3d4fcb0d3d5715fc2f30485f001717463b14a0c4eef830832e63b8bc5e`

#### Hasil di perangkat

_(belum diisi — menunggu satu flash di S10e; foto baris `USB.*`)_

---

## 7. Rollback

Sama seperti BUILD.md §8. UEFI ditulis ke partisi **BOOT**, bukan ke `sboot`,
jadi Download Mode selalu bisa dicapai:

1. Bixby + Volume Down + USB → Download Mode
2. `AP` = `AP_G970F..._*.tar.md5` (firmware stok S10e), atau sideload LineageOS
   dari recovery.

`RP SWREV` S10e (G970F) harus dicek sebelum mem-flash apa pun yang lebih tua;
aturan anti-rollback sama ketatnya dengan S10+.
