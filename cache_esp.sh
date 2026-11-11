#!/usr/bin/env bash
#
# CACHE bolumu icin ESP imaji uretir. esp/ klasorundeki her seyi iceri koyar
# (startup.nsh, kernel, initramfs...). Iki cikti verir:
#
#   out/cache.img       ham imaj   -> recovery + adb dd yolu
#   out/cache-esp.tar   sparse tar -> Odin, AP slotu
#
#   bash cache_esp.sh [--win] [--baseline|--boot-tani|--psci-hvc|--bos] [iceri-konacak-dosya.efi]
#
# Argüman verilmezse Mu-Silicium build'indeki Shell.efi aranir.
#
# ---------------------------------------------------------------------------
# Dolu ESP uretim yolunun uc temel kisiti var:
#
# 1) -S 4096 : UFS'in mantiksal blok boyutu 4096 (log: "BlockSize : 4096").
#    FAT'in BPB'sindeki sektor boyutu BlockIo'nunkiyle esitlesmeli, yoksa
#    FatPkg birimi baglamaz.
#
# 2) Dolu CACHE ESP yolu, daha once cihazda dogrulanan FAT16 + 32 KiB kume
#    davranisini korur. CACHE'in dogru boyutu 600 MiB oldugundan FAT32 + 4 KiB
#    kume de gecerlidir (yaklasik 153600 kume). `--win --bos` bunu kullanarak
#    tam boyutlu ve icinde dosya olmayan FAT32 imaji uretir.
#
# 3) sparse  : Odin ham FAT imajini reddediyor. Android sparse formatina
#    (0xED26FF3A) cevirince kabul ediyor. dd yolu icin ham imaj gerekli,
#    o yuzden ikisini birden uretiyoruz.
# ---------------------------------------------------------------------------
set -euo pipefail
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MU="${MU_SILICIUM:-$HOME/Mu-Silicium}"
OUT="$PROJ/out"; mkdir -p "$OUT"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

#
# --win   : sadece Windows dosyalarini koy (Linux kernel/initramfs haric).
# --linux : Linux dosyalarini BILEREK buyuk boluma yaz. Asagidaki korumayi
#           kaldirmaz, niyeti acikca soyler.
# --bos   : eski CACHE ESP icerigini bos, 600 MiB FAT32 CACHE imajiyla
#           degistir. Bu bir Windows WinPE turudur; bayraksiz calisma Linux
#           modu oldugu icin --win zorunludur.
#
# 600 MiB'lik bolume hem 570 MB'lik boot.wim hem 68 MB'lik Linux dosyalari
# sigmiyor - bir tur mcopy "Disk full" verdi ve sources/ bos kaldi.
# Iki mod ayri ayri calisiyor, ikisi ayni anda degil.
#
#   --win  -> Windows: bootmgfw + BCD + boot.sdi + boot.wim, Linux dosyalari YOK
#   (bayraksiz) -> Linux: kernel + initramfs, WINDOWS DOSYALARI DA YOK
#
# Ikincisi onemli: onceden bayraksiz mod Windows dosyalarini da koyuyordu ve
# startup.nsh once bootmgfw'yi gordugu icin Linux'a hic sira gelmiyordu.
# Yani "Linux modu" diye bir sey yoktu.
#
WINONLY=0
LINUXOK=0
BIGPART=0
BOSALT=0
BASELINE=0
PSCI_HVC=0
BOOT_TANI=0
DEV="${B2L_DEVICE:-beyond2lte}"
ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --win)    WINONLY=1 ;;
    --linux)  LINUXOK=1 ;;
    --system) BIGPART=1 ;;
    --bos)    BOSALT=1 ;;
    --baseline) BASELINE=1 ;;
    --psci-hvc) PSCI_HVC=1 ;;
    --boot-tani) BOOT_TANI=1 ;;
    # Cihaz yalnizca masaustu klasor adini etkiler; bolum adlari (CACHE,
    # SYSTEM) PIT'ten gelir ve iki cihazda aynidir.
    -d|--device) DEV="${2:?-d icin cihaz adi gerekli}"; shift ;;
    *)        ARGS+=("$1") ;;
  esac
  shift
