#!/usr/bin/env bash
#
# b2lfb olcum surucusunu, b2lsg0 sirali-sistem, b2lsys grupsuz-sistem ve
# b2lauto SCM-damga suruculerini WSL'de derler, imzalar ve boot.wim'e
# yerlestirir.
#
#   bash tools_b2lfb.sh
#
# ---------------------------------------------------------------------------
# NEDEN VISUAL STUDIO / WDK YOK
# ---------------------------------------------------------------------------
# Bir Windows cekirdek surucusu, ozunde /SUBSYSTEM:NATIVE bir PE dosyasi.
# WDK'nin sagladigi sey basliklar ve ntoskrnl icin bir import kitapligi.
# Suruculerimiz ntoskrnl'den az sayida fonksiyon kullaniyor; import
# kitapligini elde uretmek birkac satir:
#
#   - basliklar  -> b2lfb.c icinde elle bildirildi
#   - import lib -> llvm-dlltool ile .def'ten uretiliyor
#
# Boylece butun zincir WSL'de kaliyor: Windows makinesine gecip proje acmak,
# derleme ayari kurcalamak yok. Her tur bir Odin flasina mal oldugu icin
# gelistirme dongusunu tek makinede tutmak onemli.
#
# ---------------------------------------------------------------------------
# IMZA
# ---------------------------------------------------------------------------
# ARM64 Windows cekirdek modu imza istiyor. Bunu bir kez gorduk:
#   ws2ifsl.sys / 0xc0000428 / STATUS_INVALID_IMAGE_HASH
#
# Kendi sertifikamizla imzaliyoruz (osslsigncode) ve BCD'de "testsigning on"
# aciyoruz. Secure Boot'un zorlamadigini OLCTUK: imzasiz EBSTEST.EFI cihazda
# calisti ve bootmgfw yuklenirken "[Security] 3rd party image can be loaded"
# satiri gecti.
#
# ACIK KALAN: test imzali bir ACILIS surucusunun sertifikasinin WinPE'nin
# guven deposunda da bulunmasi gerekebilir. Gerekirse cihaz bunu 0xc0000428
# ile SOYLEYECEK - yani basarisizlik da okunabilir bir sonuc. Sessizlikten
# iyi.
#
set -euo pipefail

#
# ARGUMAN: kapat / ac / kontrol
#
# NEDEN GEREKTI: b2lfb korumali listede, cunku olcum aracini yanlislikla
# kaybetmemek icin oyle yapildi. Ama "aletsiz taban" deneyi tam olarak
# onu kapatmayi gerektiriyor: uc UEFI tani surucusunu cikardik ve TIK
# degismedi (98), geriye sinanmamis tek alet b2lfb kaldi.
#
# Kapali haldeyken sayi okunamaz, olcum kronometreyle yapilir. Amac zaten
# sayi degil: sifirlama VAR MI ve kabaca ne zaman.
#
# "kontrol" grupsuz b2lsys damga surucusunu bir tur icin Start=0 yapar.
# WIM/hive geri okumasi dosyalarin KONULDUGUNU kanitlar, fakat Start=1
# asamasinda gercekten yuklenebildigini kanitlamaz. Bu kontrol o boslugu
# kapatir: b2lsys boot surucusu olarak calisirsa, bir sonraki normal
# Start=1 sonucunu faz siniri olarak yorumlamak mumkun olur.
#
# b2lsg0 KONTROLDE KAPALIDIR. b2lfb ile ayni LoadOrderGroup'tadir ve ikisi
# arasinda Tag olmadigindan b2lsg0 once calisip SYG=INI yazsa bile b2lfb
# sonra SYG=REG ile ustune yazabilir. Bu nedenle SYG, bu kontrol kipinde
# negatif yuklenebilirlik kaniti olamaz. ErrorControl=3 sayesinde b2lsys
# icin imza/yukleme reddi de sessizce atlanmaz.
#
B2L_MOD="kur"
DEV="${B2L_DEVICE:-beyond2lte}"
while [ $# -gt 0 ]; do
  case "$1" in
    # Cihaz yalnizca imza aciklamalarini ve masaustu klasorunu etkiler.
    -d|--device) DEV="${2:?-d icin cihaz adi gerekli}"; shift ;;
    kur|kapat|ac|kontrol) B2L_MOD="$1" ;;
    *) echo "Kullanim: bash $0 [-d cihaz] [kur|kapat|ac|kontrol]"; exit 1 ;;
  esac
  shift
done
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/windows/b2lfb"
WIM="$HERE/esp/sources/boot.wim"
OUT="$HERE/out/b2lfb"; mkdir -p "$OUT"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

for t in clang lld-link llvm-dlltool; do
  command -v $t >/dev/null || { echo "$t yok -> sudo apt install -y clang lld llvm"; exit 1; }
done
command -v osslsigncode >/dev/null || { echo "osslsigncode yok -> sudo apt install -y osslsigncode"; exit 1; }
command -v openssl      >/dev/null || { echo "openssl yok -> sudo apt install -y openssl"; exit 1; }

