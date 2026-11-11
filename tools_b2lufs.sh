#!/usr/bin/env bash
#
# B2LUFS — WDK basliklariyla ARM64 UFS StorPort miniportunu paketle,
# mevcut b2lfb test sertifikasi ile imzala ve yalniz WinPE WIM'ine koy.
#
# Bu tur LUN 0 icin normal okuma/yazma disk surucusudur. ACPI'nin verdigi alti MMIO
# kaynagini StorPort uzerinden sahiplenir; firmware'in canli UFS linkini
# resetlemeden tek DMA slotu kurar ve yalnız kullanıcı LUN 0 için normal
# okuma/yazma SCSI CDB'lerini iletir. Boot ve RPMB LUN'ları gizli kalır.
#
# WSL'de, Windows'taki WDK + Visual Studio Build Tools gorunurken calistir:
#   bash tools_b2lufs.sh
#
# Gerekenler: clang, lld, llvm, osslsigncode, openssl, wimlib-imagex;
# Windows tarafinda WDK (Inf2Cat dahil) ve MSVC C++ Build Tools (ARM64 hedefi).
# Yalniz b2lufs.sys, Windows PowerShell'in baslattigi yerel MSVC cl.exe/link.exe
# araclariyla derlenir. Salt-okunur WinPE GPT dump yardimcisi WSL clang/lld ile
# uretilir; surucu INF/CAT paketinin parcasi degildir.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Imza aciklamasi cihaza gore degisir; SERTIFIKA DEGISMEZ. b2lfb ile ayni
# b2l-test.pem kullanilir cunku cihazin guven deposuna bir kez eklenen kok
# iki cihazda da ayni olmali.
DEV="${B2L_DEVICE:-beyond2lte}"
SRC="$HERE/windows/b2lufs"
OUT="$HERE/out/b2lufs"
WIM="$HERE/esp/sources/boot.wim"
WINROOT="${B2L_WINROOT:-/mnt/c}"
KITS="${B2L_WDK_ROOT:-$WINROOT/Program Files (x86)/Windows Kits/10}"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\nHATA: %s\n' "$*" >&2; exit 1; }

for tool in powershell.exe clang lld-link llvm-dlltool osslsigncode openssl wimlib-imagex sha256sum wslpath; do
  command -v "$tool" >/dev/null || die "$tool yok"
done

[ -s "$WIM" ] || die "boot.wim yok; once tools_winpe_adk.sh ve tools_b2lfb.sh calistir"
[ -d "$KITS/Include" ] || die "WDK Include yok: $KITS/Include"

# En yeni, hem WDK basliklarini hem ARM64 km kutuphanelerini iceren surum.
WDK_VERSION=""
while IFS= read -r Candidate; do
  if [ -f "$KITS/Include/$Candidate/km/ntddk.h" ] && \
     [ -f "$KITS/Include/$Candidate/km/storport.h" ] && \
     [ -f "$KITS/Lib/$Candidate/km/arm64/storport.lib" ] && \
     [ -f "$KITS/Lib/$Candidate/km/arm64/ntoskrnl.lib" ]; then
    WDK_VERSION="$Candidate"
    break
  fi
done < <(find "$KITS/Include" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort -V -r)

[ -n "$WDK_VERSION" ] || die "ARM64 WDK km/storport.lib bulunamadi; Windows WDK'yi kur"
WDK_INC="$KITS/Include/$WDK_VERSION"
WDK_LIB="$KITS/Lib/$WDK_VERSION/km/arm64"

# PnP paketi SYS'ten ibaret degildir: INF ve SYS hash'lerini tasiyan imzali
# .cat gerekir. Inf2Cat / signtool WDK SDK bin agacinda bulunur; WSL Windows
# .exe'sini dogrudan calistirabilir. Araçlardan biri yoksa derlenmis ama
# yuklenemeyecek bir paket birakmak yerine burada dururuz.
INF2CAT="${B2L_INF2CAT:-}"
SIGNTOOL="${B2L_SIGNTOOL:-}"
if [ -z "$INF2CAT" ] || [ -z "$SIGNTOOL" ]; then
  if [ -z "$INF2CAT" ]; then
    INF2CAT="$(find "$KITS/bin" -iname Inf2Cat.exe -type f -print 2>/dev/null | sort -V -r | head -n 1 || true)"
  fi
  if [ -z "$SIGNTOOL" ]; then
    SIGNTOOL="$(find "$KITS/bin" -iname signtool.exe -type f -print 2>/dev/null | sort -V -r | head -n 1 || true)"
  fi
