#!/usr/bin/env bash
#
# beyond2lte / beyond0lte UEFI — tek komutluk derleme.
#
# WSL'de calistir. Kendi konumunu bulur, hicbir yol elle yazilmaz:
#   bash /mnt/c/.../UEFI_beyond2lte/yap.sh -d beyond0lte
#
# Kullanim:
#   yap.sh                 DEBUG derle (varsayilan cihaz: beyond2lte)
#   yap.sh -d beyond0lte   S10e (SM-G970F, 6/128) icin derle
#   yap.sh -r RELEASE      RELEASE derle
#   yap.sh --acpi          ACPI tablolarini derle + etkinlestir
#   yap.sh --psci-hvc      deneysel: Windows PSCI'sini EL2 HVC koprusune yonlendir
#   yap.sh --no-touch      kontrol: B2LTouchDxe'yi firmware'den cikar
#   (Odin .tar ve .img her zaman uretilir)
#   yap.sh --clean         temiz derleme (-c)
#
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MU="${MU_SILICIUM:-$HOME/Mu-Silicium}"
REL="DEBUG"; CLEAN=""; DO_ACPI=0; DO_TAR=0; PSCI_HVC=0; NO_TOUCH=0
DEV="${B2L_DEVICE:-beyond2lte}"

while [ $# -gt 0 ]; do
  case "$1" in
    -r|--release) REL="$2"; shift 2 ;;
    -d|--device)  DEV="$2"; shift 2 ;;
    --acpi)  DO_ACPI=1; shift ;;
    --psci-hvc) PSCI_HVC=1; DO_ACPI=1; shift ;;
    --no-touch) NO_TOUCH=1; shift ;;
    --tar)   shift ;;   # geriye donuk uyumluluk, artik varsayilan
    --clean) CLEAN="-c"; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "Bilinmeyen secenek: $1"; exit 1 ;;
  esac
done

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

[ -d "$MU" ] || { echo "Mu-Silicium bulunamadi: $MU"; echo "MU_SILICIUM=... ile yol verebilirsin."; exit 1; }
[ -d "$PROJ/mu-silicium-overlay/Platforms/Samsung/${DEV}Pkg" ] || {
  echo "Cihaz paketi yok: mu-silicium-overlay/Platforms/Samsung/${DEV}Pkg"
  echo "Desteklenen: beyond2lte (S10+), beyond0lte (S10e)"
  exit 1
}

say "Proje: $PROJ"
say "Mu-Silicium: $MU"
say "Cihaz: $DEV"

# BootShim, configteki FD_SIZE kadar veriyi FD_BASE'e kopyalar. UEFI FD
# descriptoru veya CPU stack bu aralikla cakisirsa firmware kendi kodunu
# ezebilir; derlemeden (ve dolayisiyla flashtan) once bunu kesin denetle.
say "UEFI bellek yerlesimi dogrulaniyor"
python3 "$PROJ/tools_uefi_layout_check.py" --device "$DEV"
say "Windows korumali RAM denetleniyor"
python3 "$PROJ/tools_reserved_memory_audit.py" --device "$DEV" --check

# ---------------------------------------------------------------- ACPI
if [ "$DO_ACPI" = 1 ]; then
  say "ACPI tablolari derleniyor"
  command -v iasl >/dev/null || { echo "iasl yok -> sudo apt install -y acpica-tools"; exit 1; }
  # PPTT iasl ile degil Python ile uretiliyor (tools_pptt_gen.py'deki nedene bak).
  # iasl'den ONCE calismali ki SoCs kopyalanirken PPTT.aml yerinde olsun.
  python3 "$PROJ/tools_pptt_gen.py"
  cp -r "$PROJ/silicium-acpi-overlay/SoCs" "$MU/Silicium-ACPI/"
  ( cd "$MU/Silicium-ACPI/SoCs/Samsung/S5E9820" && for f in *.dsl; do iasl -ve "$f"; done )
  echo "ACPI tablolari derlendi."
fi

# ---------------------------------------------------------------- PSCI conduit
#
# Windows'a PSCI beyan eden FADT, hangi konduit kullanilacagini ARM Flags
# bit 1 ile secir. Varsayilan SMC'dir ve derlemeden sonra da zorla SMC'ye
# geri konur: bir onceki --psci-hvc deneyinin FACP.aml'i Mu-Silicium'da
# kalip sonraki "bash yap.sh" derlemesini sessizce degistiremez.
#
# --psci-hvc ayrica --acpi'yi zorunlu kilar; FACP.aml'in bu turda sifirdan
# derlendigi gorulmeden ikili tablo yamalanmaz.
#
FADT_AML="$MU/Silicium-ACPI/SoCs/Samsung/S5E9820/FACP.aml"
if [ "$PSCI_HVC" = 1 ]; then
  [ -f "$FADT_AML" ] || {
    echo "HATA: --psci-hvc icin FACP.aml derlenemedi: $FADT_AML"
    exit 1
  }
  say "PSCI HVC konduiti etkinlestiriliyor (deneysel)"
  python3 "$PROJ/tools_fadt_psci.py" "$FADT_AML" --hvc
