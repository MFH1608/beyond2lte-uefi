# Build native di Linux (tanpa `sudo`, tanpa WSL)

`BUILD.md` menjelaskan alur "WSL untuk build, Windows untuk flash". Dokumen ini
menjelaskan alternatif yang **sudah terbukti**: build langsung di Linux native
(Arch/CachyOS) **tanpa hak root sama sekali**, memakai paket sistem yang sudah
ada ditambah beberapa paket yang diekstrak ke `~/.local`.

Semua perintah di bawah dijalankan sebagai user biasa. Tidak ada `sudo`.

---

## 1. Ringkasan yang dibutuhkan

| Kebutuhan | Di sini dipakai |
|---|---|
| Klon Mu-Silicium (rekursif, ~5,6 GB) | `~/Mu-Silicium` |
| Python + venv | Python 3.14.9, venv `~/Mu-Silicium/.venv` |
| Kompilator AArch64 | `clang` sistem (23.1.4) |
| Linker PE/COFF AArch64 | `lld-link` (rootless) |
| `llvm-lib`, `llvm-rc`, `llvm-objcopy` | paket `llvm` sistem |
| `nasm`, `iasl`, `cmake`, `ninja` | rootless di `~/.local/mu-sysroot` |

Toolchain EDK2 yang dipakai proyek ini hanya `CLANGPDB` + target
`aarch64-unknown-windows-msvc`. `nasm` **tidak** dipakai untuk AArch64 (hanya
untuk beberapa modul x86), `iasl` dipakai untuk ACPI.

---

## 2. Mu-Silicium dan dependensi Python

```bash
cd ~
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install -r pip-requirements.txt
```

`--recursive` wajib. `stuart_*` yang dipakai `build_uefi.py` masuk lewat
`pip-requirements.txt` (edk2-pytool-extensions), jadi tidak perlu binari
`stuart` terpisah.

---

## 3. Toolchain rootless (`~/.local/mu-sysroot`)

`setup_env.sh -p apt` mengasumsikan Debian/Ubuntu dan **butuh root**. Di sini
kita tidak memilikinya. Sebagai gantinya, paket diambil sebagai URL lalu
diekstrak ke dalam `$HOME`:

```bash
# pacman -Sp mencetak URL tanpa perlu root
pacman -Sp lld nasm acpica cmake ninja | sort -u > /tmp/pkg-urls.txt

mkdir -p ~/.local/mu-sysroot
cd /tmp && while read -r u; do curl -fsSLO "$u"; done < /tmp/pkg-urls.txt
for f in *.pkg.tar.zst; do bsdtar -xf "$f" -C ~/.local/mu-sysroot; done
```

`bsdtar` memahami `.zst`. Setelah ini aktifkan lewat `env.sh`:

```bash
cat > ~/.local/mu-sysroot/env.sh <<'EOF'
MU_SYSROOT="${MU_SYSROOT:-$HOME/.local/mu-sysroot}"
export PATH="$MU_SYSROOT/usr/bin:$PATH"
export LD_LIBRARY_PATH="$MU_SYSROOT/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export IASL_PATH="$MU_SYSROOT/usr/bin/iasl"
EOF
```

`LD_LIBRARY_PATH` penting: `lld`/`lld-link` di paket ini menautkan ke
`liblld*.so` di `$MU_SYSROOT/usr/lib`, bukan di lokasi sistem.

Verifikasi cepat:

```bash
source ~/.local/mu-sysroot/env.sh
lld-link --version   # LLD 23.1.4
iasl -v | head -1    # ... version 20251212
```

### 3.1 Stub `mu_nasm` (NuGet tanpa Mono)

Langkah `stuart update` mengambil dependensi NuGet `mu_nasm`. Pengambil NuGet
di Linux memerlukan **Mono**, yang tidak bisa dipasang tanpa root. Karena
`nasm` tidak dipakai untuk build AArch64, kita cukup memuaskan pemeriksaan
keberadaannya:

```bash
D=~/Mu-Silicium/Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p "$D/Linux-x86-64"
cp -f ~/.local/mu-sysroot/usr/bin/nasm "$D/Linux-x86-64/nasm"
printf 'version: 20016.1.1\n' > "$D/extdep_state.yaml"
```

Ini hanya memenuhi `verify()` (yang memeriksa berkas state), bukan menyediakan
NuGet sungguhan. Untuk `CLANGPDB` AArch64 hasilnya identik.

---

## 4. Build

`yap.sh` menyalin `mu-silicium-overlay/` ke Mu-Silicium, jadi tidak perlu
`cp` manual:

```bash
cd <repo>
source ~/.local/mu-sysroot/env.sh
bash yap.sh -d beyond0lte --acpi --clean
```

Keluaran di `~/Mu-Silicium`: `Mu-beyond0lte.img`, `uefi-boot.tar`; juga
disalin ke `out/` (dan ke `${DEV}-flash/` bila Desktop ditemukan).

### Isi `Mu-beyond0lte.img`

```
Android bootimg page=2048
  kernel  = BootShim (112 B) + UEFI FD (tepat FD_SIZE) + DTB S10e (ek)
  ramdisk = "dummy"
```

BootShim hanya menyalin `FD_SIZE` byte pertama ke `FD_BASE`; DTB yang
menempel **setelah** FD tidak dipakai saat runtime (hanya ikut dalam boot
image). Untuk S10e: `FD_BASE = 0xC9000000`, `FD_SIZE = 4 MiB`,
`FD = 0xC9000000..0xC9400000`, stack `0xC9400000..0xC9440000`
(`tools_uefi_layout_check.py --device beyond0lte` memverifikasi ini).

---

## 5. Catatan kompiler (clang 23)

Alur ini memakai clang yang lebih baru daripada yang diasumsikan proyek
(Ubuntu/apt). Dua peringatan baru menjadi **error** karena `-Werror` dan
menghentikan build. Keduanya kode mati, bukan bug:

- `UfsCal.c`: `ufs_cal_lock_timeout` disetel tetapi tidak pernah dibaca.
- `B2LGoster.c`: penghitung `Parca` (baris `PARCA` sudah dihapus lebih dulu).

Perbaikan ada di commit `fix(build): clang 23 -Werror ...`. Bila memakai clang
lama, commit itu tetap tidak mengubah perilaku.

Selain itu, `S5E9820Pkg.dsc.inc` di overlay merujuk `GpioLib`/`SpeedyLib` yang
**sudah tidak ada** di Mu-Silicium saat ini, dan menghapus
`PcdGpioCtrlAddr`/`PcdSpeedyBusAddr` yang dibaca `GpioDxe`/`SpeedyDxe`.
Berkas itu sudah diselaraskan ke `GpioBankLib` sambil tetap mempertahankan
tambahan overlay (`PlatformUfsLib`, `HsI2cLib`).

---

## 6. Yang belum terbukti

Build **berhasil**, tetapi belum ada satu pun yang dijalankan di perangkat.
Lihat "Yang paling rapuh" di [`PORT_S10E.md`](PORT_S10E.md) — terutama
region `"Display Reserved"` (`0xCA000000`, 20 MiB) yang masih asumsi.