fi
[ -f "$INF2CAT" ] || die "Inf2Cat.exe yok; Windows WDK 'Tools' bilesenini kur veya B2L_INF2CAT ver"
[ -f "$SIGNTOOL" ] || die "signtool.exe yok; Windows SDK Signing Tools kur veya B2L_SIGNTOOL ver"

# ntddk.h vadefs.h ister. Yeni SDK'larda bu UCRT'dedir; eski kurulumlarda
# Visual Studio'nun include dizininde olur. Kullanici isterse B2L_MSVC_INCLUDE
# ile dogrudan yol verir.
MSVC_INC="${B2L_MSVC_INCLUDE:-}"
if [ -z "$MSVC_INC" ] && [ -f "$WDK_INC/ucrt/vadefs.h" ]; then
  MSVC_INC="$WDK_INC/ucrt"
fi
if [ -z "$MSVC_INC" ]; then
  for Root in "$WINROOT/Program Files" "$WINROOT/Program Files (x86)"; do
    [ -d "$Root" ] || continue
    while IFS= read -r Header; do
      MSVC_INC="$(dirname "$Header")"
      break 2
    done < <(find "$Root/Microsoft Visual Studio" -path '*/VC/Tools/MSVC/*/include/vadefs.h' -type f -print 2>/dev/null | sort -V -r)
  done
fi

[ -f "$MSVC_INC/vadefs.h" ] || die "vadefs.h yok; Visual Studio Build Tools C++ bilesenini kur veya B2L_MSVC_INCLUDE ver"

# WSL clang ile MSVC 14.51 intrin.h'nin __prefetch bildirimleri farkli
# builtin imzasina dusebilir. Bu surucuyu dogrudan WDK'nin hedef
# aldigi MSVC cl.exe/link.exe ile derliyoruz. Ortami her cmd cagrisinda
# amd64-host/ARM64-target vcvars dosyasiyla kurmak, kullanicinin acik olan
# terminal ortamindan bagimsiz ve tekrar edilebilir bir derleme saglar.
VCVARS="${B2L_VCVARS_ARM64:-}"
if [ -z "$VCVARS" ]; then
  for Root in "$WINROOT/Program Files" "$WINROOT/Program Files (x86)"; do
    [ -d "$Root/Microsoft Visual Studio" ] || continue
    while IFS= read -r Batch; do
      VCVARS="$Batch"
      break 2
    done < <(find "$Root/Microsoft Visual Studio" \
      -path '*/VC/Auxiliary/Build/vcvarsamd64_arm64.bat' -type f -print 2>/dev/null | sort -V -r)
  done
fi
[ -f "$VCVARS" ] || die "vcvarsamd64_arm64.bat yok; Visual Studio C++ ARM64 build tools kur veya B2L_VCVARS_ARM64 ver"

# MSVC_INC bazen WDK'nin ucrt/ dizini olabilir; bu nedenle cl.exe yolunu
# include yolundan turetmiyoruz. Secilen vcvars dosyasinin ait oldugu VS
# kurulumunun ARM64 hedef aracini ariyoruz.
VS_ROOT="$(cd "$(dirname "$VCVARS")/../../.." && pwd)"
MSVC_ARM64_BIN="${B2L_MSVC_ARM64_BIN:-}"
if [ -z "$MSVC_ARM64_BIN" ]; then
  while IFS= read -r Compiler; do
    CandidateBin="$(dirname "$Compiler")"
    if [ -f "$CandidateBin/link.exe" ]; then
      MSVC_ARM64_BIN="$CandidateBin"
      break
    fi
  done < <(find "$VS_ROOT/VC/Tools/MSVC" \
    -path '*/bin/Hostx64/arm64/cl.exe' -type f -print 2>/dev/null | sort -V -r)