elif [ -f "$FADT_AML" ]; then
  python3 "$PROJ/tools_fadt_psci.py" "$FADT_AML" --smc
fi

# ---------------------------------------------------------------- overlay
#
# ONCE UPSTREAM DOSYALARI GERI AL.
#
# tools_ufs_patch.py yamayi $MU icindeki UfsDxe.c'ye KALICI olarak
# uyguluyor. Scripti cagirmayi birakmak yamayi geri ALMIYOR - dosya orada
# yamali duruyor ve "B2L UFS:" satirlari cikmaya devam ediyor. Bilinen iyi
# noktaya donerken tam olarak bu oldu ve geri donusu eksik birakti.
#
# Overlay o dosyayi kopyalamadigi icin (sadece .inf var) tek yol upstream
# surumu git'ten geri cagirmak.
#
if [ -d "$MU/.git" ]; then
  if git -C "$MU" checkout -- Silicon/Samsung/SamsungPkg/Drivers/UfsDxe/UfsDxe.c 2>/dev/null; then
    echo "  UfsDxe.c upstream haline alindi (yama temizlendi)"
  fi
fi
#
# WatchdogTimer.c BURADA GERI CAGRILMIYOR.
#
# O dosya bir ALT MODULDE (Project Mu MdeModulePkg'i ayri depoda tutuyor),
# yani "git -C $MU checkout" ona ulasmiyor. Ilk surumde sabit yol yazmistim
# ve dosya hic bulunamadi.
#
# Bunun yerine tools_wdt_kapat.py dosyayi ARIYOR ve yamayi IDEMPOTENT
# uyguluyor: imza zaten varsa dokunmuyor. Birikmeli hasar olusmuyor,
# geri cagirmaya gerek kalmiyor.
#

say "Overlay kopyalaniyor"
cp -r "$PROJ/mu-silicium-overlay/." "$MU/"
echo "  Platforms/Samsung/${DEV}Pkg"
echo "  Silicon/Samsung/S5E9820Pkg"
echo "  Silicon/Samsung/SamsungPkg/Drivers/UfsDxe/UfsDxe.inf  (upstream yamasi)"
echo "  Resources/{Configs,DTBs}/${DEV}.*"

#
# UsbfnDwc3Dxe.c intr yamasi - CDC-ACM bildirim uc noktasi.
#
# bkz. tools_usbfn_intr_patch.py basi. Kurtarma: overlay UsbfnDwc3Dxe.c'yi
# kopyalamadigi icin yama dogrudan $MU dosyasina uygulanir; script idempotent
# (imza varsa dokunmaz), geri almak icin $MU icinde git checkout yeter.
#
python3 "$PROJ/tools_usbfn_intr_patch.py" "$MU"

# Tek-degiskenli kontrol: kaynak depoda surucu etkin kalir; yalnizca bu
# derleme icin Mu-Silicium'a kopyalanan DXE.inc satiri kapatilir. Sonraki
# normal yap.sh overlay'i yeniden kopyalayarak dokunmatigi geri acar.
if [ "$NO_TOUCH" = 1 ]; then
  DXE_INC="$MU/Platforms/Samsung/${DEV}Pkg/Include/DXE.inc"
  sed -i "s|^  INF .*Drivers/B2LTouchDxe/B2LTouchDxe\.inf\$|  # INF ${DEV}Pkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf|" "$DXE_INC"
  if grep -qx "  # INF ${DEV}Pkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf" "$DXE_INC" \
     && ! grep -qx "  INF ${DEV}Pkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf" "$DXE_INC"; then
    echo "  B2LTouchDxe: KONTROL DERLEMESINDE KAPALI"
  else
    echo "HATA: B2LTouchDxe DXE.inc icinde kapatilamadi"
    exit 1
  fi
fi