#
# 1) ntoskrnl import kitapligi
#
# X18 AYRILIYOR - ACIKCA.
#
# Windows ARM64'te x18 platform yazmaci: cekirdek modunda HER ZAMAN o anki
# KPCR yapisini gosteriyor (ARM64 Windows internals). Onu bozan kod, cekirdek
# baglaminda cagrilan her seyi patlatir.
#
# Clang'in aarch64-windows-msvc hedefi x18'i zaten ayiriyor olmali, ama bunu
# hic dogrulamadik ve iki kez KMODE_EXCEPTION_NOT_HANDLED aldik. Acikca
# yazmak bedava; varsayimi ortadan kaldiriyor.
#
# lld-link "MmMapIoSpace nerede" diye soracak. Cevabi bir .def dosyasi:
# fonksiyon adlari ve hangi DLL'de olduklari. llvm-dlltool bundan gercek
# bir .lib uretiyor; ntoskrnl.exe'nin kendisine ihtiyac yok.
#
if [ "$B2L_MOD" != "kur" ]; then
  case "$B2L_MOD" in
    kapat)
      DEG=4; SYSG_DEG=4; SYS_DEG=4; AUTO_DEG=4
      SYSG_ERR=0; SYS_ERR=0
      ;;
    ac)
      DEG=0; SYSG_DEG=1; SYS_DEG=1; AUTO_DEG=2
      SYSG_ERR=0; SYS_ERR=0
      ;;
    kontrol)
      DEG=0; SYSG_DEG=4; SYS_DEG=0; AUTO_DEG=4
      SYSG_ERR=0; SYS_ERR=3
      ;;
  esac

  if [ "$B2L_MOD" = "kontrol" ]; then
    say "KONTROL: b2lsys Start=0 ErrorControl=3, b2lsg0/b2lauto Start=4 yapiliyor"
  else
    say "b2lfb Start=$DEG, b2lsg0/b2lsys Start=$SYSG_DEG/$SYS_DEG, b2lauto Start=$AUTO_DEG yapiliyor"
  fi
  HW="$(mktemp -d)"; trap 'rm -rf "$HW"' EXIT
  wimlib-imagex extract "$WIM" 1 /Windows/System32/config/SYSTEM \
    --dest-dir="$HW" --no-acls >/dev/null

  printf '[HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet001\\Services\\b2lfb]\n"Start"=dword:0000000%s\n\n[HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet001\\Services\\b2lsg0]\n"Start"=dword:0000000%s\n"ErrorControl"=dword:0000000%s\n\n[HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet001\\Services\\b2lsys]\n"Start"=dword:0000000%s\n"ErrorControl"=dword:0000000%s\n\n[HKEY_LOCAL_MACHINE\\SYSTEM\\ControlSet001\\Services\\b2lauto]\n"Start"=dword:0000000%s\n' \
    "$DEG" "$SYSG_DEG" "$SYSG_ERR" "$SYS_DEG" "$SYS_ERR" "$AUTO_DEG" > "$HW/yaz.reg"
  hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' "$HW/SYSTEM" "$HW/yaz.reg"

  V="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lfb" Start 2>/dev/null || echo '?')"
  VG="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lsg0" Start 2>/dev/null || echo '?')"
  EG="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lsg0" ErrorControl 2>/dev/null || echo '?')"
  VS="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lsys" Start 2>/dev/null || echo '?')"
  ES="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lsys" ErrorControl 2>/dev/null || echo '?')"
  VA="$(hivexget "$HW/SYSTEM" "ControlSet001\\Services\\b2lauto" Start 2>/dev/null || echo '?')"
  [ "$V" = "$DEG" ] || { echo "  ! b2lfb geri okuma $V, beklenen $DEG"; exit 1; }
  [ "$VG" = "$SYSG_DEG" ] || { echo "  ! b2lsg0 geri okuma $VG, beklenen $SYSG_DEG"; exit 1; }
  [ "$EG" = "$SYSG_ERR" ] || { echo "  ! b2lsg0 ErrorControl geri okuma $EG, beklenen $SYSG_ERR"; exit 1; }
  [ "$VS" = "$SYS_DEG" ] || { echo "  ! b2lsys geri okuma $VS, beklenen $SYS_DEG"; exit 1; }
  [ "$ES" = "$SYS_ERR" ] || { echo "  ! b2lsys ErrorControl geri okuma $ES, beklenen $SYS_ERR"; exit 1; }
  [ "$VA" = "$AUTO_DEG" ] || { echo "  ! b2lauto geri okuma $VA, beklenen $AUTO_DEG"; exit 1; }
  echo "  dogrulandi: b2lfb Start=$V, b2lsg0 Start=$VG ErrorControl=$EG, b2lsys Start=$VS ErrorControl=$ES, b2lauto Start=$VA"

  wimlib-imagex update "$WIM" 1 \
    --command="delete --force /Windows/System32/config/SYSTEM" >/dev/null 2>&1 || true
  wimlib-imagex update "$WIM" 1 \
    --command="add '$HW/SYSTEM' /Windows/System32/config/SYSTEM" >/dev/null
  echo
  if [ "$B2L_MOD" = "kapat" ]; then
    echo "Temiz taban icin: bash cache_esp.sh --win --system --baseline"
  elif [ "$B2L_MOD" = "kontrol" ]; then
    echo "Simdi: bash cache_esp.sh --win --system"
    echo "Kontrol sonrasi normal deneye donmek icin: bash tools_b2lfb.sh"
  else
    echo "Simdi: bash cache_esp.sh --win --system"
  fi
  exit 0