fi
[ -f "$MSVC_ARM64_BIN/cl.exe" ] || die "MSVC ARM64 cl.exe yok: $MSVC_ARM64_BIN/cl.exe; Visual Studio Installer'da 'MSVC v143/v14x C++ ARM64/ARM64EC build tools' bilesenini ekle"
[ -f "$MSVC_ARM64_BIN/link.exe" ] || die "MSVC ARM64 link.exe yok: $MSVC_ARM64_BIN/link.exe; Visual Studio C++ ARM64 build tools bilesenini ekle"

# WDK 28000 ile birlikte ntdef.h her kurulumda km/ altinda olmuyor. Ornegin
# bazi SDK yerlesimlerinde km/crt/ altina tasiniyor. wdm.h bunu acik bir
# acili parantez include'u ile istedigi icin clang, km/ dizini verilse bile
# onu kendiliginden bulamaz. WDK kokunun icinde yeri kesin olarak bulup
# yalniz o dizini ekliyoruz; um/km basliklarini topluca arama yoluna eklemek
# yanlis platform basliklarinin secilmesine yol acabilir.
WDK_NTDEF_INC=""
while IFS= read -r Header; do
  WDK_NTDEF_INC="$(dirname "$Header")"
  break
done < <(find "$WDK_INC" -type f -iname ntdef.h -print 2>/dev/null | sort | head -n 1)

[ -n "$WDK_NTDEF_INC" ] || die "ntdef.h yok: eslesen Windows SDK Headers kurulmamış. WDK $WDK_VERSION icin ayni build numarali Windows SDK'yi kur; ADK tek basina yeterli degil. Beklenen yol: $WDK_INC/shared/ntdef.h"

say "WDK ve MSVC bulundu"
echo "  WDK : $WDK_VERSION"
echo "  MSVC: $MSVC_INC"
echo "  vcvars: $VCVARS"
echo "  ARM64: $MSVC_ARM64_BIN"
echo "  ntdef: $WDK_NTDEF_INC/ntdef.h"

mkdir -p "$OUT"

# PowerShell betigi kaynaklari Windows TEMP altindaki yerel bir dizine
# kopyalar. Boylece WSL'nin \\wsl.localhost UNC yolu ne cmd.exe'nin calisma
# dizini olur ne de cl/link'e aktarilir.
PS_BUILD="$SRC/build-b2lufs.ps1"
[ -s "$PS_BUILD" ] || die "Windows PowerShell derleme betigi yok: $PS_BUILD"
WIN_PS_BUILD="$(wslpath -w "$PS_BUILD")"
WIN_PROJECT_ROOT="$(wslpath -w "$HERE")"
WIN_OUT="$(wslpath -w "$OUT")"
WIN_KITS="$(wslpath -w "$KITS")"

if [ "${B2L_SKIP_MSVC_BUILD:-0}" = "1" ]; then
  [ -s "$OUT/b2lufs.sys" ] || die "B2L_SKIP_MSVC_BUILD=1 ama $OUT/b2lufs.sys yok"

  #
  # BAYAT IKILI KAPISI.
  #
  # B2L_SKIP_MSVC_BUILD=1 "derlemeyi atla" demek, "kaynaklar degismedi"
  # demek DEGIL. Bir tur bu ikisi karisti: MSVC derlemesi LNK2019 ile
  # patliyordu, kaynaklar duzeltildi, ama paketleme SKIP ile calistirilinca
  # derlemenin patladigi turdan ONCEKI eski .sys paketlendi. O paket
  # flaslansaydi iki ayri duzeltme de cihazda olmayacakti ve sonuc
  # "duzeltme ise yaramadi" diye okunacakti.
  #
  # Bu yuzden ikili, kaynaklardan yeni olmak ZORUNDA. Bilerek eski ikiliyle
  # paketlemek isteniyorsa B2L_ALLOW_STALE_SYS=1 ile acikca soylenir.
  #
  # INF sablonu SYS'e baglanmaz; asagida her paketleme turunda yeniden
  # kopyalanip Inf2Cat'e verilir. Bu nedenle yalniz gercek derleme girdileri
  # bayat SYS kapisini tetikler.
  YENI_KAYNAK="$(find "$SRC" -maxdepth 1 -newer "$OUT/b2lufs.sys" \
                   \( -name 'b2lufs_*.c' -o -name 'b2lufs_*.h' -o \
                      -name 'build-b2lufs.ps1' \) \
                   -printf '%f\n' 2>/dev/null || true)"
  if [ -n "$YENI_KAYNAK" ] && [ "${B2L_ALLOW_STALE_SYS:-0}" != "1" ]; then
    echo
    echo "  ###############################################################"
    echo "  # DUR: out/b2lufs/b2lufs.sys kaynaklardan ESKI."
    echo "  #"
    echo "  # Ondan yeni olan dosyalar:"
    printf '  #   %s\n' $YENI_KAYNAK
    echo "  #"
    echo "  # Bu haliyle paketlemek, yaptigin degisiklikleri ICERMEYEN bir"
    echo "  # surucuyu flaslamak olur ve sonucu yanlis okursun."
    echo "  #"
    echo "  # Derlemek icin:  bash tools_b2lufs.sh        (SKIP vermeden)"
    echo "  # Bilerek eskiyi paketlemek icin:"
    echo "  #   B2L_ALLOW_STALE_SYS=1 B2L_SKIP_MSVC_BUILD=1 bash tools_b2lufs.sh"
    echo "  ###############################################################"
    echo
    exit 1
  fi

  say "Var olan B2LUFS SYS kullaniliyor (B2L_SKIP_MSVC_BUILD=1)"