# ---------------------------------------------------------------- PSCI HVC bridge
#
# Varsayilan DXE.inc B2LEl2Dxe'yi yorumda birakir. HVC denemesinde bu tek
# satiri Mu-Silicium kopyasinda acariz; bir sonraki normal derlemede overlay
# yeniden kopyalandigi icin satir otomatik olarak tekrar kapali olur.
#
# Bu yol eski HCR_EL2.TSC SMC tuzagini kullanmaz. FADT HVC bitinin Windows'un
# PSCI cagrilarini HVC ile gondermesini sagladigi dar yolu kullanir.
#
if [ "$PSCI_HVC" = 1 ]; then
  DXE_INC="$MU/Platforms/Samsung/${DEV}Pkg/Include/DXE.inc"
  DSC="$MU/Platforms/Samsung/${DEV}Pkg/${DEV}.dsc"
  sed -i "s|^  # INF .*Drivers/B2LEl2Dxe/B2LEl2Dxe\.inf\$|  INF ${DEV}Pkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf|" "$DXE_INC"
  sed -i "s|^  # .*Drivers/B2LEl2Dxe/B2LEl2Dxe\.inf\$|  ${DEV}Pkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf|" "$DSC"
  if grep -qx "  INF ${DEV}Pkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf" "$DXE_INC" \
     && grep -qx "  ${DEV}Pkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf" "$DSC"; then
    echo "  B2LEl2Dxe: HVC koprusu DERLEME VE FV'DE"
  else
    echo "HATA: B2LEl2Dxe DSC/DXE.inc icinde etkinlestirilemedi"
    exit 1
  fi
fi

#
# UfsDxe.c yamasi - YAZMA YOLU.
#
# UEFI'den UFS'e yazma calismiyor (iki bagimsiz kanit: EFI Shell yonlendirmesi
# ve bootmgr'in bootstat.dat'i). Okuma sorunsuz. Yama hem onbellek bakimini
# ekliyor hem de yazma yolunu enstrümante ediyor.
#
# Dosyanin tamamini overlay'e kopyalamiyoruz (~1000 satir, upstream
# duzeltmelerini kaybederdik); sadece dokundugumuz satirlar yamada.
#
#
# UFS YAMASI GECICI OLARAK KAPALI - BILINEN IYI NOKTAYA DONUS
#
# tools_ufs_patch.py, upstream UfsDxe.c'ye onbellek bakimi (WriteBack/
# Invalidate) ve "B2L UFS:" enstrumantasyonu ekliyor. Ikisi de UEFI
# TARAFINI degistiriyor - yani Windows devraldiginda UFS denetleyicisinin
# BULUNDUGU DURUMU degistirebilir.
#
# Bu yama, ekranda animasyonlu sonmeyi gordugumuz noktadan (43ef06c^) SONRA
# eklendi. Bilinen iyi noktaya donerken bunu birakmak, "her seyi geri aldik"
# demeyi yanlis kilardi.
#
# Geri acmak icin asagidaki satirin yorumunu kaldir. Enstrumantasyon
# ciktilarina (WriteBlocks/OCS satirlari) tekrar ihtiyac olursa gerekli.
#
# python3 "$PROJ/tools_ufs_patch.py" "$MU"
echo "  UFS enstrumantasyon yamasi ATLANDI"

#
# PWM GEAR 1 YAMASI - VARSAYILAN OLARAK KAPALI
#
#   B2L_UFS_PWM=1 bash yap.sh   -> yamayi uygula
#
# NEDEN KONULMUSTU: storufs.sys jenerik; UEFI linki HS'e cikarirsa
# devralamiyor. PWM G1 zaten UniPro'nun LINKSTARTUP sonrasi varsayilan
# durumu, yani vendor kalibrasyonu gerektirmiyor.
#
# NEDEN KAPATILDI: o kapi YAPISAL olarak kapandi (bkz. STATUS.md,
# "storufs: kapanan kapi"). HCE reseti Exynos'ta UniPro'nun yerel
# niteliklerini siliyor ve hicbir ACPI nesnesi jenerik bir surucuye Samsung
# PHY kalibrasyonu yaptiramaz. Artik RAM diskten aciliyoruz ve storufs'a
# hic ihtiyacimiz yok.
#
# BEDELI COK AGIRDI: PWM G1 tek serit ~3 MB/s. Her acilista bootmgfw 742
# MB'lik boot.wim'i o linkten RAM diske okuyor - dakikalar. Ustelik hicbir
# karsiligi olmadan. HS Gear 3'e donmek gelistirme dongusunu elli kat
# hizlandiriyor ve kaybettigimiz hicbir sey yok.
#
#
# WATCHDOG ETKISIZLESTIRME
#
# Gerekce tools_wdt_kapat.py basinda. Kisaca: bes bagimsiz olcum
# "bootmgfw'nin kurdugu bir UEFI watchdog'u suresi dolunca cihazi
# sifirliyor" aciklamasina oturuyor.
#
# Kapatmak icin:  B2L_WDT=0 bash yap.sh
#
if [ "${B2L_WDT:-1}" = "1" ]; then
  python3 "$PROJ/tools_wdt_kapat.py" "$MU"
