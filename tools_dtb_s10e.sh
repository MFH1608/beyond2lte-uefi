#!/usr/bin/env bash
#
# beyond0lte (Galaxy S10e, SM-G970F) GERCEK DTB'sini uretir ve overlay'e koyar.
#
# NEDEN GEREKLI
# ---------------------------------------------------------------------------
# build_uefi.py, boot imajina Resources/DTBs/<cihaz>.dtb ekler (append_dtb =
# true) ve dosya yoksa derlemeyi durdurur. Depodaki beyond0lte.dtb SU AN
# beyond2lte'den turetilmis bir YER TUTUCUDUR; icindeki reserved-memory ve
# board revizyon bilgileri S10e'ye ait degildir.
#
# Ayrica tools_reserved_memory_audit.py bu DTB'yi okur: S10e'nin gercek
# ayrilmis bellek bolgelerini bilmeden Windows'un "dokunmamasi gereken RAM"
# listesi eksik kalir. Bu, projede daha once SYSTEM_THREAD_EXCEPTION_
# NOT_HANDLED (0x7E) ile sonuclanan hatanin ta kendisidir.
#
# GIRDI SECENEKLERI
# ---------------------------------------------------------------------------
#   1) Calisan cihazdan dokum (EN KOLAY, TAVSIYE EDILEN):
#        bash tools_dtb_s10e.sh --fdt s10e-fdt.dtb
#      Dosyayi cihazda uret:
#        (stock root)   adb shell su -c 'cat /sys/firmware/fdt' > s10e-fdt.dtb
#        (LineageOS)    adb shell su -c 'cat /sys/firmware/fdt' > s10e-fdt.dtb
#      Bu, S-Boot'un cekirdege verdigi BIRLESIK DTB'dir: base + overlay.
#
#   2) Base + overlay ayri ayri:
#        bash tools_dtb_s10e.sh --base dt.img-cikti.dtb --overlay dtbo-rev26.dtb
#      Iki dosya fdtoverlay ile birlestirilir. Base, Samsung'un dt.img'inden
#      cikarilir; overlay, dtbo.img icindeki board girdisidir. Ikisini de
#      tools_dtbo_extract.py cikarir (asagidaki 3. yol).
#
#   3) Stock AP tar'indan OTOMATIK (mkdtimg GEREKMEZ):
#        bash tools_dtb_s10e.sh --tar AP_G970F....tar.md5
#      tools_dtbo_extract.py dt.img'den base'i, dtbo.img'den model/compatible
#      alaninda 'beyond0lte' gecen girdiyi secer ve fdtoverlay ile birlestirir.
#      Girdileri once gormek icin:
#        python3 tools_dtbo_extract.py --dtbo dtbo.img --list
#      Yanlis girdi secilirse: --pick <anahtar> ya da
#        python3 tools_dtbo_extract.py --dtbo dtbo.img --index N --out overlay.dtb
#
# Cikti (depoya yazilir):
#   mu-silicium-overlay/Resources/DTBs/beyond0lte.dtb
#   mu-silicium-overlay/Resources/DTBs/beyond0lte.dts   (insan okunur kopya)
#
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DTBDIR="$PROJ/mu-silicium-overlay/Resources/DTBs"
OUT_DTB="$DTBDIR/beyond0lte.dtb"
OUT_DTS="$DTBDIR/beyond0lte.dts"
MODEL="Samsung BEYOND0LTE EUR OPEN 26 board based on EXYNOS9820"

FDT=""; BASE=""; OVERLAY=""; TAR=""; DTB_PICK="beyond0lte"
while [ $# -gt 0 ]; do
  case "$1" in
    --fdt)     FDT="$2"; shift 2 ;;
    --base)    BASE="$2"; shift 2 ;;
    --overlay) OVERLAY="$2"; shift 2 ;;
    --tar)     TAR="$2"; shift 2 ;;
    --pick)    DTB_PICK="$2"; shift 2 ;;
    -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
    *) echo "Bilinmeyen secenek: $1"; exit 1 ;;
  esac
done

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { echo "HATA: $*" >&2; exit 1; }

command -v dtc >/dev/null || die "dtc yok -> sudo apt install -y device-tree-compiler"
command -v fdtoverlay >/dev/null || die "fdtoverlay yok -> sudo apt install -y device-tree-compiler"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

#
# 1) Ham (birlesik) DTB'yi elde et
#
MERGED="$WORK/merged.dtb"

if [ -n "$FDT" ]; then
  [ -f "$FDT" ] || die "dosya yok: $FDT"
  cp -f "$FDT" "$MERGED"
elif [ -n "$BASE" ] && [ -n "$OVERLAY" ]; then
  [ -f "$BASE" ]    || die "base yok: $BASE"
  [ -f "$OVERLAY" ] || die "overlay yok: $OVERLAY"
  say "fdtoverlay: base + overlay"
  fdtoverlay -i "$BASE" -o "$MERGED" "$OVERLAY" \
    || die "fdtoverlay basarisiz (base/overlay uyumsuz olabilir)"