fi

say "ntoskrnl import kitapligi uretiliyor"
cat > "$OUT/ntoskrnl.def" <<'DEF'
LIBRARY ntoskrnl.exe
EXPORTS
MmMapIoSpace
MmUnmapIoSpace
KeRegisterBugCheckCallback
PsCreateSystemThread
KeGetCurrentThread
KeSetPriorityThread
KeInitializeDpc
KeInitializeTimer
KeSetTimerEx
KeQueryPerformanceCounter
IoRegisterBootDriverReinitialization
ZwOpenEvent
ZwCreateFile
ZwWriteFile
ZwClose
DEF
llvm-dlltool -m arm64 -d "$OUT/ntoskrnl.def" -l "$OUT/ntoskrnl.lib"
echo "  $(basename "$OUT/ntoskrnl.lib") hazir"

cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
CreateEventW
CloseHandle
Sleep
ExitProcess
DEF
llvm-dlltool -m arm64 -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"
echo "  $(basename "$OUT/kernel32.lib") hazir"

#
# 2) Derleme
#
# -ffreestanding : C kutuphanesi yok, cekirdekte zaten olmaz
# -fno-stack-protector : __security_cookie sembolunu istemesin
# -mno-stack-arg-probe : __chkstk istemesin
# -ffixed-x18 : x18 PLATFORM YAZMACI, cekirdek modunda KPCR'yi gosteriyor
# Ucu de "linkte cozulemeyen sembol" hatasini onlemek icin; WDK olmadan
# derlerken en sik carpilan uc tas bunlar.
#
say "Derleniyor (aarch64-windows-msvc)"
clang -target aarch64-windows-msvc \
      -ffreestanding -fno-stack-protector -mno-stack-arg-probe -ffixed-x18 \
      -O1 -Wall -Wextra \
      -c "$SRC/b2lfb.c" -o "$OUT/b2lfb.obj"
echo "  b2lfb.obj: $(stat -c%s "$OUT/b2lfb.obj") bayt"
clang -target aarch64-windows-msvc \
      -ffreestanding -fno-stack-protector -mno-stack-arg-probe -ffixed-x18 \
      -O1 -Wall -Wextra \
      -c "$SRC/b2lsg0.c" -o "$OUT/b2lsg0.obj"
echo "  b2lsg0.obj: $(stat -c%s "$OUT/b2lsg0.obj") bayt"
clang -target aarch64-windows-msvc \
      -ffreestanding -fno-stack-protector -mno-stack-arg-probe -ffixed-x18 \
      -O1 -Wall -Wextra \
      -c "$SRC/b2lsys.c" -o "$OUT/b2lsys.obj"
echo "  b2lsys.obj: $(stat -c%s "$OUT/b2lsys.obj") bayt"
clang -target aarch64-windows-msvc \
      -ffreestanding -fno-stack-protector -mno-stack-arg-probe -ffixed-x18 \
      -O1 -Wall -Wextra \
      -c "$SRC/b2lauto.c" -o "$OUT/b2lauto.obj"
echo "  b2lauto.obj: $(stat -c%s "$OUT/b2lauto.obj") bayt"
clang -target aarch64-windows-msvc \
      -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
      -O1 -Wall -Wextra \
      -c "$SRC/b2lcmd.c" -o "$OUT/b2lcmd.obj"
echo "  b2lcmd.obj: $(stat -c%s "$OUT/b2lcmd.obj") bayt"

#
# 3) Baglama
#
# /driver      : cekirdek surucusu bayraklari
# /subsystem:native : cekirdek modu PE
# /entry:DriverEntry : varsayilan giris noktasi degil, bizimki
#
say "Baglaniyor"
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 \
         /nodefaultlib \
         /out:"$OUT/b2lfb.sys" "$OUT/b2lfb.obj" "$OUT/ntoskrnl.lib"
echo "  b2lfb.sys: $(stat -c%s "$OUT/b2lfb.sys") bayt"
file "$OUT/b2lfb.sys" | sed 's/^/  /'
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 \
         /nodefaultlib \
         /out:"$OUT/b2lsg0.sys" "$OUT/b2lsg0.obj" "$OUT/ntoskrnl.lib"
echo "  b2lsg0.sys: $(stat -c%s "$OUT/b2lsg0.sys") bayt"
file "$OUT/b2lsg0.sys" | sed 's/^/  /'
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 \
         /nodefaultlib \
         /out:"$OUT/b2lsys.sys" "$OUT/b2lsys.obj" "$OUT/ntoskrnl.lib"