else
  echo "  Watchdog yamasi ATLANDI (B2L_WDT=0)"
fi

if [ "${B2L_UFS_PWM:-0}" = "1" ]; then
  python3 "$PROJ/tools_ufs_pwm.py" "$MU"
else
  echo "  UFS PWM G1 yamasi ATLANDI (HS Gear 3) - hizlandirma icin"
  echo "    geri acmak istersen:  B2L_UFS_PWM=1 bash yap.sh"
fi

#
# ACPI.inc'i ETKINLESTIR - ama $MU icindeki kopyada, depoda degil.
#
# Onceden bu sed depodaki dosyaya uygulaniyordu. Sonuc: her --acpi derlemesi
# takip edilen iki dosyayi degistiriyor ve sonraki "git pull" su hatayla
# duruyordu:
#   error: Your local changes to the following files would be overwritten by merge
#
# $MU bir git deposu degil, orada degistirmek serbest. Depo temiz kaliyor.
#
if [ "$DO_ACPI" = 1 ]; then
  INC="$MU/Platforms/Samsung/${DEV}Pkg/Include/ACPI.inc"
  sed -i 's|^#\(FILE FREEFORM = 7E374E25\)|\1|; s|^#\(    SECTION \)|\1|; s|^#\(}\)|\1|' "$INC"
  echo "  ACPI.inc etkinlestirildi ($(grep -c '^    SECTION RAW' "$INC") tablo)"
fi

# ---------------------------------------------------------------- derle
say "Derleniyor ($REL)"
cd "$MU"
[ -d .venv ] && source .venv/bin/activate || true

# Eski ciktiyi sil ki "yeni uretildi mi" sorusu belirsiz kalmasin
rm -f "$MU"/Mu-${DEV}*.img "$MU"/uefi-boot.tar

set +e
python3 build_uefi.py -d "$DEV" -r "$REL" $CLEAN
RC=$?
set -e
if [ $RC -ne 0 ]; then
  echo
  echo "DERLEME BASARISIZ (cikis kodu $RC). Cikti uretilmedi."
  echo "Ayrintili log: $MU/Build/BUILDLOG_${DEV}Pkg.txt"
  exit $RC
fi

# ---------------------------------------------------------------- rapor
# build_uefi.py model son ekiyle "Mu-${DEV}-0.img" de uretebiliyor,
# o yuzden isme sabitlemiyoruz: en yeni eslesmeyi aliyoruz.
IMG="$(ls -t "$MU"/Mu-${DEV}*.img 2>/dev/null | head -1)"
say "Sonuc"
if [ -n "$IMG" ] && [ -f "$IMG" ]; then
  echo "Uretilen imaj: $(basename "$IMG")"
  ls -lh "$IMG"
  RPT="$MU/Build/${DEV}Pkg/${REL}_CLANGPDB/BUILD_REPORT.TXT"
  if [ -f "$RPT" ]; then
    echo "--- FV kullanimi:"
    grep -A6 -i "FV Space Information" "$RPT" | head -12 || true
    echo "--- Shell FV'de mi:"
    if grep -qi '7C04A583' "$RPT"; then echo "  EVET (7C04A583 bulundu)"; else echo "  HAYIR"; fi
  fi
  # Odin tar'ini her zaman uret - flash icin gereken bu.
  ( cd "$MU" && cp -f "$IMG" boot.img \
      && tar -H ustar -cf uefi-boot.tar boot.img && rm -f boot.img )

  # Masaustunu bul. OneDrive yonlendirmesi ve Turkce klasor adi oncelikli
  # (ORTAM.md: C:\Users\orhun\OneDrive\Masaustu).
  DESK=""
  if [ -n "${OUTDIR:-}" ]; then
    DESK="$OUTDIR"
  else
    for p in /mnt/c/Users/*/OneDrive/Masa*st* \
             /mnt/c/Users/*/OneDrive/Desktop \
             /mnt/c/Users/*/Masa*st* \
             /mnt/c/Users/*/Desktop; do
      [ -d "$p" ] && { DESK="$p"; break; }
    done
  fi

  # Proje icindeki out/ klasorune de koy (Mac'e senkronlamak icin sabit yol)
  mkdir -p "$PROJ/out"
  cp -f "$IMG" "$MU/uefi-boot.tar" "$PROJ/out/" 2>/dev/null || true
  if [ "$NO_TOUCH" = 1 ]; then
    cp -f "$MU/uefi-boot.tar" "$PROJ/out/uefi-boot-no-touch.tar"
  fi

  if [ -n "$DESK" ]; then
    mkdir -p "$DESK/${DEV}-flash"
    cp -f "$IMG" "$DESK/${DEV}-flash/"
    cp -f "$MU/uefi-boot.tar" "$DESK/${DEV}-flash/"
    if [ "$NO_TOUCH" = 1 ]; then
      cp -f "$MU/uefi-boot.tar" "$DESK/${DEV}-flash/uefi-boot-no-touch.tar"
      echo "  uefi-boot-no-touch.tar -> Odin, AP (dokunmatik DXE kapali kontrol)"
    fi
    echo "Flash dosyalari: $DESK/${DEV}-flash"
    echo "  uefi-boot.tar        -> Odin, AP slotu  (Re-Partition ISARETSIZ)"
    echo "  Mu-${DEV}.img    -> heimdall flash --BOOT Mu-${DEV}.img"
  else
    echo "Masaustu bulunamadi. OUTDIR=/mnt/c/... ile yol verebilirsin."
    echo "  $IMG"
    echo "  $MU/uefi-boot.tar"
  fi
