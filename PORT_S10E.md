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
mu-silicium-overlay/Silicon/Samsung/S5E9820Pkg/Library/ResetSystemLib/  ← fix reset SoC (§6.9)
mu-silicium-overlay/Resources/Configs/beyond0lte.toml
mu-silicium-overlay/Resources/DTBs/beyond0lte.dts/.dtb  ← DTB asli S10e, lihat §5
tools_dtb_s10e.sh                                        ← pembuat DTB asli
tools_dtbo_extract.py                                    ← pengurai dtbo.img/dt.img
tools_serial_monitor.py                                  ← monitor serial stdlib (§6.7)
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

#### v1: device hang (Data abort) dan perbaikannya

Flash v1 di S10e berhenti di percobaan baca pertama:

```
Synchronous Exception at ...
ESR 0x96000006  EC 0x25  ISS 0x6   (Data abort)
                 Translation fault, second level
FAR  0x0000000010B00000
X19  0x0000000010B00000
ASSERT [ArmCpuDxe] CpuExceptionHandlerLib.c(438)
```

Artinya: pembacaan PMU (`0x15860000`) berhasil (region itu ada di memory map
S-Boot), lalu pembacaan USBCON (`0x10B00000`) meleset — **region USB tidak
dipetakan di page table UEFI**. Sebelumnya diasumsikan S-Boot meninggalkan
seluruh blok USB terpetakan; ternyata tidak.

v2 memakai pola yang sama seperti `HsI2cDxe`/`GpioDxe`/`SpeedyDxe`: setiap
region USB dipetakan dulu sebagai `EfiMemoryMappedIO` lewat `MapMemoryRegion`
(`SiliciumPkg/Library/MemoryAllocationHelperLib`), lalu **hanya region yang
berhasil dipetakan yang dibaca**. Kalau pemetaan gagal, region itu tidak pernah
disentuh, sehingga Data abort tidak mungkin lagi terjadi. Bila ada yang gagal,
layar menampilkan satu baris merah tambahan, `USB.map : con=%r dw3=%r`;
pada keadaan normal hanya 4 baris `USB.*` seperti sebelumnya.

Bundle probe (build v2):

- `uefi-boot.tar` SHA-256
  `71590e634af8a0ce89ab2125fb6d6339a1941c378d2f4d15fecda7419bdfab50`
- `Mu-beyond0lte.img` SHA-256
  `0cd2b3acd11b33902019e1fcbbc7134271db7255de6d0d2b9dc197c3a9ffcc0d`
- zip SHA-256
  `795c4d496c8bcb2b329c78cd81e8b61b7a979374fe52ecc105eb420b6b3aa692`

#### Hasil di perangkat

Flash v2 di S10e: **tidak hang lagi**, layar self-test tampil penuh, dan
kedua region USB berhasil dipetakan — **tidak ada** baris merah `USB.map`.

```
USB.pmu : 00000000 00000000
USB.con : 00030000 00000000
USB.dw3 : 33313130 30C12004
USB.xhc : 01100020 02000240
```

Decode:

- `USB.dw3` `GSNPSID` = `0x33313130`. 16 bit atas = `0x3331`, **persis**
  `DWC3_GSNPSID_USB31` (`Dwc3.h`); `Dwc3CoreInit()` juga menerima nilai ini
  (bukan hanya `0x5533`). Jadi kontrolernya bukan DWC3 USB3 biasa, melainkan
  **DWC3 USB 3.1**. Blok register DWC3 hidup dan terjangkau.
- `USB.dw3` `GCTL` = `0x30C12004` → `PRTCAPDIR` (bit 12..13) = `0b10` = **2 =
  DEVICE**. S-Boot meninggalkan core dalam **mode device** (konsisten dengan
  Download Mode).
- `USB.xhc` = `0x01100020`, `0x02000240`:
  - `CAPLENGTH` = `0x20` (32 byte), `HCIVERSION` = `0x0110` → **xHCI 1.1**.
  - `HCSPARAMS1`: `MaxSlots` = 64, `MaxIntrs` = 2, `NumPorts` = **2** (satu
    USB2 + satu USB3) — konsisten dengan satu port USB-C S10e.
- `USB.con` `CTRL_VER` = `0x00030000` (blok PHY `0x10B00000` terjangkau, bukan
  `0`/`0xffffffff`); `CLKRST` = `0`.
- `USB.pmu` = `0`/`0` → bit enable PHY (`0x15860000+0x72C`) **belum aktif**;
  PHY belum dinyalakan (probe memang tidak menulis apa pun).

Kesimpulan: masalahnya **bukan lagi mapping**. Blok DWC3 + xHCI + register PHY
sudah hidup dan dapat diakses setelah S-Boot menyerahkan kendali. Yang belum
ada hanyalah bring-up PHY/clock eksplisit dan pemilihan mode. `PRTCAPDIR=DEVICE`
menunjukkan jalur **device** paling sedikit hambatannya.

Catatan kosmetik: cek "hijau" di `USB.dw3` v2 hanya menguji `0x5533`, sehingga
`0x3331` tampil abu. Akan diperlonggar pada build berikutnya (perilaku driver
tidak berubah; `Dwc3CoreInit()` sudah menerima keduanya).

### 6.4 M1: bring-up pengendali device (DWC3) + probe `USB.dev`

Setelah Fase 0 membuktikan blok USB hidup, jalur yang dipilih adalah **device**
(bukan host/xHCI). Alasannya: `PRTCAPDIR` sudah `DEVICE`, arsitektur repo
(`UsbfnDwc3Dxe` + `PlatformUsbfnLib`) langsung mendukung, dan tidak perlu
mengeluarkan VBUS 5 V ke port (PC yang menyuplai). Kelas gadget target (M2)
adalah **CDC-ACM serial** (PC melihat `/dev/ttyACM`).

Recon yang mengarahkan bentuk M1:

- `UsbfnDwc3Dxe` (`SamsungPkg/Drivers/UsbfnDwc3Dxe`) sudah ada dan ikut
  dibangun, tetapi **tidak pernah masuk FV** — tidak terdaftar di
  `beyond0ltePkg/Include/DXE.inc`.
- `PlatformUsbfnLib` untuk Exynos 9820 masih dipetakan ke **Null**
  (`SamsungPkg.dsc.inc`). Belum ada `Silicon/Samsung/S5E9820Pkg/Library/PlatformUsbfnLib`.
- **Tidak ada satu pun driver kelas gadget** (EEM/RNDIS/MSD/serial) di pohon
  OSS ini. `UsbMsdDxe`/`UsbDeviceDxe` hanyalah **biner Qualcomm** yang bergantung
  pada protokol Qualcomm → tidak bisa dipakai ulang. Bagian gadget = tulis dari
  nol (itu M2).
- `UsbfnDwc3Dxe` entry hanya *mengalokasi* dan *menerbitkan* protokol. PHY dan
  core baru dinyalakan ketika sebuah driver kelas memanggil `StartController`
  (yang di dalamnya memanggil `UsbfnPlatformPhyInit` + `Dwc3CoreInit`).
  `ConfigureEnableEndpoints` (yang menyetel `DCTL.RUN_STOP`) juga ada di sisi
  kelas. Jadi M1 = buktikan rantai bring-up lewat `StartController` langsung.

#### Yang ditambahkan

| Berkas | Isi |
| --- | --- |
| `Silicon/Samsung/S5E9820Pkg/Library/PlatformUsbfnLib/PlatformUsbRegisters.h` | Peta register 9820: `CTRL_BASE=0x10B00000` (USBCON), `LINK_BASE=0x10C00000` (DWC3), PMU `0x15860000+0x72C`, offset USBCON gaya 7885 (termasuk `SSPPLLCTL 0x30`) |
| `Silicon/Samsung/S5E9820Pkg/Library/PlatformUsbfnLib/BoardInit.c` | Urutan PHY dari templat 7885/9830; `UsbfnPlatformGetData/PhyInit/PhyExit/IsVbusPresent` |
| `Silicon/Samsung/S5E9820Pkg/Library/PlatformUsbfnLib/PlatformUsbfnLib.inf` | `FILE_GUID 7E2A5C41-...`; butuh `MemoryAllocationHelperLib` |
| `S5E9820Pkg.dsc.inc` | `PlatformUsbfnLib` → inf 9820 (menimpa Null) |
| `beyond0ltePkg/Include/DXE.inc` | `INF SamsungPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.inf` |
| `B2LPlatformBootManager.c` | `B2LUsbDevBringUp`/`B2LDrawUsbDev` (`USB.dev`, `USB.ctl`) + cek hijau `GSNPSID` kini menerima `0x3331` |

`beyond2lte` **tidak disentuh**.

#### Keputusan penting: `MapMemoryRegion` (kembali)

