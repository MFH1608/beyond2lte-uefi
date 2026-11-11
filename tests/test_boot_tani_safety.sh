#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG_ROOT="$ROOT/mu-silicium-overlay/Platforms/Samsung"
NSH="$ROOT/esp/startup-boot-tani.nsh"

# Cihaz argumani verilmezse B2LBootTani iceren tum paketler denetlenir; boylece
# yeni bir port (or. beyond0lte) sozlesmeden sessizce kacmaz.
if [ $# -gt 0 ]; then
  APPS=("$PKG_ROOT/$1Pkg/Applications/B2LBootTani/B2LBootTani.c")
else
  APPS=()
  for f in "$PKG_ROOT"/*Pkg/Applications/B2LBootTani/B2LBootTani.c; do
    [ -f "$f" ] && APPS+=("$f")
  done
fi
[ ${#APPS[@]} -gt 0 ] || { echo "HATA: B2LBootTani.c bulunamadi"; exit 1; }

forbidden='(Mmio(Read|Write)|CF0[0-9A-Fa-f]{5}|GICD|PMU_RST|TARA_KAYIT|SEH_(CANLI|ONCEKI))'
for APP in "${APPS[@]}"; do
  echo "  denetleniyor: ${APP#"$ROOT"/}"
  if grep -En "$forbidden" "$APP"; then
    echo "HATA: boot tani yolunda sabit fiziksel adres veya eski tani erisimi var."
    exit 1
  fi
  grep -Fq 'FileDevicePath (Kendi->DeviceHandle, BOOT_YOLU)' "$APP"
  grep -Fq 'gEfiEventExitBootServicesGuid' "$APP"
  grep -Fq 'TimerPeriodic' "$APP"
  grep -Fq 'KORUMALI BELLEK: ERISIM YOK' "$APP"
done
if grep -En '^[[:space:]]*(dmem|[^#]*B2LGOSTR)' "$NSH"; then
  echo "HATA: boot tani startup yolu eski fiziksel taniyi calistiriyor."
  exit 1
fi

grep -Fq -- '--no-touch) NO_TOUCH=1' "$ROOT/yap.sh"
grep -Fq 'B2LTouchDxe: KONTROL DERLEMESINDE KAPALI' "$ROOT/yap.sh"
grep -Fq 'uefi-boot-no-touch.tar' "$ROOT/yap.sh"

bash -n "$ROOT/cache_esp.sh" "$ROOT/yap.sh" "$ROOT/tools_windisk.sh" \
        "$ROOT/tools_odin_winpe_bundle.sh" "$ROOT/tools_b2lfb.sh" \
        "$ROOT/tools_b2lufs.sh" "$ROOT/tools_dtb_s10e.sh"
echo "B2LBootTani guvenlik sozlesmesi: TAMAM"