done
if [ ${#ARGS[@]} -gt 0 ]; then set -- "${ARGS[@]}"; else set --; fi

#
# --baseline: Windows'un kendisi disinda B2L tani kodu calistirmayan ESP.
#
# Bu mod B2LGOSTR.EFI ve EBSTEST.EFI'yi imaja ALMAZ, normal startup.nsh
# yerine yalnizca bootmgfw.efi'yi bulan kucuk baslangic betigini koyar.
# B2LGoster'in tum conventional RAM'i yaz/oku/geri-yaz taramasi yaptigi
# unutulmamali: normal Shell yolu onu Windows'tan ONCE calistirir. Bu,
# b2lfb Start=4 ve DXE.inc'teki B2L suruculerinin kapatilmasiyla birlikte
# gercekten aletsiz tabani olusturur.
#
# UFS/PWM veya Watchdog yamalarini DEGISTIRMEZ. Bir deneyde tek degisken
# aletler olsun diye onlar yap.sh'in mevcut ayarinda kalir.
#
if [ "$BASELINE" = 1 ] && [ "$WINONLY" = 0 ]; then
  echo "--baseline yalnizca Windows modu ile kullanilir: bash cache_esp.sh --win --system --baseline"
  exit 1
fi
if [ "$PSCI_HVC" = 1 ] && [ "$WINONLY" = 0 ]; then
  echo "--psci-hvc yalnizca Windows modu ile kullanilir: bash cache_esp.sh --win --system --psci-hvc"
  exit 1
fi
if [ "$BOOT_TANI" = 1 ] && [ "$WINONLY" = 0 ]; then
  echo "--boot-tani yalnizca Windows modu ile kullanilir: bash cache_esp.sh --win --system --boot-tani"
  exit 1
fi
if [ "$BOOT_TANI" = 1 ] && [ "$BASELINE" = 1 ]; then
  echo "--boot-tani ve --baseline ayni anda kullanilmaz."
  exit 1
fi
if [ "$BOOT_TANI" = 1 ] && [ ! -s "$PROJ/esp/B2LBOOT.EFI" ]; then
  echo "HATA: esp/B2LBOOT.EFI yok. Once: bash yap.sh"
  exit 1
fi
if [ "$PSCI_HVC" = 1 ] && [ "$BASELINE" = 1 ]; then
  echo "--psci-hvc ve --baseline birlikte kullanilmaz: HVC deneyinde onceki acilisin HVC kaydi ekranda okunur."
  exit 1
fi
if [ "$BOSALT" = 1 ] && [ "$WINONLY" = 0 ]; then
  echo "--bos yalnizca Windows WinPE turunde kullanilir: bash cache_esp.sh --win --bos"
  echo "Bayraksiz calisma Linux modu oldugu icin bos CACHE imaji yanlis moda etiketlenmez."
  exit 1
fi
if [ "$BOSALT" = 1 ] && [ "$BIGPART" = 1 ]; then
  echo "--bos yalniz CACHE icindir; --system ile birlikte kullanilmaz."
  echo "Dogru komut: bash cache_esp.sh --win --bos"
  exit 1
fi
if [ "$WINONLY" = 1 ]; then say "MOD: Windows"; else say "MOD: Linux"; fi
if [ "$BASELINE" = 1 ]; then
  say "MOD: Windows temiz taban (B2L tani yok)"
fi
if [ "$PSCI_HVC" = 1 ]; then
  [ -s "$PROJ/esp/B2LGOSTR.EFI" ] || {
    echo "HATA: HVC kaydini gosterecek B2LGOSTR.EFI yok. Once: bash yap.sh --psci-hvc"
    exit 1
  }
  say "MOD: Windows PSCI HVC deneyi (B2LGoster sonraki acilista HVC kaydini gosterir)"
fi
if [ "$BOOT_TANI" = 1 ]; then
  say "MOD: Windows bootmgfw canlilik tanisi (sabit fiziksel adres yok)"
fi

#
# GUVENLIK KILIDI: --system + Linux modu neredeyse kesin bir hata.
#
# BU BIR TURA MAL OLDU. Talimat "bash cache_esp.sh --system" seklinde
# verildi, --win unutuldu. Script gorevini kusursuz yapti: SYSTEM'e bir
# LINUX ESP'si yazdi. Cihaz da startup.nsh'in Linux dalini calistirip
# vmlinuz-lts'i acti. Windows dosyalari imaja hic girmemisti.
#
# Ekranda "MOD: Linux" yaziyordu ama tek satir, elli satir cikti icinde.
# Sessiz dogru davranis, gurultulu yanlis davranistan daha tehlikeli
# olabiliyor - burada oyleydi.
#
# --system'in VAR OLMA SEBEBI 742 MB'lik boot.wim'in 600 MiB'lik CACHE'e
# sigmamasi. Linux dosyalari 68 MB; onlar icin buyuk bolume gecmenin hicbir
# anlami yok. Yani bu kombinasyon istenerek yazilmis olamaz.
#
if [ "$BIGPART" = 1 ] && [ "$WINONLY" = 0 ] && [ "$BOSALT" = 0 ] && [ "$LINUXOK" = 0 ]; then
  echo
  echo "  ###############################################################"
  echo "  # DUR: --system verildi ama --win YOK."
  echo "  #"
  echo "  # Bu haliyle SYSTEM bolumune bir LINUX ESP'si yazilir ve"
  echo "  # cihaz Windows yerine vmlinuz-lts'i acar."
  echo "  #"
  echo "  # Windows istiyorsan:   bash cache_esp.sh --win --system"
  echo "  # Gercekten Linux mu:   bash cache_esp.sh            (CACHE'e)"
  echo "  # Linux'u SYSTEM'e:     bash cache_esp.sh --linux --system"
  echo "  #"
  echo "  # Son secenek Linux kontrol deneyi icin: iki ayri ESP birakmak"
  echo "  # (CACHE'te Linux, SYSTEM'de Windows) kabugun hangisini bulacagini"
  echo "  # belirsiz birakiyor - bu tuzaga eski B2LGOSTR kopyalariyla zaten"
  echo "  # dustuk. Tek birim, tek startup.nsh."

  echo "  ###############################################################"
  echo
  exit 1
fi

#
# --system : CACHE (600 MiB) yerine SYSTEM (5.57 GB) bolumunu hedefle.
#
# NEDEN GEREKTI: Windows 11 23H2'nin boot.wim'i 742 MB. LZX ile yeniden
# sikistirmak HIC kazanc vermedi (742 -> 742) cunku ISO'nunki zaten LZX;
# acilmis icerik 1650 MiB. Yani sikistirmayla cozulecek bir sey yok, 600
# MiB'lik bolume sigmiyor.
#
# ODIN HEDEFI DOSYA ADIYLA BELIRLENIYOR: tar icindeki "cache.img" CACHE'e,
# "system.img" SYSTEM'e yaziliyor. Ikisini karistirma.
#
# FAT32'YE GECIYORUZ: FatPkg FAT32 icin en az 65525 kume istiyor
# (Init.c: MaxCluster < FAT_MAX_FAT16_CLUSTER -> VOLUME_CORRUPTED).
# 600 MiB'de 4K kumeyle 153600 kume FAT16'nin ust sinirini asiyordu, bu
# yuzden orada FAT16 + 32 KiB kume kullanmistik. 2 GiB'de 4K kumeyle
# 524288 kume cikiyor - FAT32 icin fazlasiyla gecerli.
#
# BOLUMUN TAMAMINI DOLDURMUYORUZ: 2 GiB'lik bir dosya sistemi 5.57 GB'lik
# bolumun basina yaziliyor, gerisi bos kaliyor. FatPkg birimin boyutunu
# BPB'den okuyor, bolum tablosundan degil - sorun cikmiyor. Sparse imaj
# oldugu icin aktarilan veri yine kucuk.
#
if [ "$BIGPART" = 1 ]; then
  PART="system"
  #
  # IMAJ BOYUTU ARTIK ICERIGE GORE - SABIT 2 GiB DEGIL.
  #
  # Her turda 2 GiB'lik bir dosya sifirdan uretiliyor, bicimlendiriliyor ve
  # img2simg ile bastan sona taraniyordu; oysa icerik ~800 MB. Sparse imaj
  # sayesinde AKTARILAN veri zaten kucuktu, ama YEREL islem tam boyut
  # uzerinden yapiliyordu ve dongunun gorunmez maliyetiydi.
  #
  # Icerik + %30 pay, 512 MiB alt sinirla. Alt sinirin sebebi FAT32: FatPkg
  # en az 65525 kume istiyor (Init.c), 4 KiB kumeyle bu 256 MiB ediyor.
  # 512 ile rahat payda kaliyoruz.
  #
  ICERIK_MB=$(du -sm "$PROJ/esp" 2>/dev/null | cut -f1 || echo 800)
  IMAJ_MB=$(( ICERIK_MB * 130 / 100 ))
  [ "$IMAJ_MB" -lt 512 ] && IMAJ_MB=512
  [ "$IMAJ_MB" -gt 5000 ] && IMAJ_MB=5000    # SYSTEM 5.57 GB
  SZ=$(( IMAJ_MB * 1048576 ))
  FATOPT="-F 32 -S 4096 -s 1"
  FATDESC="FAT32 (${IMAJ_MB} MiB, 4096 bayt sektor, 4 KiB kume)"
  LIMIT_MB=$(( IMAJ_MB - 60 ))
  say "HEDEF BOLUM: SYSTEM (5.57 GB) - imaj ${IMAJ_MB} MiB (icerik ${ICERIK_MB} MB)"
else
  PART="cache"
  SZ=629145600
  FATOPT="-F 16 -S 4096 -s 8"
  FATDESC="FAT16 (600 MiB, 4096 bayt sektor, 32 KiB kume)"
  LIMIT_MB=588
fi

#
# CACHE = 153600 blok x 4096 = 629145600 bayt = 600 MiB.
#
# Onceden 512 bayt varsayip 75 MB imaj yaziyorduk; bolumun geri kalani bos
# kaliyordu. Cihazdan geri okununca gercek boyut 600 MiB cikti (PARTITIONS.md).
# Windows'un boot.wim'i icin bu alana ihtiyacimiz var.
#
# KUME BOYUTU: Dolu ESP'nin tarihsel FAT16 yolu "-s 8" ile 32 KiB kume ve
# yaklasik 19200 kume kullanir. Bosaltma yolu ise FAT32 + 4 KiB kume kullanir;
# 600 MiB'de yaklasik 153600 kume FAT32'nin gecerli araligindadir.
#
#
# ---------------------------------------------------------------------------
# --bos : WINDOWS TURUNDE BOS FAT32 CACHE DOSYASI URET
# ---------------------------------------------------------------------------
# NEDEN GEREKTI - BES TURLUK KAFA KARISIKLIGININ SEBEBI BUYDU:
#
# Bir noktada ESP'yi SYSTEM bolumune flasladik (23H2'nin 742 MB'lik WIM'i
# 600 MiB'lik CACHE'e sigmiyordu). Sonra CACHE'e geri dondük - ama SYSTEM'de
# birakt.gimiz TAM ESP orada durmaya devam etti, icindeki ESKI startup.nsh
# ile birlikte.
#
# EFI Shell startup.nsh'i hangi FAT biriminde ONCE bulursa onu calistirir.
# Iki birim vardi ve eskisini buldu. Sonuc: depoda dogru dosya, cihaza dogru
# imaj gidiyor, ama calisan script eski. Yaptigimiz hicbir degisiklik test
# edilmedi - eski map ciktisi, kaybolmayan B2L UFS satirlari, bulunamayan
# EBSTEST.EFI, degismeyen 15 saniye, hepsi bundan.
#
# Bu mod yalnizca `--win --bos` ile cagrilir. Tam CACHE boyutunda, icinde
# dosya bulunmayan FAT32 olusturur. Boylece eski startup.nsh silinir ama
# Windows bolumu gecerli bir FAT32 birim olarak gorebilir.
#
if [ "$BOSALT" = 1 ]; then
  IMG="$OUT/$PART.img"
  say "BOS FAT32 URETILIYOR: $(echo "$PART" | tr a-z A-Z)  (600 MiB, 4096 bayt sektor/kume)"
  rm -f "$IMG"
  dd if=/dev/zero of="$IMG" bs=1M count=600 status=none
  mkfs.vfat -F 32 -S 4096 -s 1 -n CACHE "$IMG" >/dev/null

  BPB_SECTOR_BYTES="$(od -An -tu2 -j11 -N2 "$IMG" | tr -d ' ')"
  BPB_SECTORS_PER_CLUSTER="$(od -An -tu1 -j13 -N1 "$IMG" | tr -d ' ')"
  BPB_LABEL="$(dd if="$IMG" bs=1 skip=71 count=11 status=none | tr -d ' ')"
  BPB_FILESYSTEM="$(dd if="$IMG" bs=1 skip=82 count=8 status=none | tr -d ' ')"
  [ "$BPB_SECTOR_BYTES" = 4096 ] || {
    echo "HATA: bos CACHE BPB sektor boyutu 4096 degil: $BPB_SECTOR_BYTES"
    exit 1
  }
  [ "$BPB_SECTORS_PER_CLUSTER" = 1 ] || {
    echo "HATA: bos CACHE BPB kume boyutu 1 sektor degil: $BPB_SECTORS_PER_CLUSTER"
    exit 1
  }
  [ "$BPB_FILESYSTEM" = FAT32 ] || {
    echo "HATA: bos CACHE dosya sistemi FAT32 degil: $BPB_FILESYSTEM"
    exit 1
  }
  [ "$BPB_LABEL" = CACHE ] || {
    echo "HATA: bos CACHE birim etiketi CACHE degil: $BPB_LABEL"
    exit 1
  }
  echo "  dogrulandi: FAT32, 4096 bayt sektor, 4096 bayt kume, etiket CACHE"
  if command -v mdir >/dev/null; then
    MTOOLS_SKIP_CHECK=1 mdir -i "$IMG" ::/
  fi

  WORK="$(mktemp -d)"
  if command -v img2simg >/dev/null; then
    img2simg "$IMG" "$WORK/$PART.img"
  else
    cp "$IMG" "$WORK/$PART.img"
  fi
  ( cd "$WORK" && tar -H ustar -cf "$OUT/$PART-esp.tar" "$PART.img" )
  rm -rf "$WORK"

  ls -lh "$OUT/$PART-esp.tar"
  for p in /mnt/c/Users/*/OneDrive/Masa*st* /mnt/c/Users/*/OneDrive/Desktop \
           /mnt/c/Users/*/Masa*st* /mnt/c/Users/*/Desktop; do
    [ -d "$p" ] || continue
    mkdir -p "$p/${DEV}-flash"
    cp -f "$OUT/$PART-esp.tar" "$p/${DEV}-flash/"
    echo
    echo "Masaustune kopyalandi: $p/${DEV}-flash/$PART-esp.tar"
    echo "  Odin, AP slotu (Re-Partition ISARETSIZ)"
    break
  done
  exit 0
fi

APP="${1:-}"
if [ -z "$APP" ]; then
  APP="$(find "$MU/Build" -name 'Shell.efi' -path '*ShellPkg*' 2>/dev/null | head -1)"
  [ -n "$APP" ] || APP="$(find "$MU/Build" -maxdepth 5 -name 'Shell.efi' 2>/dev/null | head -1)"
fi
[ -f "$APP" ] || { echo "EFI uygulamasi bulunamadi. Yolu argüman olarak ver."; exit 1; }
say "Icerik: $APP ($(du -h "$APP" | cut -f1))"

# `out/b2lufs/b2lufs.sys` varsa bu WinPE turu o surucuyu olcmek icindir.
# Eski hata zinciri burada kapanir: WIM'e basariyla enjekte edilmis bir
# b2lufs paketi, daha sonra eski bir ESP arsiviyle degistirilebiliyordu ve
# cihazdaki startnet yalnizca "paket yok" diyordu. Sparse SYSTEM imajini
# uretmeden once WIM'in ayni SYS hash'ini tasiyan manifestini geri okuyoruz.
# Bu bir imza dogrulamasinin yerine gecmez (onu tools_b2lufs.sh yapar);
# yalniz dogru WIM'in dogru ESP imajina girdigini zorunlu kilar.
verify_winpe_b2lufs_package() {
  local VerifyDir ExpectedSys ManifestSys

  [ -s "$PROJ/out/b2lufs/b2lufs.sys" ] || return 0
  [ -s "$PROJ/esp/sources/boot.wim" ] || {
    echo "HATA: B2LUFS SYS var ama esp/sources/boot.wim yok. Once tools_b2lufs.sh calistir."
    return 1
  }
  command -v wimlib-imagex >/dev/null || {
    echo "HATA: B2LUFS WIM kapisi icin wimlib-imagex gerekli."
    return 1
  }

  VerifyDir="$(mktemp -d)" || {
    echo "HATA: B2LUFS WIM dogrulama dizini olusturulamadi."
    return 1
  }
  if ! wimlib-imagex extract "$PROJ/esp/sources/boot.wim" 1 \
      /Windows/B2LUFS/manifest.txt --dest-dir="$VerifyDir" --no-acls >/dev/null; then
    rm -rf "$VerifyDir"
    echo "HATA: boot.wim'de B2LUFS manifest yok; eski WIM'i sparse ESP'ye koymak reddedildi."
    echo "Once: B2L_SKIP_MSVC_BUILD=1 bash tools_b2lufs.sh"
    return 1
  fi

  ExpectedSys="$(sha256sum "$PROJ/out/b2lufs/b2lufs.sys" | awk '{print $1}')"
  ManifestSys="$(sed -n 's/^B2LUFS_SYS_SHA256=//p' "$VerifyDir/manifest.txt" | head -n 1)"
  rm -rf "$VerifyDir"
  if [ "$ManifestSys" != "$ExpectedSys" ]; then
    echo "HATA: boot.wim B2LUFS SYS hash'i derlenen SYS ile ayni degil; eski WIM reddedildi."
    echo "  WIM: ${ManifestSys:-YOK}"
    echo "  SYS: $ExpectedSys"
    echo "Once: B2L_SKIP_MSVC_BUILD=1 bash tools_b2lufs.sh"
    return 1
  fi

  say "WinPE B2LUFS paketi dogrulandi"
  echo "  SYS SHA256: ${ExpectedSys:0:16}"
}

if [ "$WINONLY" = 1 ]; then
  verify_winpe_b2lufs_package
fi

IMG="$OUT/$PART.img"

say "$FATDESC imaji olusturuluyor"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=$((SZ/1048576)) status=none
mkfs.vfat $FATOPT -n UEFIESP "$IMG" >/dev/null

# startup.nsh: Shell acilista bunu otomatik calistirir. Klavye olmadan
# komut vermenin tek yolu. Iki yere de koyuyoruz cunku Shell'in arama sirasi
# surume gore degisiyor: once kendi dizini, sonra kok.
NSH="$PROJ/esp/startup.nsh"
if [ "$BASELINE" = 1 ]; then
  NSH="$PROJ/esp/startup-baseline.nsh"
fi
if [ "$BOOT_TANI" = 1 ]; then
  NSH="$PROJ/esp/startup-boot-tani.nsh"
fi

# Bir tur, cihazdan geri okunan imajda ESKI startup.nsh cikti: kernel'ler
# eklendikten sonra ama script duzenlenmeden once uretilmis bir imaj
# flaslanmisti ve hicbir log uretilmedi. Boyut+ozet basiyoruz ki hangi surumun
# gittigi belli olsun; cihazdan geri okuyunca bununla karsilastir.
if [ -f "$NSH" ]; then
  say "startup.nsh: $(stat -c%s "$NSH") bayt  sha1=$(sha1sum "$NSH" | cut -c1-12)"
fi

if command -v mcopy >/dev/null; then
  export MTOOLS_SKIP_CHECK=1
  mmd   -i "$IMG" ::/EFI ::/EFI/BOOT
  mcopy -i "$IMG" "$APP" ::/EFI/BOOT/BOOTAA64.EFI
  [ -f "$NSH" ] && { mcopy -i "$IMG" "$NSH" ::/startup.nsh
                     mcopy -i "$IMG" "$NSH" ::/EFI/BOOT/startup.nsh; }
  # esp/ agacinin tamamini al: dosyalar VE alt klasorler.
  # Alt klasor destegi Windows icin gerekli - bootmgfw BCD'yi
  # \EFI\Microsoft\Boot\BCD yolunda ariyor.
  COPYFAIL=0
  for f in "$PROJ"/esp/*; do
    b="$(basename "$f")"
    [ "$b" = "startup.nsh" ] && continue
    [ "$b" = "startup-baseline.nsh" ] && continue
    [ "$b" = "startup-boot-tani.nsh" ] && continue
    [ -e "$f" ] || continue
    if [ "$BASELINE" = 1 ]; then
      case "$b" in
        B2LGOSTR.EFI|B2LBOOT.EFI|EBSTEST.EFI)
          echo "  - $b (atlandi: temiz taban)"
          continue
          ;;
      esac
    fi
    if [ "$BOOT_TANI" = 1 ]; then
      case "$b" in
        B2LGOSTR.EFI|EBSTEST.EFI)
          echo "  - $b (atlandi: sabit fiziksel adres tanisi kapali)"
          continue
          ;;
      esac
    fi
    if [ "$WINONLY" = 1 ]; then
      case "$b" in
        vmlinuz-*|initramfs-*) echo "  - $b (atlandi: Windows modu)"; continue ;;
      esac
    else
      # EFI/BOOT zaten yukarida ayrica dolduruldu; buradaki EFI klasoru
      # sadece Microsoft/ agacini tasiyor, Linux modunda istemiyoruz.
      case "$b" in
        EFI|sources|boot) echo "  - $b (atlandi: Linux modu)"; continue ;;
      esac
    fi
    # Buyukten kucuge degil, sirayla kopyaliyoruz; hata SESSIZ GECMESIN.
    if [ -d "$f" ]; then
      if mcopy -i "$IMG" -s "$f" "::/"; then echo "  + $b/ (klasor)"
      else echo "  ! $b/ KOPYALANAMADI"; COPYFAIL=1; fi
    else
      if mcopy -i "$IMG" "$f" "::/$b"; then echo "  + $b ($(du -h "$f" | cut -f1))"
      else echo "  ! $b KOPYALANAMADI ($(du -h "$f" | cut -f1))"; COPYFAIL=1; fi
    fi
  done
  if [ "$COPYFAIL" = 1 ]; then
    echo
    echo "  DIKKAT: en az bir dosya imaja giremedi (buyuk ihtimalle yer yok)."
    echo "  Windows icin:  bash cache_esp.sh --win"
    echo "  esp/ toplami: $(du -sh "$PROJ/esp" | cut -f1) / imaj ${LIMIT_MB} MB"
  fi
  # Windows icin boot.wim gerekiyor; sessizce eksik kalmasin.
  if mdir -i "$IMG" ::/sources >/dev/null 2>&1; then
    echo "--- sources/ (WinPE):"
    mdir -i "$IMG" ::/sources | sed 's/^/  /'
  else
    echo "--- sources/boot.wim YOK  (WinPE acilmaz; tools_winboot.sh ciktisina bak)"
  fi
  echo "--- bos alan:"
  mdir -i "$IMG" ::/ | tail -1
  echo "--- imaj icerigi:"
  mdir -i "$IMG" ::/
  mdir -i "$IMG" ::/EFI/BOOT
else
  echo "mtools yok -> sudo apt install -y mtools   (loop mount'a dusuluyor)"
  T=$(mktemp -d); sudo mount -o loop "$IMG" "$T"
  sudo mkdir -p "$T/EFI/BOOT"; sudo cp "$APP" "$T/EFI/BOOT/BOOTAA64.EFI"
  [ -f "$NSH" ] && { sudo cp "$NSH" "$T/startup.nsh"; sudo cp "$NSH" "$T/EFI/BOOT/startup.nsh"; }
  sudo umount "$T"; rmdir "$T"
fi

say "Odin icin sparse imaj + tar"
WORK="$(mktemp -d)"
if command -v img2simg >/dev/null; then
  img2simg "$IMG" "$WORK/$PART.img"
  MAGIC=$(od -An -tx4 -N4 "$WORK/$PART.img" | tr -d ' ')
  echo "  sparse sihirli sayisi: 0x$MAGIC  (beklenen: ed26ff3a)"
else
  echo "  UYARI: img2simg yok -> sudo apt install -y android-sdk-libsparse-utils"
  echo "  Ham imaj tar'lanıyor; Odin reddedebilir."
  cp "$IMG" "$WORK/$PART.img"
fi
( cd "$WORK" && tar -H ustar -cf "$OUT/$PART-esp.tar" "$PART.img" )
rm -rf "$WORK"

say "Cikti"
ls -lh "$IMG" "$OUT/$PART-esp.tar"

for p in /mnt/c/Users/*/OneDrive/Masa*st* /mnt/c/Users/*/OneDrive/Desktop \
         /mnt/c/Users/*/Masa*st* /mnt/c/Users/*/Desktop; do
  [ -d "$p" ] || continue
  mkdir -p "$p/${DEV}-flash"
  cp -f "$OUT/$PART-esp.tar" "$IMG" "$p/${DEV}-flash/"
  echo
  echo "Masaustune kopyalandi: $p/${DEV}-flash"
  echo "  $PART-esp.tar  -> Odin, AP slotu (Re-Partition ISARETSIZ)"
  echo "  $PART.img      -> recovery: adb push + dd"
  echo "  hedef bolum: $(echo "$PART" | tr a-z A-Z)"
  break
done
