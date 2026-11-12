# Media Windows ARM64 untuk beyond0lte — jalur irit

Dokumen ini menuntun dari nol sampai punya berkas boot Windows (ARM64) yang
cocok untuk perangkat S10e di port ini (`PORT_S10E.md`). Fokusnya **irit
unduhan**: kita memakai **UUP dump, satu edisi saja**, bukan ISO resmi
multi-edisi ~8 GB.

Ringkas angka:

| Jalur | Unduh | Isi yang dipakai di S10e |
|---|---|---|
| ISO resmi Microsoft (semua edisi) | ~7,9 GB | ~0,5 GB |
| **UUP dump, 1 edisi (Pro), ESD** | **~4,7 GB** | **~0,5 GB** |
| ADK WinPE add-on (butuh Windows) | ~2,8 GB | ~0,3 GB |
| Linux ARM64 dari ESP (bukan Windows) | ~0,1 GB | ~0,1 GB |

Yang **masuk ke perangkat** selalu kecil; yang besar cuma unduhannya.

Konvensi layout di sini sengaja disamakan dengan USB installer Windows supaya
`bootmgfw.efi` langsung mengenalinya:
`/efi/`, `/boot/`, `/sources/boot.wim`.

---

## 1. Prasyarat (CachyOS / Arch)

```bash
sudo pacman -S --needed aria2 cabextract wimlib chntpw cdrtools unzip
```

- `wimlib` → `wimlib-imagex` (mengekspor WinPE dari `boot.wim`).
- `cdrtools` → `mkisofs` (dipakai `convert.sh`; `genisoimage` dari `cdrkit`
  sama-sama diterima).
- `chntpw` → mengubah registry index-1 `boot.wim` saat konversi.

Kalau `chntpw` tidak ada di repo, ambil dari AUR (`chntpw`). Cek cepat:

```bash
for t in aria2c cabextract wimlib-imagex chntpw mkisofs genisoimage; do
  printf '%-14s ' "$t"; command -v "$t" || echo MISSING
done
```

Hanya butuh `aria2c`, `cabextract`, `wimlib-imagex`, `chntpw`, dan **salah
satu** dari `mkisofs`/`genisoimage`.

---

## 2. Pilih paket UUP (sehemat mungkin)

1. Buka <https://uupdump.net>.
2. Klik **Latest Public Release build** pada kolom **arm64**
   (jangan kolom x64 — salah arsitektur = tidak akan jalan).
   - Ingin lebih kecil lagi? **Windows 11 23H2 arm64** (22631) lebih ringan
     dari 26H2 karena tanpa komponen AI. Untuk uji boot pertama, versi lama
     justru sering lebih ramah.
3. Pilih build terbaru, lalu **Next**.
4. Pilih bahasa (mis. **English (United States)**).
5. **Penting untuk irit:** centang **hanya satu edisi**, mis.
   `Windows 11 Pro`. Hilangkan centang Home dan edisi lain.
   - Setiap edisi tambahan menambah payload di unduhan.
6. Method: **Download using aria2 options**.
7. Klik **Create download package** → dapat satu berkas `.zip`.

Halaman ringkasan akan menampilkan **Total download size** (untuk Pro arm64
biasanya **~4,7 GiB**). Kalau angkanya jauh lebih besar, mungkin masih
banyak edisi tercentang.

---

## 3. Unduh & konversi di Linux

```bash
mkdir -p ~/uup && cd ~/uup
unzip ~/Downloads/*_convert.zip -d .
# ekstrak ke folder tanpa spasi (hindari path ber-spasi)
ls -1          # lihat nama skrip yang benar (bisa sedikit berbeda antar-paket)

bash uup_download_linux.sh      # unduh ~4,7 GB (aria2), lalu konversi otomatis
```

Paket UUP biasanya sudah memanggil konverter di akhir `uup_download_linux.sh`,
jadi Anda langsung mendapat `.ISO`. Untuk **memilih kompresi**, jalankan
konverternya sendiri setelah unduhan (lokasi skrip bisa di root atau di
`files/` — cek hasil `ls`):

```bash
# pilih salah satu:
bash convert.sh esd        # install.esd  (LZMS solid, ISO lebih kecil)
bash convert.sh wim        # install.wim  (lebih cepat dipasang, sering > 4 GB)
# bila berada di subfolder:
bash files/convert.sh esd
```

Catatan:

- `esd` memakai kompresi LZMS (solid) → ISO lebih kecil dan **aman untuk
  batas 4 GB/file FAT32**; `wim` menghasilkan `install.wim` yang biasanya
  > 4 GB sehingga tidak muat di FAT32 (tapi lebih cepat saat dipasang).
- Proses konversi bisa lama (CPU & disk). Pastikan ruang kosong ≥ 30 GB
  (`df -h /home`). Mesin ini punya ~439 GB kosong di `/home`.
- Hasil akhir: sebuah `.ISO` arm64 di folder yang sama.
- Bila konversi berhenti karena `mkisofs` tidak ada → pasang `cdrtools`.
- Unduhan sudah ada tetapi konversi gagal? Jalankan ulang konverter saja
  (`convert.sh ...`); tidak perlu mengunduh ulang.

Verifikasi ISO:

```bash
ls -lh *.ISO
sha256sum *.ISO | tee ~/uup/SHA256SUMS
```

