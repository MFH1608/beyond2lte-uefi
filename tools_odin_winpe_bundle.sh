#!/usr/bin/env bash
#
# UFS WinPE turu icin BOOT + SYSTEM + CACHE temizleme imajlarini tek Odin AP
# arsivinde birlestirir.
#
# NEDEN: Bu turda iki farkli katman degisiyor:
#   boot.img   -> UEFI + etkin UFS ACPI tanimi (ham firmware imaji)
#   system.img -> WinPE boot.wim + girissiz PnP/UFS raporu (Android sparse)
#   cache.img  -> eski startup.nsh olmadan bos FAT32 CACHE dosya sistemi
#                (Android sparse)
#
# Odin, FAT imajlarini ham halde reddedebiliyor. Bu betik SYSTEM ve CACHE
# kaynak imajlarini bizzat img2simg ile sparse'e cevirir; sparse basligini ve
# son tar'dan cikarilan baytlarin hash'ini dogrular. BOOT kasitli olarak ham
# kalir: daha once calisan uefi-boot.tar da boot.img'yi bu bicimde tasiyordu.
#
# Kullanim (yap.sh, tools_b2lfb.sh ve iki cache_esp.sh cagrisindan sonra):
#   bash cache_esp.sh --win --system
#   bash cache_esp.sh --win --bos
#   bash tools_odin_winpe_bundle.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/out"

DEV="${B2L_DEVICE:-beyond2lte}"
POS=()
while [ $# -gt 0 ]; do
  case "$1" in
    # Cihaz yalnizca cikti adini ve masaustu klasorunu etkiler; tar icindeki
    # boot.img/system.img/cache.img adlari Odin hedefi oldugu icin sabittir.
    -d|--device) DEV="${2:?-d icin cihaz adi gerekli}"; shift ;;
    -h|--help)   sed -n '2,22p' "$0"; exit 0 ;;
    *)           POS+=("$1") ;;
  esac
  shift
done

BOOT_TAR="${POS[0]:-$OUT/uefi-boot.tar}"
SYSTEM_RAW="${POS[1]:-$OUT/system.img}"
CACHE_RAW="${POS[2]:-$OUT/cache.img}"
BUNDLE="$OUT/${DEV}-winpe-ufs.tar"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
fail() { echo "HATA: $*" >&2; exit 1; }

[ -s "$BOOT_TAR" ] || fail "BOOT arsivi yok ya da bos: $BOOT_TAR"
for F in "$SYSTEM_RAW" "$CACHE_RAW"; do
  [ -s "$F" ] || fail "ham ESP imaji yok ya da bos: $F"
done

command -v tar >/dev/null || fail "tar yok"
command -v img2simg >/dev/null || fail "img2simg yok -> sudo apt install -y android-sdk-libsparse-utils"

say "Girdiler"
printf '  BOOT arsivi: %s\n' "$BOOT_TAR"
printf '  SYSTEM ham:   %s\n' "$SYSTEM_RAW"
printf '  CACHE ham:    %s\n' "$CACHE_RAW"

WORK="$(mktemp -d)"
CHECK="$WORK/check"
trap 'rm -rf "$WORK"' EXIT

only_entry() {
  local ARCH="$1" EXPECT="$2" ACTUAL
  ACTUAL="$(tar -tf "$ARCH" | sed '/^$/d')"
  [ "$ACTUAL" = "$EXPECT" ] || fail "$ARCH icinde yalniz '$EXPECT' bekleniyordu; bulunan: ${ACTUAL:-<bos>}"
}

sparse_magic() {
  od -An -tx4 -N4 "$1" | tr -d '[:space:]'
}

require_sparse() {
  local IMAGE="$1" MAGIC
  MAGIC="$(sparse_magic "$IMAGE")"
  [ "$MAGIC" = "ed26ff3a" ] || fail "sparse basligi gecersiz: $IMAGE (0x${MAGIC:-<bos>})"
  printf '  %s sparse sihirli sayisi: 0x%s\n' "$(basename "$IMAGE")" "$MAGIC"
}

only_entry "$BOOT_TAR" boot.img
tar -xf "$BOOT_TAR" -C "$WORK"
[ -s "$WORK/boot.img" ] || fail "BOOT arsivinden boot.img cikarilamadi"

say "SYSTEM ve CACHE Android sparse'e cevriliyor"
img2simg "$SYSTEM_RAW" "$WORK/system.img"
img2simg "$CACHE_RAW" "$WORK/cache.img"
require_sparse "$WORK/system.img"
require_sparse "$WORK/cache.img"

say "Tek AP arsivi uretiliyor"
rm -f "$BUNDLE"
(
  cd "$WORK"
  tar -H ustar -cf "$BUNDLE" boot.img system.img cache.img
)

say "Dogrulama"
EXPECTED=$'boot.img\nsystem.img\ncache.img'
ACTUAL="$(tar -tf "$BUNDLE" | sed '/^$/d')"
[ "$ACTUAL" = "$EXPECTED" ] || fail "cikti arsiv hedef listesi beklenenden farkli: $ACTUAL"
mkdir "$CHECK"
tar -xf "$BUNDLE" -C "$CHECK"
for F in boot.img system.img cache.img; do
  BEFORE="$(sha256sum "$WORK/$F" | awk '{print $1}')"
  AFTER="$(sha256sum "$CHECK/$F" | awk '{print $1}')"
  [ "$BEFORE" = "$AFTER" ] || fail "tar icinde $F degismis"
  printf '  %-10s hash esit: %s\n' "$F" "$AFTER"
done
require_sparse "$CHECK/system.img"
require_sparse "$CHECK/cache.img"
tar -tvf "$BUNDLE"
sha256sum "$BUNDLE"

for DESK in /mnt/c/Users/*/OneDrive/Masa*st* \
            /mnt/c/Users/*/OneDrive/Desktop \
            /mnt/c/Users/*/Masa*st* \
            /mnt/c/Users/*/Desktop; do
  [ -d "$DESK" ] || continue
  mkdir -p "$DESK/${DEV}-flash"
  cp -f "$BUNDLE" "$DESK/${DEV}-flash/"
  echo
  echo "Masaustune kopyalandi: $DESK/${DEV}-flash/$(basename "$BUNDLE")"
  break
done

cat <<SON

Hazir: out/${DEV}-winpe-ufs.tar
  Odin -> AP: bu TEK dosya
  Re-Partition: KAPALI

Arsiv yalnizca boot.img, system.img ve cache.img icerir.
SYSTEM ve CACHE'nin Android sparse oldugu bu betik tarafindan dogrulandi.
SON