Pelajaran dari crash v1 (Data abort, `FAR=0x10B00000`) dipakai lagi di sini:
`UsbfnPlatformPhyInit()` memanggil `UsbMapRegions()` **sebelum** menyentuh
register apa pun, memetakan kedua region (`CTRL` dan `LINK`) sebagai
`EfiMemoryMappedIO`. Bila sudah terpetakan, `MapMemoryRegion` mengembalikan
`EFI_ALREADY_STARTED`/`EFI_ACCESS_DENIED` — itu dianggap "sudah baik", bukan
galat. Templat 9830/7885 tidak melakukan ini karena asumsinya MMIO sudah
terpetakan oleh firmware lain; di S10e S-Boot tidak melakukannya.

Lalu `UsbfnPlatformPhyInit` membaca `CTRL_VER` (USBCON `0x10B00000`) sebagai
gerbang: `0`/`0xFFFFFFFF` → `EFI_DEVICE_ERROR` (blok PHY tidak hidup), selain itu
lanjut ke urutan reset PHY.

#### Asumsi M1 yang belum terverifikasi

1. **Clock USB sudah hidup dari S-Boot.** Probe Fase 0 berhasil membaca
   `CTRL_VER=0x00030000`, jadi blok USBCON terjangkau; CMU/PLL 9820
   (CMU `0x1A240000`, gate `aclk`/`sclk`) **sengaja dilewati** dulu. Bila PHY
   tidak bangkit, langkah berikutnya adalah menambah bring-up CMU/PLL.
2. **Tuning HSP dilewati.** Node `usb_hs_tune`/`usb_ss_tune` di DTB stok
   berstatus `"disabled"`, jadi nilai tune tidak diterapkan pada putaran ini.
3. **`HSP_FSV_OUT_EN`** (7885 BIT24) sengaja tidak disentuh.
4. `MaximumSpeed = High`, `SuperSpeedPhyDisabled = TRUE`, UTMI 16-bit — USB 2.0
   dulu. SuperSpeed (combo PHY USB3 + SS PLL + PIPE3) menyusul bila perlu.

#### Apa yang diharapkan di layar

Dua baris baru menggantikan cek kosmetik lama:

```
USB.dev : st=Success
USB.ctl : <GCTL> <DCTL> <DSTS>
```

- `st=` adalah hasil `StartController` (hijau bila sukses). `st=Success`
  berarti `UsbfnPlatformPhyInit` + `Dwc3CoreInit` selesai tanpa galat.
- `USB.ctl` GCTL seharusnya tetap `PRTCAPDIR=DEVICE` (2). `DCTL.RUN_STOP`
  kemungkinan masih **0** dan `DSTS` menunjukkan halted, karena **belum ada
  yang connect/enable endpoint** — itu tugas driver kelas (M2). Jadi M1
  membuktikan **bring-up**, bukan enumerasi.
- Bila `st=` galat atau `DCTL`/`DSTS` `0xffffffff` → iterasi
  `BoardInit.c` (tambah CMU/PLL, atau setel dari DTB).

#### Hasil di perangkat (M1)

Flash di S10e: **`st=Success`** (hijau) dan `USB.ctl` menunjukkan nilai
bermakna, bukan `0xffffffff`:

```
USB.dev : st=Success
USB.ctl : 30C12004 00F00000 00D20001
```

Decode (`Dwc3.h`):

- **GCTL = `0x30C12004`** → `PRTCAPDIR` (bit 13:12) = `0b10` = **2 = DEVICE**;
  `U2EXIT_LFPS` (bit 2) set; `U2RSTECN` (bit 16) set. Sama dengan probe Fase 0 →
  `Dwc3CoreInit` memang menegakkan DEVICE, dan memang sudah DEVICE.
- **DCTL = `0x00F00000`** → `RUN_STOP` (bit 31) = **0**, `CSFTRST` (bit 30) = 0,
  `KEEP_CONNECT` (bit 19) = 0. Kontroler **berhenti** — persis yang diharapkan
  karena belum ada driver kelas yang memanggil `ConfigureEnableEndpoints`
  (bit 20..23 yang menyala adalah default pasca-reset, di luar daftar define).
- **DSTS = `0x00D20001`** → `CONNECTSPD` (bit 2:0) = `1` = **Full Speed**;
  `USBLNKST` (bit 21:18) = `4` = **`HS_DISCONNECTED`**; `RXFIFOEMPTY` (bit 17)
  = 1; `DEVCTRLHLT` (bit 22) = 0; `COREIDLE` (bit 23) = 0. Belum ada link
  partner aktif (kontroler tidak connect).

Catatan: `USB.pmu`/`USB.con`/`USB.dw3`/`USB.xhc` **dibaca sebelum** bring-up
(urutan `B2LUsbRead` → `B2LUsbDevBringUp`), jadi `USB.pmu = 0` wajar — snapshot
pra-bring-up. Bila perlu, pembacaan ulang PMU pasca-bring-up ditambahkan di M2.

**Kesimpulan M1:** rantai `UsbfnPlatformPhyInit` (USBCON/PHY + PMU) →
`Dwc3CoreInit` (core DWC3) **berhasil** pada Exynos 9820, tanpa hang dan tanpa
Data abort. Fondasi device-mode siap; enumerasi ke PC menunggu driver kelas
(M2 = CDC-ACM), yang akan memanggil `ConfigureEnableEndpoints` untuk menyetel
`RUN_STOP` dan mengaktifkan endpoint.

#### Bundel flash (M1)

`bash tools_flash_bundle.sh -d beyond0lte -o ~/Downloads`

| Berkas | SHA-256 |
| --- | --- |
| `~/Downloads/beyond0lte-flash/uefi-boot.tar` | `7c0df366197f969440a3ccfd07b5a88c772b5570c97072a684c4fd0f373b7671` |
| `~/Downloads/beyond0lte-flash/Mu-beyond0lte.img` | `af88cb69931ffc3f881de8739f6987b1fb3cf4998d14e3868a07e112f027ee08` |
| `~/Downloads/beyond0lte-flash.zip` | `5f140c8b9edbd6d96e137a67fa85a2e4602175c86908e3c83821c726511bb634` |

Build: `YAP_RC=0`, **67 image**. `UsbfnDwc3Dxe` (GUID
`0BF8B0C9-30E6-4E2A-9E1D-5F6C1A47D3B2`) ada di `FVMAIN` offset `0x000F2630`.
`PlatformUsbfnLib` 9820 (`BoardInit.obj`) terkompilasi; string `USB.dev`/`USB.ctl`
ada di `BdsDxe.efi`.

> **Catatan FVMAIN:** `FVMAIN` (tak terkompresi) adalah FV yang **auto-size**
> (`NumBlocks=0`): ia tumbuh mengikuti isi, jadi angka "sisa" di `FVMAIN.Fv.map`
> hanya sesaat dan bukan batas nyata. Batas nyata adalah **`FVMAIN_COMPACT`**
> yang ukurannya tetap (`0x400000`, sebesar FD). Di M1 ia terpakai `0xd1e18`
> (lihat §6.5 untuk angka M2a) — masih sangat lega.

### 6.5 M2a: driver kelas gadget — buktikan enumerasi dulu

Kelas gadget target akhirnya **CDC-ACM serial** (PC melihat `/dev/ttyACM`). Karena
enumerasi dan mesin kontrol adalah bagian paling rapuh (dan paling sulit dibedakan
saat gagal), M2 dikerjakan **bertahap**: **M2a** hanya membuktikan PC
mengenumerasi perangkat dan mesin Setup berjalan; **M2b** baru menambahkan
descriptor CDC-ACM + request kelas + echo bulk.

#### Batasan `EFI_USBFN_IO_PROTOCOL` yang membentuk desain

Recon `SamsungPkg/Drivers/UsbfnDwc3Dxe` menunjukkan kontrak protokol ini sangat
sempit (memang begitu spec-nya):

- `ConfigureEnableEndpoints` hanya membaca `ConfigInfoTable[0]->InterfaceInfoTable[0]`
  → **satu konfigurasi, satu interface**.
- Hanya endpoint **Bulk** diterima; non-Bulk → `EFI_UNSUPPORTED`
  (tidak ada interrupt/isochronous di protokol ini).
- `DeviceDescriptor`/`ConfigDescriptor` **tidak dibaca** controller; controller
  hanya butuh daftar endpoint untuk diaktifkan. Jadi descriptor yang dijawab ke
  host sepenuhnya milik driver kelas.
- `SET_ADDRESS` ditangani controller sendiri (tulis `DCFG`), **tidak** diteruskan
  ke driver kelas. `SET_CONFIG`/`SET_INTERFACE` mereset endpoint non-control lalu
  diteruskan. Sisanya (`GET_DESCRIPTOR`, `GET_STATUS`, ...) diteruskan sebagai
  `EfiUsbMsgSetupPacket` dan **harus** dijawab driver kelas lewat `Transfer` EP0.
- `ConfigureEnableEndpoints` = titik yang menyetel `DCTL.RUN_STOP`.

#### Yang ditambahkan