else
  say "B2LUFS derleniyor (yerel Windows MSVC ARM64 + WDK)"
  echo "  PowerShell, kaynaklari yerel Windows TEMP dizininde derleyecek"
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WIN_PS_BUILD" \
    -ProjectRoot "$WIN_PROJECT_ROOT" -OutDir "$WIN_OUT" \
    -KitsRoot "$WIN_KITS" -WdkVersion "$WDK_VERSION"
fi
say "Baglandi (MSVC link.exe; StorPort + ntoskrnl, ARM64)"
file "$OUT/b2lufs.sys" | sed 's/^/  /'

# Bu arac PhysicalDrive0'i yalniz GENERIC_READ ile acar ve dort GPT bolgesini
# X: RAM diskine kopyalar. Hepsi dogrulaninca Windows'un bolum onbellegini
# IOCTL_DISK_UPDATE_PROPERTIES ile yeniler; bu IOCTL medyaya yazmaz.
say "B2LGPTDUMP derleniyor (WSL clang/lld ARM64, salt-okunur)"
cat > "$OUT/kernel32-gptdump.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
CreateFileW
ReadFile
WriteFile
SetFilePointerEx
DeviceIoControl
GetStdHandle
CloseHandle
ExitProcess
DEF
llvm-dlltool -m arm64 -d "$OUT/kernel32-gptdump.def" \
  -l "$OUT/kernel32-gptdump.lib"
clang -target aarch64-windows-msvc -ffreestanding -fno-stack-protector \
  -mno-stack-arg-probe -O1 -Wall -Wextra -Werror \
  -c "$SRC/b2lgptdump.c" -o "$OUT/b2lgptdump.obj"
lld-link /subsystem:console /entry:mainCRTStartup /machine:arm64 \
  /nodefaultlib /out:"$OUT/b2lgptdump.exe" \
  "$OUT/b2lgptdump.obj" "$OUT/kernel32-gptdump.lib"
file "$OUT/b2lgptdump.exe" | sed 's/^/  /'

# b2lfb ile AYNI sertifika: cihazda zaten guvenilen zinciri degistirmeyiz.
CERT="$HERE/windows/b2lfb/b2l-test.pem"
KEY="$HERE/windows/b2lfb/b2l-test.key"
if [ ! -s "$CERT" ] || [ ! -s "$KEY" ]; then
  say "Mevcut b2lfb test sertifikasi uretiliyor (bir kereye mahsus)"
  # CN cihazdan bagimsiz; bkz. tools_b2lfb.sh.
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$KEY" -out "$CERT" \
    -subj "/CN=beyond2lte test signing" 2>/dev/null
fi

say "b2lufs.sys imzalaniyor"
osslsigncode sign -certs "$CERT" -key "$KEY" \
  -n "${DEV} Exynos UFS StorPort LUN 0" -h sha256 \
  -in "$OUT/b2lufs.sys" -out "$OUT/b2lufs-signed.sys" >/dev/null
mv -f "$OUT/b2lufs-signed.sys" "$OUT/b2lufs.sys"