echo "  b2lsys.sys: $(stat -c%s "$OUT/b2lsys.sys") bayt"
file "$OUT/b2lsys.sys" | sed 's/^/  /'
lld-link /driver /subsystem:native /entry:DriverEntry /machine:arm64 \
         /nodefaultlib \
         /out:"$OUT/b2lauto.sys" "$OUT/b2lauto.obj" "$OUT/ntoskrnl.lib"
echo "  b2lauto.sys: $(stat -c%s "$OUT/b2lauto.sys") bayt"
file "$OUT/b2lauto.sys" | sed 's/^/  /'
lld-link /subsystem:console /entry:mainCRTStartup /machine:arm64 \
         /nodefaultlib \
         /out:"$OUT/b2lcmd.exe" "$OUT/b2lcmd.obj" "$OUT/kernel32.lib"
echo "  b2lcmd.exe: $(stat -c%s "$OUT/b2lcmd.exe") bayt"
file "$OUT/b2lcmd.exe" | sed 's/^/  /'

#
# 4) Sertifika (bir kez uretiliyor, sonra tekrar kullaniliyor)
#
# Sertifika DEGISIRSE cihazdaki guven deposuna eklenen kopya gecersiz kalir,
# o yuzden var olani koruyoruz.
#
CERT="$HERE/windows/b2lfb/b2l-test.pem"
KEY="$HERE/windows/b2lfb/b2l-test.key"
if [ ! -s "$CERT" ] || [ ! -s "$KEY" ]; then
  say "Test sertifikasi uretiliyor (bir kereye mahsus)"
  # CN bilerek CIHAZDAN BAGIMSIZ: ayni kok sertifika iki cihazin da guven
  # deposuna bir kez eklenir. Imza ACIKLAMALARI cihazi belirtir (${DEV}), CN
  # degismez.
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$KEY" -out "$CERT" \
    -subj "/CN=beyond2lte test signing" 2>/dev/null
  echo "  uretildi: $(basename "$CERT")"
else
  echo
  echo "  mevcut sertifika kullaniliyor: $(basename "$CERT")"
fi

say "Imzalaniyor"
osslsigncode sign -certs "$CERT" -key "$KEY" \
  -n "${DEV} framebuffer" -h sha256 \
  -in "$OUT/b2lfb.sys" -out "$OUT/b2lfb-signed.sys" 2>&1 | sed 's/^/  /'
mv -f "$OUT/b2lfb-signed.sys" "$OUT/b2lfb.sys"
osslsigncode sign -certs "$CERT" -key "$KEY" \
  -n "${DEV} system-group checkpoint" -h sha256 \
  -in "$OUT/b2lsg0.sys" -out "$OUT/b2lsg0-signed.sys" 2>&1 | sed 's/^/  /'
mv -f "$OUT/b2lsg0-signed.sys" "$OUT/b2lsg0.sys"
osslsigncode sign -certs "$CERT" -key "$KEY" \
  -n "${DEV} system-start checkpoint" -h sha256 \
  -in "$OUT/b2lsys.sys" -out "$OUT/b2lsys-signed.sys" 2>&1 | sed 's/^/  /'
mv -f "$OUT/b2lsys-signed.sys" "$OUT/b2lsys.sys"
osslsigncode sign -certs "$CERT" -key "$KEY" \
  -n "${DEV} SCM checkpoint" -h sha256 \
  -in "$OUT/b2lauto.sys" -out "$OUT/b2lauto-signed.sys" 2>&1 | sed 's/^/  /'
mv -f "$OUT/b2lauto-signed.sys" "$OUT/b2lauto.sys"

#
# 5) WIM'e yerlestirme
#
[ -s "$WIM" ] || { echo; echo "boot.wim yok - once tools_winboot.sh calistir."; exit 1; }
command -v wimlib-imagex >/dev/null || { echo "wimlib yok"; exit 1; }
command -v hivexregedit  >/dev/null || { echo "hivexregedit yok -> sudo apt install -y libwin-hivex-perl"; exit 1; }

#
# ONCE SIL, SONRA EKLE, SONRA GERI OKU
#
# BU BIR TURA MAL OLDU. Surucu guncellendi, "TIK=..." kaydi eklendi, ama
# cihazda hala yalnizca "B2LF" gorunuyordu - yani ESKI surucu calisiyordu.
#
# Sebep: "add" hedefte ayni isimde bir dosya varken onu degistirmeyebiliyor.
# Script ciktiyi /dev/null'a atiyordu, dolayisiyla hicbir uyari gorunmedi ve
# her sey basarili sanildi.
#
# Ayni sinifin ucuncu hatasi:
#   1. hivexsh setval dugumdeki diger degerleri siliyordu
#   2. .reg icindeki ters bolukler kacis olarak yutuluyordu
#   3. simdi bu
# Ucunde de ortak ders: YAZDIGINI GERI OKU.
#
say "boot.wim'e ekleniyor"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/drivers/b2lfb.sys" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lfb.sys' /Windows/System32/drivers/b2lfb.sys" >/dev/null
echo "  /Windows/System32/drivers/b2lfb.sys"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/drivers/b2lsg0.sys" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lsg0.sys' /Windows/System32/drivers/b2lsg0.sys" >/dev/null
echo "  /Windows/System32/drivers/b2lsg0.sys"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/drivers/b2lsys.sys" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lsys.sys' /Windows/System32/drivers/b2lsys.sys" >/dev/null
echo "  /Windows/System32/drivers/b2lsys.sys"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/drivers/b2lauto.sys" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lauto.sys' /Windows/System32/drivers/b2lauto.sys" >/dev/null
echo "  /Windows/System32/drivers/b2lauto.sys"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/b2lcmd.exe" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$OUT/b2lcmd.exe' /Windows/System32/b2lcmd.exe" >/dev/null
echo "  /Windows/System32/b2lcmd.exe"