| Berkas | Isi |
| --- | --- |
| `beyond0ltePkg/Drivers/B2LUsbSerialDxe/B2LUsbSerialDxe.c` | Driver kelas M2a. Entry: `StartController` → `AllocateTransferBuffer` (EP0) → `ConfigureEnableEndpoints` → timer 1 ms (TPL_CALLBACK) yang memompa `EventHandler`. Menjawab request standar (GET_DESCRIPTOR device/config/string, GET_STATUS, GET_CONFIG/GET_INTERFACE, SET_CONFIG/SET_INTERFACE, SET/CLEAR_FEATURE). |
| `beyond0ltePkg/Drivers/B2LUsbSerialDxe/B2LUsbSerialDiag.h` | Protokol teshis `B2L_USB_SERIAL_DIAG_PROTOCOL_GUID`, pola sama dengan `B2LTouchDiag.h` (GUID/yerlesim lokal di kedua modul, Signature `"BUD1"`). |
| `beyond0ltePkg/Drivers/B2LUsbSerialDxe/B2LUsbSerialDxe.inf` | `FILE_GUID 7C4B1E92-3A85-4D67-B0F2-9E5C8A1D4F30`; `Depex gEfiUsbFunctionIoProtocolGuid`. |
| `beyond0ltePkg/beyond0lte.dsc` `[Components]` | `beyond0ltePkg/Drivers/B2LUsbSerialDxe/B2LUsbSerialDxe.inf` (penyebab build `F001` sebelumnya: INF di `DXE.inc` saja tidak cukup — harus juga di `[Components]` DSC). |
| `beyond0ltePkg/Include/DXE.inc` | `INF .../B2LUsbSerialDxe.inf` (masuk FVMAIN). |
| `B2LPlatformBootManager.c` | Baris live `USB.ser` + mirror `B2L_USB_SERIAL_DIAG` (GUID/struct lokal). |

Descriptor M2a sengaja minimal: **1 konfigurasi, 1 interface** (vendor `0xFF`,
**tanpa endpoint**), `VID 0x04E8 / PID 0x2143`. Artinya hanya EP0 yang aktif;
tujuannya memisahkan "enumerasi jalan?" dari "CDC-ACM benar?".

`beyond2lte` **tidak disentuh**; tidak ada perubahan di layer SoC untuk M2a.

#### Bagaimana bukti dibaca

Baris `USB.ser` di self-test:

- **Merah `USB.ser: s=N st=%r`** → driver berhenti di tahap `N` dengan status
  `%r` (rentetan `ENTRY`→`INSTALL`→`LOCATE`→`START`→`BUFFER`→`CONFIG`→`TIMER`→
  `DONE`; lihat `B2LUsbSerialDiag.h`). Menunjukkan **tepat** adegan mana yang gagal.
- **Hijau `USB.ser: su=NN cf=1 ad=N`** → enumerasi berhasil: `su` = jumlah setup
  yang diterima, `cf=1` = `SET_CONFIGURATION` diterima, `ad` = alamat USB yang
  ditulis controller ke `DCFG` (dibaca langsung dari register, bukan dari driver).
- **Abu-abu `USB.ser: no diag protocol`** → protokol teshis tidak ketemu
  (module tidak jalan atau Signature/Size tak cocok).

Di PC, saat perangkat terhubung: `lsusb` harus menampilkan `04e8:2143`
(Mu-Silicium / Samsung Exynos Gadget) dan kernel mencatat perangkat baru di
`dmesg`.

#### Bundel flash (M2a)

`bash tools_flash_bundle.sh -d beyond0lte -o ~/Downloads`

| Berkas | SHA-256 |
| --- | --- |
| `~/Downloads/beyond0lte-flash/uefi-boot.tar` | `ab7db61b766c3ead596b0e52b7f7343f66f6093ba2e9b20e1ae72d5fe04d2bb3` |
| `~/Downloads/beyond0lte-flash/Mu-beyond0lte.img` | `11185c0190758f12789558611973b0751fde38f23d144eb877a8d01d8ca0617b` |

> **Catatan hash:** setiap build menyisipkan cap waktu, jadi SHA-256
> `uefi-boot.tar`/`Mu-beyond0lte.img` **berubah tiap rebuild** meski sumbernya
> sama. Angka di atas hanya berlaku untuk bundel di `~/Downloads` saat ini.

Build: `YAP_RC=0`, **68 image**. `B2LUsbSerialDxe` (GUID
`7C4B1E92-3A85-4D67-B0F2-9E5C8A1D4F30`) ada di `FVMAIN`; string `USB.ser` ada di
`BdsDxe.efi`. `FVMAIN_COMPACT` terpakai `0xd2138` dari `0x400000` (naik ~`0x320`
dari M1) — masih sangat lega.

#### Self-test: uji sentuh disembunyikan (layar muat)

Layar self-test punya tinggi tetap (1080×2280, `B2L_UI_SATIR = 61` → ~37 baris).
Sebelum ini isinya ~37–40 baris, jadi baris bawah terpotong dan blok informasi
atas jadi tidak terbaca. Blok uji sentuh (`Touch.*`, 12–15 baris) **dihapus dari
layar** supaya semuanya muat.

Yang berubah di `B2LPlatformBootManager.c`:

- Semua `B2LUiPrint (… L"Touch.*" …)` di loop hidup dihapus; `Abs->GetState`
  polling dan penghitung `TouchPolls/TouchHits/LastTouch` ikut dihapus.
- Baris petunjuk statis `Touch.dx hit and Touch.ev co should grow` dihapus.
- **Driver sentuh tidak diubah** dan tetap ditenagai: protokol teshis
  (`B2L_TOUCH_DIAG_PROTOCOL_GUID`) masih dicari hanya untuk mengambil pointer
  `AbsolutePointer` yang asli, lalu `Abs->Reset(TRUE)` tetap dijalankan.
  Jadi sentuh tetap berfungsi sebagai perangkat input; yang hilang hanya
  tampilannya di layar.

Sisa blok hidup kini cuma **2 baris** (`Key` dan `USB.ser`), jadi layar penuh
terbaca (informasi Display/RAM/Memmap/USB di atas tidak lagi terpotong).

#### Hasil di perangkat (M2a)

Layar kini penuh terbaca. Yang terlihat:

```
USB.map : con=Access Denied dw3=Access Denied > merah
USB.pmu : 00000001 00000000 > abu
USB.con : 00000000 00000000 > abu
USB.dw3 : 00000000 00000000 > abu
USB.xhc : 00000000 00000000 > abu
USB.dev : st=Success > hijau
USB.ctl : 30C12004 80F00000 008E0001 > abu
Key     : Volume Up   -> SCAN_UP (0x01)
USB.ser : su=0 cf=0 ad=0 > kuning
```

Tafsir:

- `USB.dev : st=Success` → `StartController` (PHY+core) OK.
- `USB.ctl` → GCTL `PRTCAPDIR=DEVICE`, DCTL `RUN_STOP`(bit31)=**1** (port
  aktif), DSTS `USBLNKST`=3 (`HS_SUSPEND`). Karena PHY memaksa
  `VBUSVALID`/`BVALID` (tidak ada pad VBUS nyata), "suspend" di sini **bukan**
  bukti ada host; tanpa host pun state ini muncul.
- **`su=0 cf=0`** → belum ada satu pun SETUP; yaitu **host/PC belum
  mengenumerasi**. Sebab paling mungkin: kabel tidak tersambung ke PC (atau ke
  port yang tidak melakukan enumerasi) saat self-test.

#### Perbaikan cepat pasca-M2a

1. **`USB.map` merah (`Access Denied`) adalah regresi tampilan, bukan
   kesalahan nyata.** `B2LUsbSerialDxe` sudah memetakan region USB lebih dulu
   (lewat `StartController` di DXE dispatch), jadi `B2LUsbRead` menerima
   `EFI_ACCESS_DENIED`. Ini kini dianggap "sudah terpetakan" (`B2LMapOk`),
   sehingga `USB.con/dw3/xhc` kembali terisi nilai asli.
2. **`USB.ser` kini memuat `pm` dan `ev`** untuk membedakan sebab `su=0`:
   - `pm` = berapa kali pompa olah-event jalan. `pm=0` berarti pompa **tidak
     pernah** jalan (masalah time-timer/TPL) — device tak akan pernah menjawab.
   - `ev` = jumlah bus event (reset/attach/detach/suspend). `ev>0` berarti
     host **ada** dan sudah menyentuh kita, walau `su=0`.
   Jadi bacaan yang diinginkan: `pm` besar, dan bila PC tersambung `ev≥1`.

##### Verifikasi: SUKSES (M2a selesai)

Setelah perbaikan, dengan kabel tersambung ke PC:

```
USB.map : (satir hilang - kedua region terpetakan)
USB.con : 00030000 00001004 > abu
USB.dw3 : 33313130 30C12004 > hijau
USB.xhc : 01100020 02000240 > hijau
USB.ctl : 30C12004 80F00000 008E0001
USB.ser: su=18 cf=1 ad=40 pm=7770 ev=12 > hijau
```

- **`su=18`** → 18 SETUP diterima; **`cf=1`** → `SET_CONFIGURATION` diterima
  (host mengonfigurasi perangkat). **`ad=40`** → alamat USB `0x28` (`DCFG`).