elif [ -n "$TAR" ]; then
  [ -f "$TAR" ] || die "tar yok: $TAR"
  [ -f "$PROJ/tools_dtbo_extract.py" ] || die "tools_dtbo_extract.py yok"
  say "tar'dan base (dt.img) + S10e overlay (dtbo.img) seciliyor"
  python3 "$PROJ/tools_dtbo_extract.py" --tar "$TAR" --dir "$WORK/tar" \
    --pick "${DTB_PICK:-beyond0lte}" --quiet \
    || die "DTB secilemedi (girdi tablosunu gormek icin: \
python3 tools_dtbo_extract.py --dtbo <dtbo.img> --list)"
  [ -s "$WORK/tar/base.dtb" ]    || die "base.dtb uretilmedi (tar'da dt.img yok olabilir)"
  [ -s "$WORK/tar/overlay.dtb" ] || die "overlay.dtb uretilmedi (tar'da dtbo.img yok olabilir)"
  say "fdtoverlay: base + overlay"
  fdtoverlay -i "$WORK/tar/base.dtb" -o "$MERGED" "$WORK/tar/overlay.dtb" \
    || die "fdtoverlay basarisiz (base/overlay uyumsuz olabilir)"
else
  die "--fdt veya --base+--overlay veya --tar vermelisin (--help)"
fi

say "DTB cozuluyor"
dtc -I dtb -O dts -o "$WORK/raw.dts" "$MERGED" 2>/dev/null \
  || die "dtc cozemedi: girdi gecerli bir DTB mi?"

#
# 2) model satirini S10e'ye sabitle (yoksa compatible'in altina ekle)
#
if grep -q '^	model = ' "$WORK/raw.dts"; then
  sed -i "s|^	model = .*|	model = \"$MODEL\";|" "$WORK/raw.dts"
else
  sed -i "0,/^	compatible = /s||	model = \"$MODEL\";\n	compatible = |" "$WORK/raw.dts"
fi

say "DTB yeniden derleniyor"
dtc -I dts -O dtb -o "$WORK/out.dtb" "$WORK/raw.dts" 2>/dev/null \
  || die "dtc derleyemedi"

cp -f "$WORK/out.dtb" "$OUT_DTB"; chmod 0644 "$OUT_DTB"
cp -f "$WORK/raw.dts" "$OUT_DTS"; chmod 0644 "$OUT_DTS"
echo "  yazildi: $OUT_DTB"
echo "  yazildi: $OUT_DTS"

#
# 3) Dogrulama: model, bellek banklari, reserved-memory
#
say "Dogrulama"
dtc -I dtb -O dts "$OUT_DTB" 2>/dev/null > "$WORK/verify.dts"
grep -m1 'model = ' "$WORK/verify.dts" | sed 's/^[[:space:]]*/  /' || true

python3 - "$OUT_DTB" <<'PY'
import re, subprocess, sys
dts = subprocess.run(["dtc","-I","dtb","-O","dts",sys.argv[1]],
                     capture_output=True, text=True).stdout

#
# /memory dugumleri: #address-cells=2, #size-cells=1 -> reg = <hi lo size>
# ONEMLI: bu dugumler RAM boyutunun KAYNAGI DEGILDIR. S-Boot, cekirdege
# devrederken memory dugumlerini kendi tespitine gore YAMALAR; ayni base
# DTB hem 6 hem 8 GiB varyantta kullanilir. UEFI'nin gordugu RAM boyutu
# NS_IRAM+0x848'den gelir (RamManagerLib). Asagidaki sayi yalnizca DTB'nin
# o an ne dedigini gosterir.
#
total = 0
banks = []
for m in re.finditer(r"memory@[0-9a-fA-F]+\s*\{[^}]*?reg = <([^>]+)>;", dts, re.S):
    cells = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]
    if len(cells) >= 3:
        base = (cells[0] << 32) | cells[1]
        size = cells[2] if len(cells) == 3 else (cells[2] << 32) | cells[3]
        banks.append((base, size))
        total += size
print(f"  DTB memory banklari: {len(banks)}  (toplam {total/2**30:.2f} GiB)")
for base, size in banks:
    print(f"    0x{base:011X}  {size/2**30:.2f} GiB")
print("  NOT: gercek RAM boyutu S-Boot'tan gelir; DTB yamalanir.")

for key in ("reserved-memory", "sec_rdx_bootdev", "seclog", "modem_spare"):
    print(f"  {key:18} {'VAR' if key in dts else 'YOK'}")
PY

cat <<EOF

SIRADAKI ADIM
  1) Ayrilmis bellek denetimini yeni DTB ile calistir:
       python3 tools_reserved_memory_audit.py --device beyond0lte --check
     "ACIK" cikan her sabit bolge, MemoryMapLib.c'ye EKLENMELI (Reserv).
  2) Derle:
       bash yap.sh -d beyond0lte --clean
EOF