#
# b2lcmd.exe'nin tek cagrildigi yer startnet.cmd'dir. Bu ikisini ayri
# yerlestirip startnet'i eski bir kopyada birakmak, WIM'de dogru EXE varken
# kullanici-modu damgasinin hic yazilmamasi demek olur. CRLF'ye cevir,
# once sil, sonra ekle ve hash'ini geri oku.
#
SNTMP="$OUT/startnet.cmd"
sed 's/\r$//; s/$/\r/' "$HERE/windows/startnet.cmd" > "$SNTMP"
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/startnet.cmd" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$SNTMP' /Windows/System32/startnet.cmd" >/dev/null
CK="$OUT/dogrulama-startnet"; rm -rf "$CK"; mkdir -p "$CK"
if wimlib-imagex extract "$WIM" 1 /Windows/System32/startnet.cmd \
     --dest-dir="$CK" --no-acls >/dev/null 2>&1; then
  A="$(sha1sum "$SNTMP" | cut -c1-16)"
  B="$(sha1sum "$CK/startnet.cmd" | cut -c1-16)"
  [ "$A" = "$B" ] || {
    echo "  ! startnet.cmd WIM'de farkli ($A != $B); flaslama."; exit 1;
  }
  echo "  dogrulandi: startnet.cmd b2lcmd ile ayni ($A)"
else
  echo "  ! startnet.cmd WIM'den geri okunamadi - dogrulanamiyor"; exit 1
fi
rm -rf "$CK"

for ARTEFAKT in b2lfb.sys b2lsg0.sys b2lsys.sys b2lauto.sys b2lcmd.exe; do
  CK="$OUT/dogrulama-$ARTEFAKT"; rm -rf "$CK"; mkdir -p "$CK"
  if [[ "$ARTEFAKT" = *.sys ]]; then WIM_YOL="/Windows/System32/drivers/$ARTEFAKT"; else WIM_YOL="/Windows/System32/$ARTEFAKT"; fi
  if wimlib-imagex extract "$WIM" 1 "$WIM_YOL" \
       --dest-dir="$CK" --no-acls >/dev/null 2>&1; then
    A="$(sha1sum "$OUT/$ARTEFAKT" | cut -c1-16)"
    B="$(sha1sum "$CK/$ARTEFAKT"  | cut -c1-16)"
    if [ "$A" = "$B" ]; then
      echo "  dogrulandi: $ARTEFAKT WIM'de derledigimizle AYNI ($A)"
    else
      echo
      echo "  ###############################################################"
      echo "  # WIM'DEKI $ARTEFAKT FARKLI - eski surum kalmis."
      echo "  #   derlenen: $A"
      echo "  #   WIM'deki: $B"
      echo "  # Flaslamak bosuna; cihaza eski kod gider."
      echo "  ###############################################################"
      exit 1
    fi
  else
    echo "  ! $ARTEFAKT WIM'den geri okunamadi - dogrulanamiyor"
    exit 1
  fi
  rm -rf "$CK"
done