- **`pm=7770`** → pompa olah-event hidup normal; **`ev=12`** → 12 bus event.
- **Kesimpulan M2a:** pengendali DWC3 device-mode pada Exynos 9820
  **berhasil dienumerasi oleh PC**. Rantai `StartController` →
  `ConfigureEnableEndpoints` → mesin Standart-Request milik driver kelas
  berjalan. Ini fondasi yang diperlukan M2b (CDC-ACM).

### 6.6 M2b: kelas gadget CDC-ACM (serial `/dev/ttyACM*`)

Tujuan: dengan kabel ke PC, perangkat muncul sebagai **port serial** — Linux
`/dev/ttyACM0` (driver `cdc_acm`), Windows `usbser`. Di layar self-test muncul
tiga baris `USB.ser` berisi bukti enumerasi + lalu lintas data.

#### Mengapa notification interrupt EP itu wajib

Pilihan pertama kami adalah CDC-ACM **tanpa** notification interrupt endpoint
(control interface tanpa endpoint) supaya tidak perlu menyentuh driver DWC3
bersama. Itu **tidak bisa jalan**, dan bukan karena "mungkin": driver Linux
`cdc_acm` menolaknya secara eksplisit. Bukti dari source kernel v6.14,
`drivers/usb/class/cdc-acm.c`, `acm_probe()`:

```c
if (data_interface->cur_altsetting->desc.bNumEndpoints < 2 ||
    control_interface->cur_altsetting->desc.bNumEndpoints == 0)
        return -EINVAL;

epctrl = &control_interface->cur_altsetting->endpoint[0].desc;
```

`acm_probe()` menolak perangkat yang control interface-nya tidak punya endpoint
(`-EINVAL`), lalu **tanpa syarat** membaca `endpoint[0]`. Pemeriksaan ini ada
sejak 2012, jadi berlaku untuk praktis semua kernel modern. **Tanpa interrupt
notification endpoint, `/dev/ttyACM*` tidak akan pernah muncul.** Karena itu
interrupt EP dipakai (keputusan dikonfirmasi user).

#### Yang ditambahkan

1. **Driver kelas `B2LUsbSerialDxe`** (di `beyond0ltePkg`) diubah dari
   "satu interface tanpa endpoint" (M2a) menjadi **CDC-ACM dua interface**:
   - `IF0` (comm, `02/02/01`): functional descriptor header, call management,
     ACM, union + **notification interrupt IN `0x82`** (interval 10).
   - `IF1` (data, `0x0A`): **bulk OUT `0x02`** + **bulk IN `0x81`**.
   - `bDeviceClass = 0x02` (Communications). Blob config 67 byte.
   - Request kelas: `SET/GET_LINE_CODING` (7 byte), `SET_CONTROL_LINE_STATE`,
     `SEND_BREAK`. Request tak dikenal → STALL.
   - Data: begitu `SET_CONFIGURATION` diterima, bulk OUT di-arm; setiap paket
     yang masuk **di-echo** ke bulk IN. Penghitung `rx` (byte masuk), `tx`
     (byte keluar), `lc` (`SET_LINE_CODING` diterima) dinaikkan.
2. **Patch `UsbfnDwc3Dxe.c` (SamsungPkg, file upstream bersama)** lewat script
   idempotent `tools_usbfn_intr_patch.py`, dipanggil `yap.sh` tiap build.
   Driver bersama itu menolak semua endpoint non-Bulk (`EFI_UNSUPPORTED`),
   jadi tanpa patch ini interrupt EP gagal dikonfigurasi. Patch hanya menambah
   cabang interrupt → `DWC3_DEPCMD_TYPE_INTR`; **tidak** mengubah logika loop
   maupun string. Alasan memakai script (bukan menyalin file utuh) sama dengan
   `tools_ufs_patch.py`: file ini ~1000 baris upstream, salinan penuh akan
   kehilangan perbaikan upstream dan cepat basi.

#### Batasan `UsbfnDwc3Dxe` yang membentuk desain M2b

`Dwc3ConfigureEnableEndpoints` **hanya** menelusuri `InterfaceInfoTable[0]`
(tidak berubah, sengaja dibiarkan minimal). Karena itu `B2LUsbSerialDxe`
menggabungkan **ketiga** endpoint ke satu "interface konfigurasi perangkat
keras" sintetik (`mHwEndpointTable`, `NumEndpoints = 3`) supaya ketiganya
dikonfigurasi dalam satu loop, sementara descriptor yang dikirim ke host tetap
dua interface yang benar (`mConfigBlob`). `InterfaceInfoTable[0]` dan
`ConfigDescriptor` wajib non-NULL.

#### Perubahan tampilan self-test

Baris `USB.ser` (yang di M2a hanya satu baris enumerasi) kini menjadi tiga:

```
USB.ser: s=<n> st=<status>            -> merah bila belum Stage DONE
USB.ser: su=.. cf=.. ad=.. pm=.. ev=..  -> hijau bila cf=1
USB.ser: rx=.. tx=.. lc=..              -> hijau bila rx/tx != 0
```

- `su` setup count, `cf` SET_CONFIGURATION (1 = host mengonfigurasi), `ad`
  alamat USB (`DCFG`), `pm` jumlah iterasi pompa, `ev` jumlah bus event.
- `rx`/`tx`/`lc` = lalu lintas bulk/line-coding (lihat di atas). Nol bila PC
  belum mengirim data.

#### Bundel flash (M2b)

Kabel ke **PC**, Download Mode, Odin `AP = uefi-boot.tar`, **Re-Partition tidak
dicentang**. Di PC, setelah boot: `modprobe cdc_acm`, `dmesg | grep cdc_acm`,
`lsusb` (diharapkan `04e8:2143`), lalu `/dev/ttyACM*`. Uji echo:

```
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0 &
echo HELLO > /dev/ttyACM0
```

Balasan `HELLO` = jalur data dua arah bekerja. Foto baris `USB.ser` sebagai
bukti (`rx`/`tx` harus naik).

##### Verifikasi: SUKSES (M2b selesai)

Dengan kabel ke PC:

Sisi Linux (laptop):

```
$ lsusb | grep 04e8
Bus 001 Device 036: ID 04e8:2143 Samsung Electronics Co., Ltd Samsung Exynos Gadget

$ sudo dmesg | grep -i cdc_acm
cdc_acm 1-4:1.0: ttyACM0: USB ACM device
usbcore: registered new interface driver cdc_acm

$ ls /dev/ttyACM*
crw-rw---- 166,0 root /dev/ttyACM0

$ echo HELLO > /dev/ttyACM0   # cat /dev/ttyACM0 di terminal lain
HELLO
```

Sisi ponsel (layar self-test):

```
USB.ser: su=19 cf=1 ad=42 pm=35701 ev=6
USB.ser: rx=24 tx=24 lc=1
```

- `usbcore` + `cdc_acm` bind ke interface data (`1-4:1.0`/`1.1`) → `/dev/ttyACM0`
  muncul. VID:PID `04e8:2143`.
- Echo dua arah: `HELLO` dikirim dari PC kembali utuh. Di layar **`rx=24
  tx=24`** (24 byte masuk, 24 byte di-echo) dan **`lc=1`** (satu
  `SET_LINE_CODING` dari `stty`).
- **Kesimpulan M2b:** jalur **device/gadget CDC-ACM berfungsi penuh** —
  enumerasi, kontrol (kelas), dan data bulk dua arah. Ini juga mengonfirmasi
  patch interrupt EP (`tools_usbfn_intr_patch.py`) berhasil: tanpa itu Linux
  `acm_probe()` akan `-EINVAL` dan `/dev/ttyACM0` tak akan ada.

#### Perbaikan tampilan: "bagian atas blank"

Gejala: baris paling atas layar self-test kosong dan blok statis (banner
"S10e … UEFI SELF TEST" + info display/RAM/memmap/USB) **hilang**, digantikan
satu baris kecil firmware `Connect ConIn in first ReadKeyStoke in Lazy ConIn
mode.`; hanya blok hidup (`Key`, `USB.ser`) yang terlihat.

Sebab: ConIn berjalan dalam **mode lazy** (PcdConInConnectOnDemand). Panggilan
`ReadKeyStroke` **pertama** menandai `ConnectConInEvent` (bukti source:
`MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitter.c`, pesan itu
dibuat tepat sebelum `gBS->SignalEvent`). Notify-nya baru dijalankan saat TPL
diturunkan — yaitu di **iterasi pertama loop canli**, yang sudah **setelah**
blok statis digambar. Saat konsol menangani connect itu, ia menggambar ulang
teks (dan membersihkan framebuffer), sehingga blok statis kami terhapus. Blok
kanan hidup tetap muncul karena digambar ulang setiap frame.

Perbaikan: pemicu lazy-connect **dijalankan lebih dulu, sebelum menggambar** —
satu `ReadKeyStroke` kosong + satu pompa TPL (`RestoreTPL(RaiseTPL
(TPL_HIGH_LEVEL))`) menyelesaikan penggambaran ulang konsol; setelah itu layar
dibersihkan dan blok statis digambar di atasnya. Karena flag-nya sekali-jalan
(`mConInIsConnect`), loop canli tidak memicunya lagi. Perubahan hanya di
`B2LPlatformBootManager.c`.