---

## 4. Ekstrak berkas boot untuk S10e

```bash
ISO=$(ls ~/uup/*.ISO | head -1)
sudo mkdir -p /mnt
sudo mount -o loop "$ISO" /mnt

mkdir -p ~/winboot/{efi,boot,sources}
cp -r /mnt/efi  ~/winboot/
cp -r /mnt/boot ~/winboot/

# (a) hanya WinPE (index 1) -> ~300 MB, tanpa Windows Setup
wimlib-imagex info /mnt/sources/boot.wim          # lihat daftar index
rm -f ~/winboot/sources/boot.wim
wimlib-imagex export /mnt/sources/boot.wim 1 \
    ~/winboot/sources/boot.wim 'Microsoft Windows PE' --compress=LZX || \
cp /mnt/sources/boot.wim ~/winboot/sources/boot.wim   # (b) boot.wim utuh (~450 MB)

# boot.sdi (letaknya bisa di salah satu ini)
cp /mnt/efi/microsoft/boot/boot.sdi ~/winboot/efi/microsoft/boot/ 2>/dev/null \
 || cp /mnt/boot/boot.sdi ~/winboot/boot/

sudo umount /mnt
du -sh ~/winboot
sudo chown -R "$USER" ~/winboot
```

Dua mode:

| Mode | Cara | Ukuran `~/winboot` | Kapan dipakai |
|---|---|---|---|
| **WinPE-only** | `wimlib-imagex export ... 1 ...` | ~0,3 GB | uji boot / shell WinPE |
| **Lengkap (WinPE+Setup)** | salin `boot.wim` utuh | ~0,5 GB | mau menjalankan `setup.exe` (M5c) |

Untuk M5c, `install.esd`/`install.wim` dari ISO **tidak** ikut ke `~/winboot`;
ia diletakkan terpisah (lihat §6).

---

## 5. Ke perangkat: layout ESP untuk M5b

Tujuannya meniru USB installer, jadi taruh tiga hal ini di **root** satu
volume FAT:

```
/                      <- root volume ESP (FAT)
├── efi/               (dari §4)
├── boot/              (dari §4)
└── sources/
    └── boot.wim       (WinPE-only atau utuh)
```

Ukuran total ~0,3–0,5 GB.

**Ke mana?** Partisi CACHE hanya 600 MB dan FAT32 dibatasi 4 GB/file:

- Kalau `~/winboot` ≤ ~550 MB → boleh ke **CACHE** (cara §6.13 `PORT_S10E.md`).
- Lebih aman ke partisi **SYSTEM (5,57 GB)** — di situlah media Windows
  yang sebenarnya (boot.wim 23H2 ~742 MB saja sudah tidak muat di CACHE).

Setelah itu, di BDS pilih volume yang memuat `\efi\microsoft\boot\bootmgfw.efi`
(biasanya tercantum sebagai entri FAT), atau daftarkan boot option yang
menunjuk ke sana.

---

## 6. Untuk M5c (instal penuh)

- `install.esd`/`install.wim` berukuran **4–6 GB**, **tidak muat di CACHE
  (600 MB)** dan **melebihi batas 4 GB/file FAT32**.
- Letakkan di partisi **SYSTEM (5,57 GB)** sebagai `sources/install.esd`
  (ESD lebih kecil dari WIM; dipilih lewat `./convert.sh esd`).
- `boot.wim` tetap di ESP/volume kecil; `setup.exe` mencari
  `\sources\install.*` di volume yang sama atau yang ditunjuk BCD.
- Karena port ini belum punya driver Exynos (storage/GPU/USB) untuk Windows,
  jangan berharap desktop; target realistis M5c adalah **WinPE/Setup berjalan**
  dan melihat sejauh mana boot maju di konsol serial.

---

## 7. Troubleshooting

| Gejala | Sebab / solusi |
|---|---|
| `aria2c: command not found` | `sudo pacman -S aria2` |
| Konversi gagal di tahap akhir (`mkisofs`) | `sudo pacman -S cdrtools` (atau `cdrkit`) |
| Unduhan jauh > 5 GB | terlalu banyak edisi tercentang di langkah §2.5 |
| ISO jalan tapi di perangkat "no OS" | arsitektur salah (x64, bukan arm64) atau `bootmgfw.efi`/BCD tidak lengkap |
| File > 4 GB gagal ditulis ke FAT | pakai `install.esd` (LZMS) atau pecah jadi `install.swm` |
| Ingin ulang tanpa unduh lagi | `cd ~/uup && bash convert.sh esd` |

---

## 8. Referensi

- UUP dump: <https://uupdump.net> — konverter Linux resmi:
  <https://git.uupdump.net/uup-dump/converter>
- WinPE dari ISO di Linux (Arch Wiki):
  <https://wiki.archlinux.org/title/Windows_PE>
- Ukuran/opsi ISO resmi arm64 (26H2 ~7,9 GB):
  <https://www.microsoft.com/en-us/software-download/windows11arm64>
- Layout media & WinPE (Windows on R):
  <https://worproject.com/guides/getting-windows-images>
- Konteks perangkat & jalur boot: `PORT_S10E.md` (§6.12 M5a, §6.13 tulis ESP),
  `LINUX_ACPI_PLAN.md`.
