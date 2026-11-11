#!/usr/bin/env bash
#
# Bir cihaz icin Odin flash paketi hazirlar.
#
# yap.sh'nin urettigi iki dosyadan okur:
#   out/uefi-boot.tar   -> Odin AP (icinde yalnizca boot.img)
#   out/Mu-<dev>.img    -> ayni boot.img, heimdall icin
#
# ve yanlarina kisa bir talimat + SHA256SUMS koyar.
#
# Kullanim:
#   bash tools_flash_bundle.sh -d beyond0lte
#   bash tools_flash_bundle.sh -d beyond2lte
#
# Cikti:
#   out/<dev>-flash/        (klasor)
#   out/<dev>-flash.zip     (ayni klasorun zip'i)
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/out"
DEV="${B2L_DEVICE:-beyond2lte}"

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--device) DEV="${2:?-d icin cihaz adi gerekli}"; shift ;;
    -h|--help)   sed -n '2,18p' "$0"; exit 0 ;;
    *)           echo "Bilinmeyen arguman: $1" >&2; exit 1 ;;
  esac
  shift
done

TAR="$OUT/uefi-boot.tar"
IMG="$OUT/Mu-${DEV}.img"
BUNDLE="$OUT/${DEV}-flash"
ZIP="$OUT/${DEV}-flash.zip"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { echo "HATA: $*" >&2; exit 1; }

[ -s "$TAR" ] || fail "yok: $TAR  (once: bash yap.sh -d $DEV --acpi --clean)"
[ -s "$IMG" ] || fail "yok: $IMG"

# Odin AP arsivi yalnizca boot.img tasimali.
ENTRIES="$(tar -tf "$TAR" | sed '/^$/d')"
[ "$ENTRIES" = "boot.img" ] \
  || fail "$TAR icinde yalnizca 'boot.img' olmali; bulunan: ${ENTRIES:-<bos>}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
tar -xf "$TAR" -C "$WORK"
[ -s "$WORK/boot.img" ] || fail "boot.img arsivden cikarilamadi"
cmp -s "$WORK/boot.img" "$IMG" \
  || fail "arsivdeki boot.img ile $IMG ayni degil (yanlis cihaz icin derlenmis olabilir)"

say "Paket hazirlaniyor: $BUNDLE"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp -f "$TAR" "$BUNDLE/uefi-boot.tar"
cp -f "$IMG" "$BUNDLE/Mu-${DEV}.img"

#
# Cihaza ozel uyarilar. Yalnizca S10e icin ek metin var; digerleri genel.
#
DEV_NOTE=""
if [ "$DEV" = "beyond0lte" ]; then
  DEV_NOTE="$(cat <<'EOF'
CATATAN KHUSUS S10e (beyond0lte)
- Revisi board fisik tidak diketahui; DTB memakai revisi tertinggi
  (EUR OPEN 25). Bila tombol/menu berperilaku aneh, lihat PORT_S10E.md.
- Satu-satunya asumsi yang tersisa: region "Display Reserved"
  (0xCA000000, 20 MiB) warisan S10+. Bila layar tetap gelap padahal
  perangkat hidup, ini tersangka pertama.
- PIT: BEYOND2LTE_EUR_OPEN.pit di repo MILIK S10+ (offset berbeda).
  JANGAN dipakai dengan Re-Partition.
EOF
)"
fi

cat > "$BUNDLE/BACA-INI.txt" <<EOF
BUNDEL FLASH UEFI — ${DEV}
============================================================

Isi bundel
----------
  uefi-boot.tar    -> Odin, slot AP (Re-Partition JANGAN dicentang)
  Mu-${DEV}.img    -> isi boot.img yang sama, untuk heimdall (Linux)
  SHA256SUMS       -> verifikasi:  sha256sum -c SHA256SUMS

PENTING — baca sebelum flashing
-------------------------------
- Ini build pertama dan BELUM pernah dijalankan di perangkat.
- UEFI ditulis ke partisi BOOT, BUKAN ke sboot/bootloader.
  Bootloader tetap utuh.
- JANGAN mencentang "Re-Partition".
- JANGAN memakai PIT S10+ (BEYOND2LTE_EUR_OPEN.pit): offset/ukuran
  partisinya berbeda dari S10e.

LANGKAH ODIN (Windows)
----------------------
1. Matikan perangkat.
2. Masuk Download Mode: tekan dan tahan Bixby + Volume Down, lalu
   colok kabel USB. (Bixby = tombol di sisi kiri, di bawah volume.)
3. Buka Odin. Pada slot AP pilih:  uefi-boot.tar
4. Options:
      Auto Reboot       : boleh dicentang
      F.Reset Time      : boleh dicentang
      Re-Partition      : JANGAN dicentang
5. Start, tunggu sampai PASS / perangkat reboot.

LANGKAH HEIMDALL (Linux)
------------------------
  heimdall flash --BOOT Mu-${DEV}.img
(hentikan Odin/Windows saat memakai heimdall; jangan keduanya)

SETELAH BOOT
------------
Masuk ke EFI Shell / menu UEFI, lalu periksa:
  memmap    -> total sekitar 6 GiB dan TIDAK ada baris >= 0x980000000
  tombol Volume Up/Down harus menggerakkan pilihan menu
(Bila muncul shell, itu tanda UEFI sudah berjalan.)

ROLLBACK
--------
- Odin: flash AP stok  G970FXXUHHXJ1  (file AP_*.tar.md5) untuk kembali
  ke Android. Atau sideload LineageOS lewat recovery.
- Jangan centang Re-Partition saat rollback.

${DEV_NOTE}

Referensi: PORT_S10E.md (§4 flash, §7 rollback) dan BUILD_LINUX.md.
EOF

( cd "$BUNDLE" && sha256sum uefi-boot.tar "Mu-${DEV}.img" > SHA256SUMS )

say "Zip olusturuluyor: $ZIP"
rm -f "$ZIP"
( cd "$OUT" && zip -q -r "$(basename "$ZIP")" "$(basename "$BUNDLE")" )

say "Dogrulama"
( cd "$BUNDLE" && sha256sum -c SHA256SUMS )
unzip -l "$ZIP" | sed -n '1,20p'

# Masaustu varsa oraya da birak (WSL yollari; Linux'ta genelde yok).
for DESK in /mnt/c/Users/*/OneDrive/Masa*st* \
            /mnt/c/Users/*/OneDrive/Desktop \
            /mnt/c/Users/*/Masa*st* \
            /mnt/c/Users/*/Desktop; do
  [ -d "$DESK" ] || continue
  rm -rf "$DESK/${DEV}-flash"
  cp -r "$BUNDLE" "$DESK/${DEV}-flash"
  echo
  echo "Masaustune kopyalandi: $DESK/${DEV}-flash/"
  break
done

cat <<EOF

Hazir: out/${DEV}-flash/  ve  out/${DEV}-flash.zip

  Odin   -> AP: uefi-boot.tar   (Re-Partition KAPALI)
  Linux  -> heimdall flash --BOOT Mu-${DEV}.img
  Detay  -> out/${DEV}-flash/BACA-INI.txt
EOF