Terverifikasi: setelah flash, seluruh blok tampil dari paling atas — banner,
`Display 1080x2280`, `RAM 6144 MB DRAM, 2079 MB free`, `Memmap 63 desc`,
`Block IO 37 device`, `USB.*`, lalu `Key` + `USB.ser: su=14 cf=1 ad=44 pm=10316
ev=6` / `rx=6 tx=6 lc=1`. Baris kecil `Touch: sec_ts id=…` di paling atas
adalah keluaran konsol driver sentuh (normal). Tidak ada lagi bagian kosong.

### 6.7 M3: konsol UEFI lewat USB serial (input + output)

Tujuan: kabel USB menjadi **konsol UEFI interaktif** — keyboard PC masuk ke
`gST->ConIn`, teks UEFI keluar ke terminal PC — tanpa mengubah layar self-test
(yang tetap digambar langsung ke framebuffer lewat GOP/HII).

#### Bagaimana konsol dibentuk di platform ini

Konsol tidak muncul otomatis. `SiliciumPkg/Library/BootManagerLib/BootManager.c`
(`PlatformBootManagerBeforeConsole`) mendaftarkan device path secara eksplisit:

```c
EfiBootManagerUpdateConsoleVariable (ConIn,  &KeypadDevicePath,      NULL);
EfiBootManagerUpdateConsoleVariable (ConIn,  &UsbKeyboardDevicePath, NULL);
EfiBootManagerUpdateConsoleVariable (ConOut, &DisplayDevicePath,     NULL);
EfiBootManagerUpdateConsoleVariable (ErrOut, &DisplayDevicePath,     NULL);
DeviceBootManagerBeforeConsole (NULL, NULL);   // <- hook milik kita
```

`BdsEntry` lalu menyambung konsol: ConOut/ErrOut **eager**, ConIn **lazy**
(dipicu `ReadKeyStroke` pertama). Rantainya:

```
device path (variabel ConIn/ConOut) -> ConPlatform (menempel
gEfiConsoleIn/OutDeviceGuid) -> ConSplitter (menggabungkan) -> gST->ConIn/ConOut
```

Karena itu pendekatannya **konsol standar**, bukan pipa khusus: `B2LUsbSerialDxe`
menerbitkan `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` + `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL`
+ satu `EFI_DEVICE_PATH_PROTOCOL` (vendor GUID khusus), lalu
`DeviceBootManagerBeforeConsole` milik kita menambahkan device path itu ke
**ConIn, ConOut, dan ErrOut**. ConSplitter menggabungkannya dengan
Keypad/Display. Tidak ada perubahan pada ConSplitter, ConPlatform, maupun
SerialPortLib.

#### Yang ditambahkan

1. **Input (M3a).** Setiap bayt Bulk OUT didekode jadi tombol ke queue 64 entri
   (`ReadKeyStroke`): printable apa adanya; `BS`/`TAB`/`LF`/`CR` sebagai
   karakter; `DEL` -> `SCAN_DELETE`; `ESC [ A/B/C/D/H/F` ->
   Up/Down/Right/Left/Home/End; `ESC O P/Q/R/S` -> F1..F4; `ESC` tunggal ->
   `SCAN_ESC`. `WaitForKey` (EVT_NOTIFY_WAIT) dinyalakan hanya saat queue
   terisi, mengikuti pola `SimpleTextInOutSerial`.
2. **Output (M3b).** `OutputString` -> TX **ring 4 KiB** -> Bulk IN. Karena bulk
   IN hanya satu dan harus satu transfer in-flight, pengiriman dipompa:
   `Transfer` sekali, lalu `EndpointStatusChangedTx` menurunkan flag dan memompa
   lanjutan. Echo input (M2b) memakai jalur TX yang sama; `Reset` mengosongkan
   ring.
3. **Device path.** Vendor device path (`HW_VENDOR_DP`) dengan GUID
   `6d2b8f41-3c7a-4e19-8ad5-2b904c6f13e7`; GUID yang sama diulang di
   `B2LPlatformBootManager.c` supaya ConPlatform memasangkannya.
4. **Robust tanpa PC.** Bila bring-up USB gagal (tidak ada kabel, depex belum
   siap, dsb.), driver **tetap `return EFI_SUCCESS`** setelah menerbitkan
   protokol konsol: protokol tetap resident (boot manager tetap bisa
   mendaftarkannya) dan layar self-test tetap menunjukkan tahan di stage mana.
   Kegagalannya tetap terlihat pada baris `USB.ser: s=.. st=..`.
5. **Mirror self-test.** `B2LUsbSerialDiag.h` menambah `UsbConIn`, `UsbConOut`
   (pointer STO), `KeyReads`, `ConsoleOut`. Self-test menulis ke PC lewat
   **pointer STO itu langsung** (bukan `gST->ConOut`), jadi jalur tampilan
   GOP/HII sama sekali tidak tersentuh; baris status diperbarui dengan `\r`.

#### Mode ConOut: mengapa 200x60

`QueryMode` melaporkan satu mode (0) berukuran 200x60. Perangkat Display sudah
lebih dulu masuk daftar, dan ConSplitter **tidak** meng-interseksi mode 0 dan 1
(keduanya dicadangkan UEFI: 80x25 dan 80x50), jadi mode layar tidak berubah.
`PcdConOutColumn/Row` (default 80/25) membuat `PreferMode = 0` sehingga tidak ada
`SetMode` per-perangkat yang dijalankan. Mode besar dilaporkan agar agregat tidak
menyusut.

#### Uji (tanpa shell/ESP)

```
modprobe cdc_acm
lsusb                         # 04e8:2143
sudo picocom /dev/ttyACM0 -b 115200
```

Lalu ketik di PC. Bukti di terminal PC:

```
Key: (not yet)   rx=0 tx=23822 co=398812 kr=0
Key: Scan=0x0000 Uni=0x0064   rx=111 tx=97685 co=117791 kr=111
Key: Scan=0x0000 Uni=0x0061   rx=134 tx=173435 co=193518 kr=134
```

##### Verifikasi: SUKSES (M3 selesai)

- **Output:** `co` (`OutputString`) dan `tx` (byte keluar lewat USB) naik, dan
  baris self-test muncul di terminal PC. `co` jauh lebih besar dari `tx` karena
  ring dibuang saat penuh sebelum PC membaca — normal.
- **Input:** `rx` naik dan **`kr` mengikutinya** (bayt PC benar-benar dibaca UEFI
  sebagai tombol); huruf ter-decode benar (`0x64`='d', `0x61`='a').
- **Layar:** self-test tetap utuh dan baris `Key :` di layar menunjukkan angka
  yang sama dengan di PC — membuktikan `gST->ConIn` agregat mengalirkan keypress
  USB **dan** registrasi ConOut tidak merusak jalur tampilan.

Catatan: alat bantu monitor serial stdlib disertakan di
`tools_serial_monitor.py`:

```
sudo python3 tools_serial_monitor.py /dev/ttyACM0 115200
```

Keluar dengan Ctrl-] (atau cabut kabel USB — port terputus akan mengakhiri
monitor). Karena `/dev/ttyACM0` milik `root:uucp`, jalankan dengan `sudo` atau
tambahkan user ke grup `uucp` (`sudo usermod -aG uucp $USER`, lalu login ulang).

---

### 6.8 M3c: EFI Shell interaktif lewat konsol USB

Setelah M3, `ConIn`/`ConOut` sudah memuat konsol USB serial, tetapi BDS tetap
berakhir di self-test. Alasannya: `MsBootOptionsLib` yang dipakai platform ini
(`SiliciumPkg`) hanya mendaftarkan opsi **"Internal Storage"** dan
**"USB Storage"** (keduanya menunjuk `MsBootPolicy`). Di S10e belum ada
partisi FAT/ESP, jadi kedua opsi itu gagal, lalu BDS memanggil
`PlatformBootManagerUnableToBoot` → layar self-test.

Padahal **EFI Shell sudah ada di FV** (`Shell.inf`, `FILE_GUID =
7C04A583-9E3E-4F1C-AD65-E05268D0B4D1` = `PcdShellFile`), dan `BootManagerMenuApp`
+ `MsBootPolicy` juga ikut ter-build ke FV. Yang kurang hanya **entri boot**
yang menunjuk Shell.

M3c menambahkan entri itu.

#### Yang ditambahkan