else
  echo "Derleme gecti ama Mu-${DEV}*.img bulunamadi."
  echo "Bulunanlar:"; ls -la "$MU"/*.img 2>/dev/null || echo "  hic .img yok"
  exit 1
fi

#
# EbsTest.efi'yi disari cikar. Firmware'e gomulu degil; testi yapmak
# isteyince esp/ icine elle kopyalanacak.
#
#
# B2LGoster.efi - buyuk puntolu tani ekrani. esp/ icine SABIT konuyor
# (EbsTest gibi opsiyonel degil): her olcumde ise yariyor ve firmware'e
# gomulu olmadigi icin acilis yolunu hic etkilemiyor.
#
GOS="$(find "$MU/Build" -name 'B2LGoster.efi' 2>/dev/null | head -1)"
if [ -n "$GOS" ]; then
  mkdir -p "$PROJ/out"
  cp -f "$GOS" "$PROJ/out/B2LGoster.efi"
  cp -f "$GOS" "$PROJ/esp/B2LGOSTR.EFI"
  echo "B2LGoster hazir: esp/B2LGOSTR.EFI"
fi

#
# B2LTool.efi - GPT dokumu, MISC BCB ile TWRP'ye giris ve imaj yazma.
# FV'ye gomulur (DXE.inc) ve BDS'te boot option olur. Ayrica esp/ icine de
# konuyor: Shell'den "fs0:\B2LTOOL.EFI part" gibi dogrudan calistirilabilsin.
#
TOOL="$(find "$MU/Build" -name 'B2LTool.efi' 2>/dev/null | head -1)"
if [ -n "$TOOL" ]; then
  mkdir -p "$PROJ/out"
  cp -f "$TOOL" "$PROJ/out/B2LTool.efi"
  cp -f "$TOOL" "$PROJ/esp/B2LTOOL.EFI"
  echo "B2LTool hazir: esp/B2LTOOL.EFI"
fi

#
# B2LBootTani.efi - bootmgfw'nin kalici bekleme noktasini, ayrilmis RAM veya
# MMIO okumadan GOP ustundeki bir nabizla ayirir. Yalnizca
# `cache_esp.sh --win --system --boot-tani` yolunda calistirilir.
#
BTANI="$(find "$MU/Build" -name 'B2LBootTani.efi' 2>/dev/null | head -1)"
if [ -n "$BTANI" ]; then
  mkdir -p "$PROJ/out"
  cp -f "$BTANI" "$PROJ/out/B2LBootTani.efi"
  cp -f "$BTANI" "$PROJ/esp/B2LBOOT.EFI"
  echo "B2LBootTani hazir: esp/B2LBOOT.EFI"
fi

EBS="$(find "$MU/Build" -name 'EbsTest.efi' 2>/dev/null | head -1)"
if [ -n "$EBS" ]; then
  mkdir -p "$PROJ/out"
  cp -f "$EBS" "$PROJ/out/EbsTest.efi"
  echo
  echo "EbsTest hazir: out/EbsTest.efi"
  echo "  Denemek icin:  cp out/EbsTest.efi esp/EBSTEST.EFI && bash cache_esp.sh --win --system"
  echo "  Geri almak icin: rm esp/EBSTEST.EFI"
fi
