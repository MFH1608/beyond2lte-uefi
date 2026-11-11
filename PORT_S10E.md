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
   S-Boot dan mencapai BDS (menampilkan pesan "No Operating System Found!"
   lalu mematikan perangkat — belum ada ESP/OS). Menu **Boot Manager** bawaan
   bisa dibuka dengan menahan **Volume Up** saat boot; tombol Volume dan
   Bixby berfungsi. Firmware lolos validasi PE (`Mu-beyond0lte.img` 4,4 MB,
   FD tepat 4 MiB di `0xC9000000`, 67 image).
   Sisa catatan khusus unit uji: tombol power fisiknya mati (§6.1);
   verifikasi tanpa shell lewat layar self-test (§6.2).

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
| `RAM` | usable/free dari memory map | usable ≈ 6144 MB, free ≠ 0 |
| `Memmap` | jumlah descriptor + alamat tertinggi | max < `0x980000000` |
| `Bank >=0x980000000` | sanity DRAM S10e | `yok (dogru)` |
| `Block IO` | perangkat block terpasang | UFS ≈ 122 GB |
| `FAT/FS` | volume FAT terpasang | `0 volume` (memang belum ada ESP) |
| `Touch` | `EFI_ABSOLUTE_POINTER_PROTOCOL` | `AbsolutePointer var` |
| `Key` / `Touch` (bawah) | uji tombol/sentuh live | berubah saat ditekan |

Keluar/matikan: **tahan Bixby, lalu tekan Volume Up/Down** (menghasilkan
`Left`/`Right`). Setelah keluar, firmware melanjutkan jalur "no OS" dan
mematikan perangkat.

Override ini hanya terpasang di `beyond0lte.dsc`. Salinan
`beyond2ltePkg` tidak disentuh, jadi jalur "no OS" S10+ tetap seperti semula.

#### Hasil uji (diisi setelah flash)

| Tanggal | SHA-256 `uefi-boot.tar` | Foto/hasil | Catatan |
| --- | --- | --- | --- |
| (menunggu flash) | `25c8f550d9a816e45cd4c9b322cfed4c7d2840ef7c2e7c6cb4be8d996a02c980` | — | — |

---

## 7. Rollback

Sama seperti BUILD.md §8. UEFI ditulis ke partisi **BOOT**, bukan ke `sboot`,
jadi Download Mode selalu bisa dicapai:

1. Bixby + Volume Down + USB → Download Mode
2. `AP` = `AP_G970F..._*.tar.md5` (firmware stok S10e), atau sideload LineageOS
   dari recovery.

`RP SWREV` S10e (G970F) harus dicek sebelum mem-flash apa pun yang lebih tua;
aturan anti-rollback sama ketatnya dengan S10+.