#
# 6) Servis kaydi - ACILIS, SIRALI/GRUPSUZ SISTEM VE SCM SURUCULERI
#
# Start=0  BOOT   : winload yukluyor. BLOKAJ MERDIVENI BITTI, en erken
#                   yuklemeye geri donuldu ki bant her acilista ciksin ve
#                   serit sayimi bastan islesin.
# Start=1  SYSTEM : PnP'den sonra, kalan sistem-start suruculer yuklenir.
#                   b2lsg0 sirali bir grupta, b2lsys grupsuz; ikisi
#                   grup sirasindan kaynaklanan belirsizligi ayirir.
# Start=2  AUTO   : Start=0/1 ve PnP sonrasinda Service Control Manager
#                   tarafindan baslatilir. b2lauto'nun tek isi bu siniri
#                   0xCF030000+a0'a damgalamaktir.
# Type=1          : cekirdek surucusu
# Group           : "System Bus Extender" - en erken gruplardan biri.
#
# ErrorControl=3 (CRITICAL) - ILK SURUMDE 0 IDI VE BU BIR TASARIM HATASIYDI.
#
# "Surucu patlarsa acilis devam etsin, yeni ariza kaynagi yaratmayalim" diye
# 0 (IGNORE) yazmistim. Sonucu: imza reddedilirse winload surucuyu SESSIZCE
# atliyor. Yani elimizdeki EN BILGILENDIRICI arizayi - ws2ifsl denemesinde
# gordugumuz "0xc0000428 STATUS_INVALID_IMAGE_HASH" ekranini - kendi elimle
# sessizlige cevirmisim. Ilk denemede ne macenta ne RAM imzasi cikmasinin
# en olasi sebebi bu.
#
# 3 ile tam tersi oluyor: surucu yuklenemezse winload DURUYOR ve sebebi
# ekrana yaziyor. Acilis olmuyor ama bu turda acilis zaten hedef degil;
# hedef surucunun yuklenip yuklenmedigini OGRENMEK.
#
#   Macenta ekran   -> surucu yuklendi ve calisti
#   Hata ekrani     -> surucu yuklenemedi, SEBEBI YAZIYOR
#   Ikisi de yok    -> winload acilis surucilerine hic gelmiyor
#
# Uc okumanin ucu de bilgi. Onceki surumde ikisi ayni goruntuyu veriyordu.
#
# hivexsh'in setval'i KULLANILMIYOR: o komut dugumdeki diger butun degerleri
# siliyor ve bu daha once bes olcumu gecersiz kilmisti. hivexregedit --merge
# yalnizca verilen degerleri yaziyor.
#
say "SYSTEM hive'ina boot, sirali/grupsuz system-start ve SCM servisleri yaziliyor"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
wimlib-imagex extract "$WIM" 1 /Windows/System32/config/SYSTEM \
  --dest-dir="$W" --no-acls >/dev/null

cat > "$W/b2lfb.reg" <<'REG'
[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Services\b2lfb]
"Type"=dword:00000001
"Start"=dword:00000000
"ErrorControl"=dword:00000003
"ImagePath"=str(2):"\\SystemRoot\\System32\\drivers\\b2lfb.sys"
"Group"="System Bus Extender"
"DisplayName"="beyond2lte framebuffer"

[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Services\b2lsg0]
"Type"=dword:00000001
"Start"=dword:00000001
"ErrorControl"=dword:00000000
"Group"="System Bus Extender"
"ImagePath"=str(2):"\\SystemRoot\\System32\\drivers\\b2lsg0.sys"
"DisplayName"="beyond2lte system-group checkpoint"

[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Services\b2lsys]
"Type"=dword:00000001
"Start"=dword:00000001
"ErrorControl"=dword:00000000
"ImagePath"=str(2):"\\SystemRoot\\System32\\drivers\\b2lsys.sys"
"DisplayName"="beyond2lte system-start checkpoint"

[HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Services\b2lauto]
"Type"=dword:00000001
"Start"=dword:00000002
"ErrorControl"=dword:00000000
"ImagePath"=str(2):"\\SystemRoot\\System32\\drivers\\b2lauto.sys"
"DisplayName"="beyond2lte SCM checkpoint"
REG
# Heredoc tirnakli oldugu icin ${DEV} genisletilmedi; cihaz adini burada yaz.
sed -i "s/beyond2lte/${DEV}/g" "$W/b2lfb.reg"
hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' "$W/SYSTEM" "$W/b2lfb.reg"

#
# IMAGEPATH GERI OKUNUYOR - BU KONTROL BIR TURA MAL OLDU
#
# Ilk surumde .reg icinde "\SystemRoot\System32\drivers\b2lfb.sys" yaziyordu.
# .reg biciminde ters boluk KACIS KARAKTERI; hive'a
# "SystemRootSystem32driversb2lfb.sys" olarak girdi ve cihaz sunu bastı:
#
#   File: \windows\SystemRootSystem32driversb2lfb.sys
#   Status: 0xc000000f  (dosya bulunamadi)
#
# Yaziyi yazip geri okumamak, yazdigini varsaymaktir. Artik okuyoruz: ters
# boluk sayisi ucten azsa yol bozulmus demektir ve script oracikta duruyor -
# bir Odin flasi harcanmadan.
#
YOL="$(hivexget "$W/SYSTEM" 'ControlSet001\Services\b2lfb' ImagePath 2>/dev/null || true)"
YOL_GRP="$(hivexget "$W/SYSTEM" 'ControlSet001\Services\b2lsg0' ImagePath 2>/dev/null || true)"
YOL_SYS="$(hivexget "$W/SYSTEM" 'ControlSet001\Services\b2lsys' ImagePath 2>/dev/null || true)"
YOL_AUTO="$(hivexget "$W/SYSTEM" 'ControlSet001\Services\b2lauto' ImagePath 2>/dev/null || true)"
echo "  b2lfb ImagePath geri okundu: $YOL"
echo "  b2lsg0 ImagePath geri okundu: $YOL_GRP"
echo "  b2lsys ImagePath geri okundu: $YOL_SYS"
echo "  b2lauto ImagePath geri okundu: $YOL_AUTO"
if [ "$(printf '%s' "$YOL" | tr -cd '\\' | wc -c)" -lt 3 ]; then
  echo
  echo "  ###############################################################"
  echo "  # ImagePath BOZUK - ters boluk kacislari kaybolmus."
  echo "  # Beklenen: \\SystemRoot\\System32\\drivers\\b2lfb.sys"
  echo "  # .reg dosyasinda ters bolukler ciftlenmeli."
  echo "  ###############################################################"
  exit 1