- `Library/PlatformBootManagerLib/B2LPlatformBootManager.c`
  - fungsi baru `B2LRegisterShellBootOption()`:
    1. `LocateHandleBuffer(gEfiFirmwareVolume2ProtocolGuid)`;
    2. untuk setiap FV, `ReadSection(PcdShellFile, EFI_SECTION_PE32)` untuk
       memastikan Shell benar-benar ada di FV itu;
    3. bangun device path `Fv()/FvFile(7C04A583-...)` (`EfiInitializeFwVolDevicepathNode`
       + `AppendDevicePathNode`);
    4. `EfiBootManagerInitializeLoadOption(..., LOAD_OPTION_ACTIVE |
       LOAD_OPTION_CATEGORY_APP, L"UEFI Shell", ...)`;
    5. idempoten: `EfiBootManagerFindLoadOption` mencegah duplikat antarboot;
    6. `EfiBootManagerAddLoadOptionVariable(&opt, (UINTN)-1)` → ditambahkan di
       **akhir** `BootOrder` (setelah SSD/USB).
  - dipanggil dari `DeviceBootManagerBeforeConsole`, tepat setelah ketiga
    `EfiBootManagerUpdateConsoleVariable(ConIn/ConOut/ErrOut, ...)`.
    Titik ini dijalankan `PlatformBootManagerBeforeConsole` **setelah**
    `MsBootOptionsLibRegisterDefaultBootOptions()`, jadi SSD/USB sudah
    terdaftar dan Shell menyusul di belakangnya.
- `Library/PlatformBootManagerLib/B2LPlatformBootManagerLib.inf`
  - `[Packages]` + `PcBdsPkg/PcBdsPkg.dec`;
  - `[LibraryClasses]` + `DevicePathLib`, `PcdLib`;
  - `[Protocols]` + `gEfiFirmwareVolume2ProtocolGuid`;
  - `[Pcd]` + `gPcBdsPkgTokenSpaceGuid.PcdShellFile` (GUID Shell dari DSC —
    satu sumber kebenaran dengan `MsBootOptionsLib`).

#### Alur boot sekarang

```
BDS -> "Internal Storage" (gagal: belum ada ESP)
    -> "USB Storage"      (gagal)
    -> "UEFI Shell"       (FV)  -> prompt Shell interaktif di konsol USB
```

Self-test tetap dipertahankan di `DeviceBootManagerUnableToBoot` sebagai
**fallback** (hanya muncul bila Shell tidak ditemukan/gagal).

#### Mode ConOut

`ConSplitter` **tidak** meng-interseksi mode 0/1 (lihat komentar
`ConSplitterGetIntersection`), jadi agregat `gST->ConOut` tetap sah di mode 0:
output Shell tampil di layar (GraphicsConsole 80×25) **dan** dikirim ke
CDC-ACM Bulk IN (200×60) sehingga terbaca di terminal PC.

#### Uji

1. Flash `~/Downloads/beyond0lte-flash.zip` (Odin `AP=uefi-boot.tar`,
   Re-Partition **tidak** dicentang).
2. Sambungkan kabel USB ke PC, pantau port serial:
   ```
   sudo python3 tools_serial_monitor.py /dev/ttyACM0 115200
   ```
   (atau `picocom /dev/ttyACM0 -b 115200`)
3. Diharapkan muncul prompt `Shell>` di terminal PC, dan perintah berikut
   bekerja: `ver`, `memmap`, `bcfg boot dump`, `ls -b`, `reset`, `exit`.

#### Echo input: mengapa dimatikan di driver

Verifikasi pertama M3c berhasil, tetapi setiap karakter yang diketik muncul
**dobel** di terminal PC (`vveerr`, `bboooott`). Penyebabnya: driver M2b
menulis ulang setiap byte yang diterima ke Bulk IN (echo), sementara UEFI
Shell **juga** meng-echo lewat `ConOut` (yang memuat USB STO). Menurut spec
UEFI, cihaz `Simple Text Input` **tidak** boleh meng-echo — pemanggil (Shell,
editor) yang melakukannya. Karena itu echo di driver dihapus
(`B2LUsbRxDone` tidak lagi memanggil `B2LUsbTxPush`). Setelah ini karakter
tampil sekali. (Jalur RX sendiri tidak berubah; `rx`/`kr` tetap naik.)

#### Uji

1. Flash `~/Downloads/beyond0lte-flash.zip` (Odin `AP=uefi-boot.tar`,
   Re-Partition **tidak** dicentang).
2. Sambungkan kabel USB ke PC, pantau port serial:
   ```
   sudo python3 tools_serial_monitor.py /dev/ttyACM0 115200
   ```
   (atau `picocom /dev/ttyACM0 -b 115200`)
3. Diharapkan muncul prompt `Shell>`, dan perintah berikut bekerja: `ver`,
   `memmap`, `bcfg boot dump`, `ls -b`, `reset`, `exit`.

`lsusb` = `04e8:2143` (sama seperti M2b/M3).

#### Bundel flash (M3c)

```
~/Downloads/beyond0lte-flash.zip
  uefi-boot.tar        sha256 9600611073a29170f7cb2177bec544f96046a0e8587c4a8a0ed9d84b310a8649
  Mu-beyond0lte.img    sha256 7143c2fce8934bc63ea281be47b145167c704db615be30f758de7961e58670a5
```

##### Verifikasi: SUKSES (M3c selesai)

- **Prompt & perintah:** di terminal PC muncul `Shell>` (UEFI Interactive
  Shell v2.2, `UEFI v2.70 (Project Silicium)`). `ver`, `memmap`, dan
  `bcfg boot dump` dijalankan dan mengeluarkan hasil (`memmap` menunjukkan
  total ≈ 6 GiB; `bcfg boot dump` mencacah `Boot0000..`).
- **Dua arah:** output perintah tampil **di layar OLED** sekaligus **di
  terminal PC** — membuktikan `gST->ConOut` agregat mengalirkan ke
  GraphicsConsole dan ke CDC-ACM Bulk IN.
- **Catatan:** echo ganda pada verifikasi pertama sudah diperbaiki (lihat
  "Echo input" di atas); flash ulang dengan bundel di atas menghasilkan
  karakter tunggal.

Langkah berikutnya yang wajar: pakai Shell ini untuk menyiapkan ESP/boot
option langsung dari perangkat (mis. `bcfg boot add`, atau tulis
`\EFI\BOOT\BOOTAA64.EFI` ke partisi FAT) — pintu menuju boot OS seperti S10+.

---

### 6.9 M4: `reset` dari Shell benar-benar me-restart SoC

Perintah `reset` di EFI Shell dulu berakhir `ASSERT` dan HP hang. Sebabnya
bukan Shell, melainkan nilai register `SWRESET` di `ResetSystemLib` **level
SoC** (S5E9820) yang salah setelah commit refactor upstream.

#### Gejala

```
Shell> reset
DXE ResetSystem2: ResetType Cold, Call Depth = 1.
ASSERT [ResetSystemRuntimeDxe] ResetSystem.c(334): ((BOOLEAN)(0==1))
```

Setelah itu perangkat diam: tidak restart, tidak merespon.

#### Akar masalah

- `ResetSystemRuntimeDxe` me-link `S5E9820Pkg/Library/ResetSystemLib`
  (dikonfirmasi dari `static_library_files.lst` modul tsb), bukan jalur PSCI.