# INF ve SYS ayni klasorde olmali ki Inf2Cat tum hash'leri pakete katsin.
cp "$SRC/b2lufs.inf.in" "$OUT/b2lufs.inf"

say "PnP katalogu uretiliyor ve ayni sertifikayla imzalaniyor"
PFX="$OUT/b2l-test.pfx"
cleanup() {
  rm -f "$PFX"
  [ -z "${CHECK:-}" ] || rm -rf "$CHECK"
}
trap cleanup EXIT

# Hedef BCD'sinde testsigning acik ve bu paket b2lfb ile ayni test
# sertifikasini kullanir. Derleme bilgisayarinin Trusted Root deposuna bu
# sertifika bilerek eklenmediginde SignTool, imza/katalog yapisi dogru olsa
# bile yalniz zincir-guven uyarisi ile 1 doner. /pa ve /c denetimlerini yine
# calistiririz; sadece bu kesin test-koku durumunu paketleme hatasi saymayiz.
# Diger her SignTool hatasi (bozuk imza, katalogda SYS yok, vb.) oldugu gibi
# derlemeyi durdurur.
verify_test_signed_package() {
  local Label="$1"
  shift
  local VerifyLog
  VerifyLog="$(mktemp "${TMPDIR:-/tmp}/b2lufs-signtool.XXXXXX")" || \
    die "SignTool dogrulama gunlugu olusturulamadi"

  if "$SIGNTOOL" verify "$@" >"$VerifyLog" 2>&1; then
    printf '  dogrulandi: %s\n' "$Label"
  elif grep -Fqi 'certificate chain processed, but terminated in a root' "$VerifyLog" && \
       grep -Fqi 'certificate which is not trusted by the trust provider' "$VerifyLog"; then
    cat "$VerifyLog"
    printf '  test imza: %s; build bilgisayari test kokune guvenmiyor (beklenen)\n' \
      "$Label"
  else
    cat "$VerifyLog" >&2
    rm -f "$VerifyLog"
    die "SignTool dogrulamasi basarisiz: $Label"
  fi

  rm -f "$VerifyLog"
}

rm -f "$OUT/b2lufs.cat" "$OUT/b2lufs-signed.cat" "$PFX"
"$INF2CAT" /driver:"$(wslpath -w "$OUT")" /os:10_GE_ARM64
[ -s "$OUT/b2lufs.cat" ] || die "Inf2Cat b2lufs.cat uretmedi"

# signtool Windows PFX bekler. PFX gecicidir; anahtar depoda tutulmaz ve
# b2lfb'nin PEM zincirinden bu tur icin uretilir.
PFX_PASS="b2lufs-lun0"
openssl pkcs12 -export -out "$PFX" \
  -inkey "$KEY" -in "$CERT" -passout "pass:$PFX_PASS" >/dev/null 2>&1
"$SIGNTOOL" sign /v /fd sha256 /f "$(wslpath -w "$PFX")" \
  /p "$PFX_PASS" "$(wslpath -w "$OUT/b2lufs.cat")"
verify_test_signed_package "b2lufs.cat" /v /pa "$(wslpath -w "$OUT/b2lufs.cat")"
verify_test_signed_package "b2lufs.sys katalog eslesmesi" /v /pa /c \
  "$(wslpath -w "$OUT/b2lufs.cat")" "$(wslpath -w "$OUT/b2lufs.sys")"
rm -f "$PFX"

# Bir sonraki katman (ESP sparse imaji) WIM'i artik yalniz var/yok ile
# degil, bu tam paket kimligi ile kontrol eder. Bu dosya imzali katalogun
# yerine gecmez; katalog INF+SYS imzasini tasir. Ama bir onceki WIM'in veya
# yanlis Odin arsivinin secilmesini, cihazdaki startnet'ten gorunur ve
# paketleme asamasinda kesin bir hata haline getirir.
MANIFEST="$OUT/b2lufs-manifest.txt"
{
  echo "B2LUFS_PACKAGE=storport-cache-online-write-report"
  echo "B2LUFS_INF_SHA256=$(sha256sum "$OUT/b2lufs.inf" | awk '{print $1}')"
  echo "B2LUFS_SYS_SHA256=$(sha256sum "$OUT/b2lufs.sys" | awk '{print $1}')"
  echo "B2LUFS_CAT_SHA256=$(sha256sum "$OUT/b2lufs.cat" | awk '{print $1}')"
  echo "B2LUFS_GPTDUMP_SHA256=$(sha256sum "$OUT/b2lgptdump.exe" | awk '{print $1}')"
} > "$MANIFEST"