fi
if [ "$(printf '%s' "$YOL_GRP" | tr -cd '\\' | wc -c)" -lt 3 ]; then
  echo
  echo "  ###############################################################"
  echo "  # b2lsg0 ImagePath BOZUK - ters boluk kacislari kaybolmus."
  echo "  # Beklenen: \\SystemRoot\\System32\\drivers\\b2lsg0.sys"
  echo "  ###############################################################"
  exit 1
fi
if [ "$(printf '%s' "$YOL_SYS" | tr -cd '\\' | wc -c)" -lt 3 ]; then
  echo
  echo "  ###############################################################"
  echo "  # b2lsys ImagePath BOZUK - ters boluk kacislari kaybolmus."
  echo "  # Beklenen: \\SystemRoot\\System32\\drivers\\b2lsys.sys"
  echo "  ###############################################################"
  exit 1
fi
if [ "$(printf '%s' "$YOL_AUTO" | tr -cd '\\' | wc -c)" -lt 3 ]; then
  echo
  echo "  ###############################################################"
  echo "  # b2lauto ImagePath BOZUK - ters boluk kacislari kaybolmus."
  echo "  # Beklenen: \\SystemRoot\\System32\\drivers\\b2lauto.sys"
  echo "  ###############################################################"
  exit 1
fi

#
# Hive de ayni tuzakta: once sil, sonra ekle. Yoksa duzenlenmis hive WIM'e
# hic girmez ve servis kaydi eski halinde kalir.
#
wimlib-imagex update "$WIM" 1 \
  --command="delete --force /Windows/System32/config/SYSTEM" >/dev/null 2>&1 || true
wimlib-imagex update "$WIM" 1 \
  --command="add '$W/SYSTEM' /Windows/System32/config/SYSTEM" >/dev/null
echo "  Services\\b2lfb    Start=0 Type=1 ErrorControl=3 (CRITICAL)"
echo "  Services\\b2lsg0   Start=1 Group=System Bus Extender ErrorControl=0"
echo "  Services\\b2lsys   Start=1 Type=1 ErrorControl=0 (IGNORE)"
echo "  Services\\b2lauto  Start=2 Type=1 ErrorControl=0 (IGNORE)"

#
# Hive de geri okunuyor: WIM'deki kopyada servis gercekten var mi?
#
HK="$W/geri"; mkdir -p "$HK"
wimlib-imagex extract "$WIM" 1 /Windows/System32/config/SYSTEM \
  --dest-dir="$HK" --no-acls >/dev/null 2>&1
GERI="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lfb' ImagePath 2>/dev/null || true)"
GERI_GRP="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lsg0' ImagePath 2>/dev/null || true)"
GERI_SYS="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lsys' ImagePath 2>/dev/null || true)"
GERI_AUTO="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lauto' ImagePath 2>/dev/null || true)"
GERI_START="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lfb' Start 2>/dev/null || echo '?')"
GERI_GRP_START="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lsg0' Start 2>/dev/null || echo '?')"
GERI_GRP_GROUP="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lsg0' Group 2>/dev/null || true)"
GERI_SYS_START="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lsys' Start 2>/dev/null || echo '?')"
GERI_AUTO_START="$(hivexget "$HK/SYSTEM" 'ControlSet001\Services\b2lauto' Start 2>/dev/null || echo '?')"
if [ -n "$GERI" ] && [ -n "$GERI_GRP" ] && [ -n "$GERI_SYS" ] && [ -n "$GERI_AUTO" ] && \
   [ "$GERI_START" = 0 ] && [ "$GERI_GRP_START" = 1 ] && \
   [ "$GERI_GRP_GROUP" = "System Bus Extender" ] && \
   [ "$GERI_SYS_START" = 1 ] && [ "$GERI_AUTO_START" = 2 ]; then
  echo "  dogrulandi: WIM hive b2lfb ImagePath = $GERI"
  echo "  dogrulandi: WIM hive b2lsg0 ImagePath = $GERI_GRP"
  echo "  dogrulandi: WIM hive b2lsys ImagePath = $GERI_SYS"
  echo "  dogrulandi: WIM hive b2lauto ImagePath = $GERI_AUTO"
  echo "  dogrulandi: WIM hive Start degerleri b2lfb=0 b2lsg0=1 b2lsys=1 b2lauto=2"
  echo "  dogrulandi: b2lsg0 Group = $GERI_GRP_GROUP"
else
  echo
  echo "  ###############################################################"
  echo "  # WIM'DEKI HIVE'DA b2lfb/b2lsg0/b2lsys/b2lauto kaydi veya Start degeri bozuk."
  echo "  #   b2lfb: path=$GERI start=$GERI_START"
  echo "  #   b2lsg0: path=$GERI_GRP start=$GERI_GRP_START group=$GERI_GRP_GROUP"
  echo "  #   b2lsys: path=$GERI_SYS start=$GERI_SYS_START"
  echo "  #   b2lauto: path=$GERI_AUTO start=$GERI_AUTO_START"
  echo "  # Duzenleme geri yazilmamis; flaslamak bosuna."
  echo "  ###############################################################"
  exit 1