- `ResetCold()` menulis `MmioWrite32 (mPmuRegion.Address + SWRESET, TRUE)`
  — yaitu **0x1**, padahal bit pemicu reset yang benar adalah **0x2**
  (`1 << 1`). Region `PMU` sendiri terpetakan (tidak ada warning "isn't
  Mapped"), jadi tulisannya jalan — hanya nilainya salah.
- Karena register tidak memicu reset HW, `ResetCold()` kembali ke RuntimeDxe
  → `ASSERT (FALSE)` di `ResetSystem.c:334` → hang.
- Ini **regresi**: commit `db939b45` (*S5E9820: Add ResetSystemLib*) memakai
  `SWRESET_TRIGGER (1 << 1)`; commit `53172176` (*Exynos: Refactor*)
  menggantinya menjadi `TRUE`.
- Referensi (dikutip `Source.txt` commit awal): `exynos-reboot.c`
  (CruelKernel/samsung-exynos9820) → `SWRESET 0x3A00`,
  `SWRESET_TRIGGER (1 << 1)`, lalu `exynos_pmu_write(SWRESET,
  SWRESET_TRIGGER)`. S5E9830 (saudara di repo yang sama) juga memakai 0x2.

#### Perbaikan

File overlay baru (level SoC; ikut disalin ke Mu-Silicium tiap build oleh
`yap.sh`):

```
mu-silicium-overlay/Silicon/Samsung/S5E9820Pkg/Library/ResetSystemLib/ResetSystem.c
mu-silicium-overlay/Silicon/Samsung/S5E9820Pkg/Library/ResetSystemLib/Pmu.h   ← +SWRESET_TRIGGER
```

```
-  MmioWrite32 (mPmuRegion.Address + SWRESET, TRUE);
+  MmioWrite32 (mPmuRegion.Address + SWRESET, SWRESET_TRIGGER);   // 0x2
```

Karena ini layer SoC bersama, perbaikan juga berlaku untuk `beyond2lte`
(bug yang sama ada di sana; ini perbaikan, bukan perubahan perilaku yang
merusak).

#### Bundel flash (M4)

```
~/Downloads/beyond0lte-flash.zip                                    sha256 e338ebbbcaab8e385ac90bbeff93df360f21d172d7dbf41d52f4e14cab90b6d9
  uefi-boot.tar        sha256 cfaa80ff817228f17edd6e63d165ccdb55c05be1b2d5f7362bed52897601c826
  Mu-beyond0lte.img    sha256 b1d93295006c068b290f38ebfc1fcd0749cd05bfdc4f74665e22c07b1ac13fda
```

`Mu-beyond0lte.img` **byte-identik** dengan build M4 yang sudah diuji
(sha256 `b1d9329...`): perubahan pasca-verifikasi hanya metadata INF, bukan
kode firmware. Karena itu hasil uji `reset` di atas tetap berlaku.

#### Rapikan: warning `does not support LIBRARY_CLASS`

Build dulu mengeluh:

```
S5E9820Pkg.dsc.inc(81): warning: .../ResetSystemLib.inf does not support LIBRARY_CLASS HwResetSystemLib
```

Sebabnya: `ResetSystemRuntimeDxe.inf` (MU_CHANGE) memakai kelas
`HwResetSystemLib`, dan DSC memetakan kelas itu ke `ResetSystemLib.inf` ini,
tetapi INF hanya mendeklarasikan `ResetSystemLib`. Perbaikan: INF di overlay
mendeklarasikan **keduanya**, sebagai **dua baris terpisah**:

```
LIBRARY_CLASS = ResetSystemLib|DXE_RUNTIME_DRIVER
LIBRARY_CLASS = HwResetSystemLib|DXE_RUNTIME_DRIVER
```

Dua baris terpisah itu perlu: BaseTools di sini (`InfBuildData.py`,
`GetSplitValueList`) mengambil `ValueList[0]` — yaitu seluruh teks sebelum
`|` — sebagai **satu** nama kelas, jadi bentuk `"A B|T"` tidak dikenali.

Sisa satu warning berasal dari upstream (`SiliciumPkg.dsc.inc(211)` →
`ArmPsciResetSystemLib.inf`); entri itu memang tertimpa oleh mapping S5E9820
kita dan tidak memengaruhi hasil. Tidak kami sentuh karena file upstream
bersama.

#### Verifikasi: SUKSES

- Dari `Shell>`, `reset` → **HP benar-benar restart** (bukan hang).
- Tidak muncul lagi `ASSERT [ResetSystemRuntimeDxe] ResetSystem.c(334)`.

---

### 6.10 B2LTool: alat UEFI serbaguna (Faz 1+2+3)

S10e di port ini tidak punya keyboard huruf dan belum punya ESP (FAT),
sehingga EFI Shell praktis tidak bisa dipakai: `map -r` hanya memunculkan
`BLK*`, tidak ada `FS*`, jadi `fs0:` tidak pernah ada. Akibatnya semua
operasi tingkat partisi (lihat GPT, tulis imaj) sulit dilakukan.

`B2LTool` mengisi celah itu: satu aplikasi UEFI yang **ditanam di FV**
(sehingga selalu ada, tanpa ESP) dan dijalankan dari tiga pintu: menu
otomatis saat "no OS", boot option BDS, atau Shell.

#### Subperintah

```
B2LTool.efi part                 -> cetak nama + LBA + ukuran tiap partisi (read-only)
B2LTool.efi recov                -> tulis reboot-reason PMU, lalu reset (TWRP)  [lihat 6.11]
B2LTool.efi clear                -> nolkan BCB di MISC + INFORM3 di PMU
B2LTool.efi pmu                  -> cetak RSTSTAT + INFORM0..7 (read-only)
B2LTool.efi flash <file> <part>  -> tulis .img / .img.tar ke partisi, lalu verifikasi
B2LTool.efi                      -> buka menu di layar
```

Urutan argumen diambil dari `EFI_SHELL_PARAMETERS_PROTOCOL` bila dijalankan
dari Shell, atau dari `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` bila
dijalankan sebagai boot option (itulah peran `OptionalData`). Tanpa argumen
sama sekali → menu.

#### Menu (8 item; jadi pengganti layar "no OS" yang dulu langsung mematikan)

```
[1] Dump GPT (read-only)
[2] Reboot to recovery (TWRP)
[3] Clear BCB + reboot reason
[4] Flash image (ESP to partition)
[5] Dump PMU reboot reason
[6] Reboot
[7] Power off
[0] Exit (self-test screen)
```

`[4]` interaktif: memilih berkas `.img`/`.img.tar` di root volume lalu
partisi tujuan. `[0]` jatuh ke layar SELF TEST lama (§6.2) agar perilaku
"no OS" yang sudah teruji tetap bisa dipanggil.

#### Yang ditambahkan

```
mu-silicium-overlay/Platforms/Samsung/beyond0ltePkg/Applications/B2LTool/
  B2LTool.c        giris + menu + komut (part/recov/clear/pmu)
  B2LGpt.c         pemindai GPT per-LUN (block size dihormati, read-only)
  B2LFlash.c       penulis imaj + verifikasi + pencari IRS di dalam .tar
  B2LTool.h        definisi bersama (GUID, ukuran, makro PMU)
  B2LTool.inf      UEFI_APPLICATION
```

Integrasi:

- `Include/DXE.inc` + `beyond0lte.dsc` menambahkan `B2LTool.inf` ke build
  (`UEFI_APPLICATION` tidak memengaruhi urutan dispatch DXE).
- `B2LPlatformBootManagerLib.inf` menambah `gEfiLoadedImageProtocolGuid`.
- `B2LPlatformBootManager.c` menambah dua helper:
  - `B2LFvFilePath()` — menyusun jalur `Fv(...)/FvFile(guid)`, memverifikasi
    FV benar-benar memuat GUID itu lewat `ReadSection`;
  - `B2LRegisterFvApp()` — mendaftarkan boot option secara idempoten
    (kalau pasangan description+path sudah ada, tidak digandakan).
- `yap.sh` menyalin `B2LTool.efi` hasil build ke `out/` dan `esp/B2LTOOL.EFI`
  supaya bisa juga dijalankan dari Shell/ESP.

#### Dua boot option BDS

`DeviceBootManagerBeforeConsole()` mendaftarkan dua entri:

| Judul | OptionalData | Perilaku bila dipilih |
|---|---|---|
| `beyond0lte Tools` | — | buka **menu** B2LTool |
| `TWRP (recovery)` | `recov` | langsung pemicu recovery (tanpa menu) |

Keduanya berkategori **APP**, jadi BDS melewatinya pada boot normal; ia
hanya jalan bila dipilih dari menu boot.

`DeviceBootManagerUnableToBoot()` sekarang **mencoba membuka menu B2LTool
lebih dulu** (`B2LStartFvApp`); bila pengguna keluar, barulah jatuh ke layar
SELF TEST lama. Jadi alur "no OS" sekarang: menu alat → (opsional) TWRP /
flash → self-test → mati.

### 6.11 Pemicu recovery tanpa tombol: PMU INFORM3

#### Masalah

Tombol power fisik unit ini mati, sehingga kombo Samsung
`Vol Up + Bixby + Power` untuk masuk RECOVERY mustahil. Kita perlu cara
perangkat lunak untuk memerintahkan S-Boot membuka RECOVERY (TWRP).

#### Hasil reverse `sboot.bin` (G970FXXUHHXJ1)

Dua temuan yang saling mengunci:

1. **S-Boot tidak memakai BCB AOSP.** Tidak ada literal `boot-recovery`
   maupun `bootonce` di `sboot.bin`. Menulis `"boot-recovery"` ke MISC
   (dugaan awal) **diabaikan**.
2. **Modus boot dipilih dari register PMU.** Sebuah fungsi dump
   (`0xb7038`) menamai register PMU (base `0x15860000`):

   ```
   0x404 RSTSTAT   0x800 INFORM0   0x804 INFORM1   0x808 INFORM2
   0x80C INFORM3   0x840 INFORM4   0x844 INFORM5   0x84C INFORM7
   ```

   Parser boot-mode (`0xac430`, dipanggil saat init boot dari `0xa77d4`)
   membaca 32-bit dari **`0x1586080C` (= INFORM3)** dan menerjemahkannya:

   | Nilai INFORM3 | Aksi S-Boot |
   |---|---|
   | `0x12345670 \| n` | `set REBOOT_MODE = n` |
   | `0xABCD0000` | `DEBUG_LEVEL` |
   | `0xABC80000` | `DUMPSINK` |
   | **`0xABC30000`** | **"set recovery mode for boot." → RECOVERY** |
   | `0xABCE0000` | `SWITCH_SEL` |
   | `0xABCF0000` | `SUD` |
   | `0xA5600000` | MULTI-CMD |

   Setelah dibaca, register langsung di-`str wzr` → ini **mailbox satu kali**;
   karena itu nilainya harus ditulis **sebelum** reset.

Jadi `0xABC30000` ke INFORM3 inilah "offset/nilai" yang dipakai S-Boot untuk
memilih RECOVERY.

#### Implementasi

`B2LDoRecov()` (di `Applications/B2LTool/B2LTool.c`):

1. baca `RSTSTAT` (0x404) dan `INFORM3` (0x80C) lalu cetak,
2. `MmioWrite32 (0x1586080C, 0xABC30000)`,
3. baca balik (readback) dan cetak; kalau beda, cetak `WARN` tapi tetap lanjut,
4. `gRT->ResetSystem (EfiResetWarm, ...)` — di platform ini `Warm`/`Cold`
   sama-sama menuju `SWRESET` (`ResetSystemLib`, §6.9), dan nilai INFORM3
   bertahan melewati SWRESET.

`clear` juga menolkan INFORM3 (membatalkan recovery yang tertunda), dan
subperintah `pmu` mencetak seluruh register reboot-reason (read-only) untuk
diagnosa. Konstanta PMU (`B2L_PMU_*`) sengaja disalin ke `B2LTool.c` supaya
aplikasi ini tidak bergantung pada header level SoC.

`gBS->Stall()` diberi jeda pendek sebelum reset agar teks diagnostik
terkirim penuh ke konsol USB.

#### Verifikasi: SUKSES (di perangkat)

- Dari menu B2LTool **`[2] Reboot to recovery (TWRP)`** → perangkat
  **reboot dan masuk TWRP tanpa tombol apa pun**. Tidak perlu `Vol Up +
  Bixby + Power`.
- Ini juga memvalidasi entri boot BDS **`TWRP (recovery)`** (jalur kode
  yang sama, `OptionalData = "recov"`) dan jalur Shell
  `B2LTool.efi recov`.

#### Bundel flash (6.10 + 6.11)

```
~/Downloads/beyond0lte-flash/
  uefi-boot.tar        sha256 b5df05362d77e0c3750862645c991dfa037b4a842bb5967cef695028547fb8d1
  Mu-beyond0lte.img    sha256 e775f221309b4985f3ee2f3dcb44b57c04ff7c92c9d1bf95a45fda09e0a293e5
  (B2LTOOL.EFI di esp) sha256 534d1cc37d0196cec004dfe9e76adad6810c135a9d8b315eba229f91df701690
```

`uefi-boot.tar` di-flash lewat Odin slot AP (**Re-Partition JANGAN**);
UEFI masuk partisi BOOT, `sboot` tidak disentuh (rollback §7 tetap aman).

> Hash di atas adalah hasil build **setelah** perbaikan parsing argumen
> (§6.12); versi sebelumnya (`uefi-boot.tar` `3a652456...`) hanya berbeda pada
> `B2LTool` di dalam FV.

### 6.12 M5a: boot dari ESP di CACHE (`fs0:` + B2LTool dari Shell)

Tujuan M5a: membuktikan bahwa jalur "boot OS dari FAT" (ESP) benar-benar
hidup di S10e — bukan hanya Shell yang tertanam di FV. Ini pintu masuk untuk
menaruh kernel/loader di ESP.

#### Cara memunculkannya

1. Tulis ESP ke partisi **CACHE** (`~/Downloads/beyond0lte-esp/cache.img`;
   lihat §6.13). Dari Odin (`cache-esp.tar`, sparse), heimdall
   (`--CACHE cache-mini.img`), atau dari TWRP dengan `dd` (§6.13).
2. Boot, lalu di menu boot pilih **"Internal Storage"**.
3. `MsBootPolicyLibIsDevicePathBootable()` menerima setiap volume valid,
   sehingga BDS memilih SimpleFileSystem non-USB → FAT di CACHE.
4. Shell `\EFI\BOOT\BOOTAA64.EFI` dijalankan **dari ESP**, lalu otomatis
   menjalankan `startup.nsh` (skrip diagnostik yang memanggil `B2LGOSTR.EFI`).

#### Verifikasi: SUKSES (di perangkat)

Layar muncul: banner `=== BEYOND2LTE TANI ===`, `RAM MB = 0x840`
(2112 MB ≈ 2 GiB, sesuai peta memori protektif port ini), `RST STAT=20000000
YAZILIM` (software reset — wajar). Itu bukti `startup.nsh` dari ESP berjalan.

Kemudian di prompt `Shell>`:

```
map -r
  FS0: Alias(s):HD0bd:;BLK21:
      VenHw(5F2C8E91-...)/HD(28,GPT,52444E41-...636865000000)
```

`HD(28,...)` = partisi **cache** (bandingkan §6.10 / PARTITIONS.md) → **`fs0:`
ada**. `ls fs0:\` menampilkan `B2LBOOT.EFI`, `B2LGOSTR.EFI`, `B2LTOOL.EFI`,
`EFI\`, `startup.nsh`.

`fs0:\B2LTool.efi part` mencetak tabel GPT 32 partisi. Nilainya cocok dengan
PARTITIONS.md, mis. `boot LBA=27264 blocks=14080`, `recovery LBA=41344
blocks=16512`, `misc LBA=71040 blocks=256`, `cache LBA=1983488 blocks=153600`
(600 MiB). Selain 31 partisi LUN0, terlihat juga `cpefs` dari LUN lain.

#### Bug yang ditemukan & diperbaiki: parsing argumen `B2LTool`

Percobaan pertama `fs0:\B2LTool.efi part` gagal:

```
Unknown command: fs0:\B2LTool.efi
Commands: part | recov | clear | pmu | flash <file> <partition>
```

Dua sebab di `B2LToolEntry()` (`B2LTool.c`):

1. `EFI_SHELL_PARAMETERS_PROTOCOL` dicari dengan `LocateProtocol()` (handle
   global), padahal Shell memasangnya di **image handle aplikasi**. Pencarian
   gagal, argumen jatuh ke jalur lain.
2. Fallback `EFI_LOADED_IMAGE_PROTOCOL.LoadOptions` memuat **seluruh command
   line** termasuk nama program, jadi `Tok[0]` = `fs0:\B2LTool.efi`, bukan
   subkomut.

Perbaikan: (1) `HandleProtocol(ImageHandle, ...)` lebih dulu, baru
`LocateProtocol` sebagai cadangan; (2) helper `B2LProgramToken()` mendeteksi
`Tok[0]` yang berisi `\`/`/` atau berakhiran `.EFI` (harf case-insensitive)
lalu menggeser token. Jalur `OptionalData` BDS (`"recov"`) tidak terpengaruh
karena `"recov"` bukan nama program.

Setelah flash ulang ESP (B2LTool `534d1cc3...`), `fs0:\B2LTool.efi part`
langsung bekerja. Karena itu pula FV di `uefi-boot.tar` dibangun ulang agar
B2LTool di BDS/UnableToBoot memakai kode yang sama.

#### Dampak

- `fs0:` ada → ESP di CACHE resmi dapat dibaca UEFI. Kernel/loader dapat
  ditaruh di sini (langkah menuju boot OS).
- B2LTool kini dapat dipakai dari Shell dengan subperintah lengkap
  (`part`/`recov`/`clear`/`pmu`/`flash`).
- Membuktikan jalur `LoadOptions` (boot option BDS) dan Shell dua-duanya
  benar — prasyarat untuk boot option yang membawa `OptionalData`.

### 6.13 Menulis ESP ke CACHE (Odin / heimdall / TWRP)

CACHE = 153600 blok × 4096 = **600 MiB**; `cache.img` kita persis 600 MiB.

| Metode | Perintah | Catatan |
|---|---|---|
| Odin | slot AP = `cache-esp.tar` | sparse (~1,2 MB terkirim), paling andal; nama di dalam tar `cache.img` → Odin menulis ke CACHE |
| heimdall | `heimdall flash --CACHE cache-mini.img` | 32 MB; heimdall rapuh untuk berkas besar |
| TWRP `dd` | lihat di bawah | tanpa Odin/heimdall, tanpa tombol |

TWRP (paling praktis sekarang karena TWRP bisa dibuka via §6.11):

```sh
adb push ~/Downloads/beyond0lte-esp/cache.img /sdcard/
adb shell "ls -l /dev/block/by-name/ | grep -i cache"
adb shell "umount /cache 2>/dev/null; umount /dev/block/by-name/cache 2>/dev/null"
adb shell "dd if=/sdcard/cache.img of=/dev/block/by-name/cache bs=4096 conv=fsync"
adb shell "sync"
```

Verifikasi cepat: `dd if=/dev/block/by-name/cache bs=512 count=1 | hexdump -C`
harus menampilkan label `UEFIESP` (offset 43) dan `FAT16`. Lalu
**Reboot → System**. CACHE hanya berisi cache Android; system/data tak
tersentuh, rollback §7 tetap berlaku.

---

## 7. Rollback

Sama seperti BUILD.md §8. UEFI ditulis ke partisi **BOOT**, bukan ke `sboot`,
jadi Download Mode selalu bisa dicapai:

1. Bixby + Volume Down + USB → Download Mode
2. `AP` = `AP_G970F..._*.tar.md5` (firmware stok S10e), atau sideload LineageOS
   dari recovery.

`RP SWREV` S10e (G970F) harus dicek sebelum mem-flash apa pun yang lebih tua;
aturan anti-rollback sama ketatnya dengan S10+.