say "B2LUFS paketi boot.wim'e ekleniyor"
for Item in b2lufs.inf b2lufs.sys b2lufs.cat; do
  wimlib-imagex update "$WIM" 1 \
    --command="delete --force /Windows/B2LUFS/$Item" >/dev/null 2>&1 || true
  wimlib-imagex update "$WIM" 1 \
    --command="add '$OUT/$Item' /Windows/B2LUFS/$Item" >/dev/null
  echo "  /Windows/B2LUFS/$Item"
done
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/B2LUFS/manifest.txt" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$MANIFEST' /Windows/B2LUFS/manifest.txt" >/dev/null
echo "  /Windows/B2LUFS/manifest.txt"

wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/b2lgptdump.exe" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lgptdump.exe' /Windows/System32/b2lgptdump.exe" >/dev/null
echo "  /Windows/System32/b2lgptdump.exe"

# Manuel ham-okuma araci WIM'de kalsin; otomatik akista cagrilmaz. Bu script
# startnet'i de kaynakla birebir yeniler. Bu sayede eski, ayrintili tani
# startnet'inin WIM'de kalmasi mumkun degil.
SNTMP="$OUT/startnet.cmd"
sed 's/\r$//; s/$/\r/' "$HERE/windows/startnet.cmd" > "$SNTMP"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/startnet.cmd" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$SNTMP' /Windows/System32/startnet.cmd" >/dev/null

CHECK="$(mktemp -d)"
for Item in b2lufs.inf b2lufs.sys b2lufs.cat; do
  wimlib-imagex extract "$WIM" 1 "/Windows/B2LUFS/$Item" \
    --dest-dir="$CHECK" --no-acls >/dev/null
  WANT="$(sha256sum "$OUT/$Item" | awk '{print $1}')"
  GOT="$(sha256sum "$CHECK/$Item" | awk '{print $1}')"
  [ "$WANT" = "$GOT" ] || die "$Item WIM geri okuma hash'i farkli; flaslama"
  echo "  dogrulandi: $Item SHA256=${WANT:0:16}"
done
wimlib-imagex extract "$WIM" 1 /Windows/B2LUFS/manifest.txt \
  --dest-dir="$CHECK" --no-acls >/dev/null
[ "$(sha256sum "$MANIFEST" | awk '{print $1}')" = \
  "$(sha256sum "$CHECK/manifest.txt" | awk '{print $1}')" ] || \
  die "B2LUFS manifest WIM geri okuma hash'i farkli; flaslama"
echo "  dogrulandi: B2LUFS manifest $(grep '^B2LUFS_SYS_SHA256=' "$MANIFEST" | cut -c1-34)"
wimlib-imagex extract "$WIM" 1 /Windows/System32/b2lgptdump.exe \
  --dest-dir="$CHECK" --no-acls >/dev/null
[ "$(sha256sum "$OUT/b2lgptdump.exe" | awk '{print $1}')" = \
  "$(sha256sum "$CHECK/b2lgptdump.exe" | awk '{print $1}')" ] || \
  die "b2lgptdump.exe WIM geri okuma hash'i farkli; flaslama"
echo "  dogrulandi: b2lgptdump.exe ARM64 ham-okuma araci"
wimlib-imagex extract "$WIM" 1 /Windows/System32/startnet.cmd \
  --dest-dir="$CHECK" --no-acls >/dev/null
[ "$(sha256sum "$SNTMP" | awk '{print $1}')" = "$(sha256sum "$CHECK/startnet.cmd" | awk '{print $1}')" ] || \
  die "startnet.cmd WIM geri okuma hash'i farkli; flaslama"
echo "  dogrulandi: startnet.cmd B2LUFS PnP kurulumunu iceriyor"

say "B2LUFS hazir"
echo "Bu imajda beklenen sonuc: CACHE W: olarak baglanir, test dosyasi yazilir ve geri okunur."
echo "Sonraki paketleme: bash cache_esp.sh --win --system && bash cache_esp.sh --win --bos"