fi

say "Dogrulama"
wimlib-imagex dir "$WIM" 1 2>/dev/null | grep -Ei "b2lfb|b2lsg0|b2lsys|b2lauto|b2lcmd" | sed 's/^/  /' || echo "  ! b2lfb/b2lsg0/b2lsys/b2lauto/b2lcmd WIM'de gorunmuyor"

cat <<'SON'

SIRA:
  1) BCD'de test imzalamayi ac. Windows'ta:
        windows\bcd_olustur.cmd testimza
     (BCD'yi kopyala, sonra tools_winboot.sh calistir)

  2) bash cache_esp.sh --win --system      ve system-esp.tar'i flasla

BEKLENEN OKUMA  (ErrorControl=3 ile artik hepsi ayirt edilebiliyor):
  EKRAN MACENTA                -> surucu yuklendi VE calisti
  Hata ekrani + 0xc0000428     -> imza reddedildi. Sertifika WinPE'nin guven
                                  deposuna eklenecek - cozulebilir, ve artik
                                  sebebi EKRANDA yaziyor
  Baska bir hata kodu          -> surucu yuklenemedi, kod sebebi soyluyor
  Hicbir sey, eskisi gibi      -> winload acilis surucilerine HIC gelmiyor;
                                  ariza daha geride

  Sonraki acilista startup.nsh dokumunde 0xCF030000'da "42 32 4C 46" (B2LF)
  gorunuyorsa surucu calismis demektir - macenta cikmasa bile.

  UFS HCI ANLIK GORUNTUSU (sonraki B2LGoster ekraninda, SURUM 32):
    UFS C=xxxxxxxx V=xxxxxxxx
    UFS H=xxxxxxxx T=xxxxxxxx
  Bu dort deger b2lfb'nin DriverEntry asamasinda bir kez, SALT-OKUNUR
  alinir: CAP, VER, HCE ve Samsung 1US_TO_CNT. Eslendikten hemen sonra
  MmUnmapIoSpace cagrilir; HCE/UIC/DME/kesme ya da UFS LUN'larina dokunulmaz.
  T degerinin alt 10 biti UEFI tarafindaki 166 MHz ayari icin 000000A6
  olmalidir. "UFS MAP=YOK" gorunurse HCI eslemesi basarisizdir.
  "UFS GIRIS" gorunurse b2lfb snapshot'a girdi fakat HCI eslemesi veya
  ilk MMIO okumasi tamamlanmadan reset geldi; o durumda daha fazla UFS
  erisimi eklenmeden once bu fiziksel erisim siniri incelenmelidir.
  "UFS KAYDI YOK" ise yuklenen b2lfb eski, baslatilmamis veya kayit soguk
  yeniden baslatmada silinmistir; HCI sonucu olarak yorumlanamaz.
  WinPE'nin bu cihazdaki yazilimsal reset istegi geri dondugu icin UFS
  sonucunun ana kanali artik CMD'dir: b2lcmd olayi goruldukten sonra
  b2lfb, salt-okunur kaydi `%SystemRoot%\B2LUFS.TXT` olarak WinPE RAM
  diskine yazar ve startnet onu taninin sonunda ekrana basar. Bu fiziksel
  UFS diskine yazmaz. Soguk kapatma sonrasi UEFI kaydi yine silinir.

  SCM SINIRI (sonraki B2LGoster ekraninda):
    SYG=REG C=000  -> sirali System Bus Extender grubuna dair isaret yok
    SYG=INI C=001  -> b2lsg0, bu system-start grubunda baslatildi
    SYS=REG C=000  -> b2lsys system-start asamasina ulasilmadan reset
    SYS=INI C=001  -> Windows system-start asamasina girdi (tum Start=1
                      suruculerinin bittigini tek basina kanitlamaz)
    AUT=REG C=000  -> b2lfb taze kayit yazdi; SCM auto-start asamasina dair
                      isaret yok
    AUT=SCM C=001  -> b2lauto, Service Control Manager tarafindan baslatildi

  KULLANICI-MODU SINIRI (sonraki B2LGoster ekraninda):
    USR=REG C=000  -> b2lcmd olayini 100 ms aralikla yoklayan surucu gormedi.
                       Bu; resetin olay
                       yaratilmadan gelmesi, CreateEventW basarisizligi ya da
                       kernelin olayi acamamasi arasinda ayrim yapmaz.
    USR=CMD C=001  -> Win32 + startnet.cmd b2lcmd satirina ulasti. b2lcmd,
                       cmd.exe /k satirindan once oldugu icin kabugu kanitlamaz.

  DIKKAT: cokusten sonra cihazi ELLE KAPATMA. RAM kaydi yalnizca sicak
  sifirlamada kaliyor.
SON
