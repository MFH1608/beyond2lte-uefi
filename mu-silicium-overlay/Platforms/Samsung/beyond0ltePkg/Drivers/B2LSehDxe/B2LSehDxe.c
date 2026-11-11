/** @file
  B2LSehDxe - Samsung EL3 monitorunun "Smart Exception Handler" kancasi.

  ---------------------------------------------------------------------------
  NEDEN
  ---------------------------------------------------------------------------
  Kirk turluk elemeden sonra geriye tek aktor kaldi: EL3.

    - Windows sifirlamayi ISTEMIYOR (ResetSystem sayaci 0, PSCI kapaliyken de)
    - Bugcheck degil (dogrulanmis geri cagri, iki yonde de)
    - Cekirdek son ana kadar saglikli (kalp atisi duzenli)
    - Acilisi bloke edince HIC olmuyor
    - Donanim watchdog'lari kapali (WTCON okundu, ikisi de 0)
    - Guc kesilmiyor (DRAM hayatta, RST_STAT 0x20000000)

  Stok boot.img'den ve ianmacd/d2s kaynagindan (d2s = Galaxy S10+ Exynos,
  tam olarak bizim cihaz) mekanizma bulundu:

    drivers/soc/samsung/exynos-el3_mon.c
      static void exynos_smart_exception_handler(
          unsigned int id, unsigned long elr, unsigned long esr,
          unsigned long sctlr, unsigned long ttbr, unsigned long tcr,
          unsigned long x6, unsigned int offset);

      ret = exynos_smc(SMC_CMD_SET_SEH_ADDRESS, addr, 0, 0);
      arch_initcall(exynos_set_seh_address);

  EL3 monitoru BEKLENMEDIK BIR ISTISNA ALDIGINDA, cekirdegin kaydettigi bu
  adrese atliyor ve EL3 baglamini argüman olarak veriyor. Samsung bunu
  arch_initcall'da, acilisin cok erken bir noktasinda kaydediyor.

  Kaydedilmemisse EL3'un atlayacak yeri yok. Bir guvenlik monitorunun baska
  secenegi de yok: SIFIRLAR. Sessizce, isletim sistemine haber vermeden.

  Windows bu SMC'yi asla yapmayacak. Biz yapiyoruz.

  ---------------------------------------------------------------------------
  NEDEN ASSEMBLY GEREKMIYOR
  ---------------------------------------------------------------------------
  Isleyici sekiz argumani x0-x7'de aliyor - AArch64 C cagri kuralinin (AAPCS64)
  tam olarak aynisi. Linux'unki de duz bir C fonksiyonu. Dolayisiyla normal bir
  C fonksiyonu dogru degerleri aliyor.

  Yine de govde MINIMUM tutuluyor: yalnizca global bir isaretci uzerinden
  yazma, fonksiyon cagrisi yok. EL3'ten donuldugunde yigin durumu belirsiz.

  ---------------------------------------------------------------------------
  SANAL ADRES MESELESI
  ---------------------------------------------------------------------------
  Linux SANAL adres kaydediyor; EL3 cekirdegin ceviri duzeninde eret yapiyor.
  Yani adres, o an gecerli EL1 sanal adres uzayinda anlamli olmali.

    UEFI icinde       -> kimlik haritalama, fiziksel adres yeterli
    Windows devralinca -> SetVirtualAddressMap sonrasi bizim calisma zamani
                          kodumuz Windows tarafindan haritalaniyor

  Bu yuzden SEH'i IKI KEZ kaydediyoruz: bir kez DriverEntry'de, bir kez de
  gEfiEventVirtualAddressChange olayinda YENI adresle. Ikinci kayit sart -
  yoksa Windows devraldigi anda EL3'un elindeki adres cope donuyor.

  Bu duzenek B2LRtHookDxe'de zaten kanitlandi (GetVariable 372 girip 372
  donuyor).

  ---------------------------------------------------------------------------
  KAYIT YERLESIMI - hepsi B2L RT Log bolgesinin (0xCF040000, 64 KB) icinde
  ---------------------------------------------------------------------------
    0xCF040000  B2LRtHookDxe'nin kaydi (ilk 0x40 bayt)
    0xCF041000  SEH yakalama kaydi        <- bu surucu
    0xCF041080  kayit durumu (SMC donusleri)
    0xCF042000  EL3'un GPR dokumu icin 2 sayfa (SMC_CMD_SET_DEBUG_MEM)

  SET_DEBUG_MEM FIZIKSEL adres aliyor (Linux virt_to_phys ile veriyor), yani
  orada ceviri derdi yok.
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiDriverEntryPoint.h>

//
// include/linux/smc.h (ianmacd/d2s). Samsung eski usul NEGATIF komut
// kimlikleri kullaniyor; 64 bite isaret genisletiliyor.
//
#define SMC_CMD_SET_DEBUG_MEM    ((UINT64)(INT64)-120)
#define SMC_CMD_GET_LOCKUP_REASON ((UINT64)(INT64)-121)
#define SMC_CMD_SET_SEH_ADDRESS  ((UINT64)(INT64)-123)

//
// Status[3] icin baslangic isareti.
//
// GEREKCE: ilk surumde Status[3] sifirla basliyordu ve ekranda "0 = KABUL"
// diye gosteriliyordu. Ama o alan SetVirtualAddressMap olayinda yaziliyor,
// olay ise kabuktan cok sonra calisiyor - yani gordugumuz sifir "basarili"
// degil "henuz calismadi" demekti. Sifirin iki anlama geldigi bir gosterge,
// gostergesizlikten kotudur.
//
//
// NOBETCI DEGERI OLCTUGU SEYLE CAKISMAMALI.
//
// Ilk secim 0xFFFFFFFE idi ve tam da EL3'un dondugu deger cikti (-2).
// Ekranda "olay calismadi" yazdi, oysa olay calismisti ve okudugumuz sey
// EL3'un CEVABIYDI. Bir SMC'nin asla donmeyecegi bir degeri seciyoruz ve
// ayrica olayin calistigini SMC'den BAGIMSIZ bir damgayla isaretliyoruz.
//
#define SANAL_CALISMADI  0xB2B2B2B2UL
#define OLAY_DAMGASI     0x4E414C53UL   // "SLAN"

//
// SURUM DAMGASI - "onceki acilis" degerleri ne zaman guvenilir?
//
// Bu tur bir sayiyi yanlis okuduk: LOCKUP ONC=EBF9DDFF. Onceki acilista
// calisan derleme Status[6]'yi hic yazmiyordu, yani okudugumuz sey
// temizlenmemis DRAM'di. Yanindaki SANAL ONC=FBFFCEB7 de ayni karakterde
// ve ayni supheyi tasiyor.
//
// Bir sayinin gercek mi cop mu oldugunu ayirt edemiyorsak o sayi olcum
// degil. Cozum: her acilista bir surum damgasi birakiyoruz. Onceki kopyanin
// damgasi bugunkuyle ayni degilse gosterici degerleri hic yazmiyor.
//
// Yerlesimi degistiren her degisiklikte BU SAYIYI ARTIR.
//
#define B2L_SURUM  18

#define SEH_LOG      0xCF041000ULL   // yakalanan EL3 baglami
#define SEH_STATUS   0xCF041080ULL   // SMC donusleri
//
// PSCI SORGUSU - stok DTB'den cikan yapisal celiskiyi olcuyor.
//
//   psci { compatible = "arm,psci"; ... }     <- 0.1, "arm,psci-0.2" DEGIL
//
// PSCI 0.1'de yalnizca cpu_suspend/cpu_off/cpu_on var. PSCI_VERSION,
// PSCI_FEATURES, SYSTEM_RESET, SYSTEM_OFF, AFFINITY_INFO YOK. Linux'un
// "PSCIv65535.65535" cikti da bunu gosteriyor: PSCI_VERSION 0xFFFFFFFF
// donuyor.
//
// ACPI ise PSCI 0.1'i ifade EDEMIYOR: FADT'deki bit "0.2 veya ustu" demek
// ve Windows ARM64 bunu sart kosuyor. Biti actigimizda Windows'a olmayan
// bir arayuzu var diyoruz; kapattigimizda Windows hic acilmiyor.
//
// YALNIZCA SORGULAR CAGRILIYOR. SYSTEM_RESET (0x84000009), SYSTEM_OFF
// (0x84000008) ve CPU_SUSPEND (0xC4000001) BILEREK cagrilmiyor - ilk ikisi
// cihazi kapatir, ucuncusu askiya alir. Bir olcum aracinin olctugu seyi
// tetiklememesi gerekiyor.
//
#define PSCI_VERSION_FN      0x84000000ULL
#define PSCI_FEATURES_FN     0x8400000AULL
#define PSCI_MIGRATE_INFO_FN 0x84000006ULL
#define PSCI_AFFINITY_FN     0xC4000004ULL

#define SEH_ONCEKI   0xCF041100ULL   // BIR ONCEKI acilisin yakalamasi
#define DURUM_ONCEKI 0xCF041180ULL   // BIR ONCEKI acilisin durum sozcugu
#define SEH_METIN    0xCF0410C0ULL   // SMC donusleri, okunabilir ASCII
#define SEH_DBGMEM   0xCF042000ULL   // EL3'un GPR dokumu, 2 sayfa
#define SEH_DBGSIZE  0x2000ULL

STATIC UINT64   *mSehLog;
STATIC UINT32   *mStatus;
STATIC UINT8    *mMetin;

/**
  SMC donuslerini ASCII yaz.

  NEDEN: ilk denemede kayit alaninda yalnizca "SEHR" gorunuyordu ve asil
  onemli iki sayiyi (SMC donusleri) bulanik bir fotograftan hex olarak
  cozmek gerekiyordu. Ayni dersi TIK= sayacinda da almistik: dmem'in sag
  sutunundaki ASCII gorunumu bedava, kullanmamak sacma.

  Ekranda tam olarak su okunacak:  DBG=00002000 SEH=00000000 SV=00000000
**/
STATIC
VOID
HexYaz (
  IN UINT8  *Hedef,
  IN UINT32  Deger)
{
  INTN I;

  for (I = 7; I >= 0; I--) {
    UINT8 D = (UINT8)(Deger & 0xF);
    Hedef[I] = (UINT8)((D < 10) ? ('0' + D) : ('A' + D - 10));
    Deger >>= 4;
  }
}
STATIC EFI_EVENT mVirtualAddressChangeEvent;

/**
  EL3 monitoru beklenmedik bir istisna alinca BURAYA atliyor.

  Argüman sirasi exynos_smart_exception_handler ile birebir:
    Id     0 ise istisna EL3 monitorunun kendisinden, degilse bir LDFW'den
    Elr    elr_el3   - istisnanin olustugu adres
    Esr    esr_el3   - ISTISNA SINIFI. Aradigimiz asil deger.
    Sctlr  sctlr_el3
    Ttbr   ttbr_el3
    Tcr    tcr_el3
    X6     scr_el3
    Offset GPR dokumunun debug_mem icindeki konumu

  DONMUYOR. Kasitli: cihaz sifirlanmasin, ekranda donup kalsin. O anda
  b2lfb'nin cizdigi seritler ekranda duruyor olacak, yani kacinci saniyede
  oldugunu da goruyoruz.
**/
STATIC
VOID
EFIAPI
SmartExceptionHandler (
  IN UINT64 Id,
  IN UINT64 Elr,
  IN UINT64 Esr,
  IN UINT64 Sctlr,
  IN UINT64 Ttbr,
  IN UINT64 Tcr,
  IN UINT64 X6,
  IN UINT64 Offset)
{
  //
  // Fonksiyon cagrisi YOK, yerel dizi YOK. EL3'ten donuldugunde yigin
  // durumu belirsiz; ne kadar az sey yaparsak o kadar iyi.
  //
  if (mSehLog != NULL) {
    mSehLog[0] = 0x0000002145534853ULL;   // "SEH!" + sifir dolgu
    mSehLog[1] = Id;
    mSehLog[2] = Elr;
    mSehLog[3] = Esr;
    mSehLog[4] = Sctlr;
    mSehLog[5] = Ttbr;
    mSehLog[6] = Tcr;
    mSehLog[7] = X6;
    mSehLog[8] = Offset;
  }

  //
  // Sonsuza kadar dur. Sifirlama olmasin ki ekran ve kayit yerinde kalsin.
  //
  for (;;) {
  }
}

/**
  SEH adresini EL3'e bildir ve donusu kaydet.

  Donus sifir degilse basarisiz. Bilinen degerler (exynos-el3_mon.h):
    1      EXYNOS_ERROR_ALREADY_INITIALIZED  - S-Boot zaten kaydetmis
    0x1000 EXYNOS_ERROR_NOT_VALID_ADDRESS
**/
STATIC
UINT32
SehKaydet (
  IN UINT64 Adres)
{
  ARM_SMC_ARGS Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = SMC_CMD_SET_SEH_ADDRESS;
  Args.Arg1 = Adres;
  Args.Arg2 = 0;
  Args.Arg3 = 0;

  ArmCallSmc (&Args);
  return (UINT32)Args.Arg0;
}

/**
  SetVirtualAddressMap sonrasi: isaretcileri cevir ve SEH'i YENIDEN kaydet.

  Ikinci kayit sart. Windows devraldigi anda EL3'un elindeki fiziksel adres
  artik gecerli degil; isleyicinin yeni sanal adresini bildirmezsek EL3
  istisna aninda copluge atlar - ki bu, olcmek istedigimiz seyi bizim
  uretmemiz olurdu.
**/
STATIC
VOID
EFIAPI
VirtualAddressChange (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  VOID       *Handler;
  UINT32     *Status;
  UINT8      *Metin;
  EFI_STATUS  C1, C2;

  Handler = (VOID *)SmartExceptionHandler;

  //
  // ISARETCILERI CEVIRMEDEN ONCE SAKLA.
  //
  // Bu olay HALA kimlik haritalamasinda calisiyor: cevrilen adresler ancak
  // isletim sistemi kendi haritasina gectikten SONRA gecerli oluyor. Ilk
  // surumde metin tamponuna cevrildikten SONRA yaziliyordu - yani heniiz
  // haritali olmayan bir adrese kor yazma. Kendi olcum aracimizi bozan
  // tam da bu tur bir ayrinti oluyor.
  //
  Status = mStatus;
  Metin  = mMetin;

  //
  // OLAYIN CALISTIGINI SMC'DEN BAGIMSIZ ISARETLE. Onceki turda bunu
  // SMC'nin donus degerinden cikarmaya calistik ve yanildik.
  //
  Status[12] = OLAY_DAMGASI;

  C1 = EfiConvertPointer (0, (VOID **)&mSehLog);
       EfiConvertPointer (0, (VOID **)&mStatus);
       EfiConvertPointer (0, (VOID **)&mMetin);
  C2 = EfiConvertPointer (0, (VOID **)&Handler);

  //
  // CEVIRININ KENDISI DE OLCULUYOR.
  //
  // "Sanal kayit reddedildi" sonucunun iki cok farkli sebebi olabilir:
  // ya EL3 adresi begenmedi, ya da cevirme hic olmadi ve EL3'e AYNI
  // fiziksel adresi ikinci kez verdik. Ikisini ayirt etmeden ilerlemek
  // tahmin olur.
  //
  Status[10] = (UINT32)C1;
  Status[11] = (UINT32)C2;

  Status[3] = SehKaydet ((UINT64)(UINTN)Handler);
  Status[4] = (UINT32)((UINT64)(UINTN)Handler);
  Status[5] = (UINT32)(((UINT64)(UINTN)Handler) >> 32);

  HexYaz (Metin + 26, Status[3]);
}

EFI_STATUS
EFIAPI
B2LSehEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS           Status;
  ARM_SMC_ARGS         Args;
  EFI_PHYSICAL_ADDRESS Adres;

  mSehLog = (UINT64 *)(UINTN)SEH_LOG;
  mStatus = (UINT32 *)(UINTN)SEH_STATUS;
  mMetin  = (UINT8  *)(UINTN)SEH_METIN;

  //
  // ONCE KOPYALA, SONRA TEMIZLE.
  //
  // Ilk surumde canli kayit dogrudan sifirlaniyordu ve bu, olcmek
  // istedigimiz seyi imha ediyordu: DXE surucileri kabuktan ONCE calisiyor,
  // yani onceki acilista yakalanmis bir istisna biz onu okuyamadan
  // siliniyordu. Ayni sinif hatayi daha once ErrorControl=0 ile de
  // yapmistik - "guvenli" gorunen bir ayar en bilgilendirici olcumu
  // korlestirmisti.
  //
  CopyMem ((VOID *)(UINTN)SEH_ONCEKI, mSehLog, 0x80);

  //
  // Durum sozcugu de kopyalaniyor. Windows sirasinda yapilan YENIDEN KAYDIN
  // sonucu (Status[3]) yalnizca burada gorulebilir: onu yazan olay bu
  // acilistan sonra calisiyor, yani ancak bir sonraki acilista okunabilir.
  //
  CopyMem ((VOID *)(UINTN)DURUM_ONCEKI, mStatus, 0x40);

  ZeroMem (mSehLog, 0x80);
  ZeroMem (mStatus, 0x40);
  ZeroMem (mMetin, 0x40);
  ZeroMem ((VOID *)(UINTN)SEH_DBGMEM, SEH_DBGSIZE);

  mStatus[0] = 0x52484553;   // "SEHR" - surucu yuklendi
  mStatus[3]  = SANAL_CALISMADI;
  mStatus[15] = B2L_SURUM;

  //
  // ASCII sablon. Rakamlar asagida dolduruluyor.
  //
  CopyMem (mMetin, "DBG=xxxxxxxx SEH=xxxxxxxx SV=xxxxxxxx  ", 39);

  //
  // 1) EL3'e GPR dokumu icin bellek ver. FIZIKSEL adres istiyor, o yuzden
  //    ceviri derdi yok. Linux'ta dogru donus degeri BOYUTUN KENDISI.
  //
  //
  // ADRESI AllocatePages ILE ALIYORUZ, sabit carve-out'tan DEGIL.
  //
  // Ilk denemede 0xCF042000 verdik ve EL3 0xFFFFFFFF dondu. Linux buraya
  // kmalloc'tan gelen, yani NORMAL cekirdek DRAM'inde bir adres veriyor;
  // EL3 muhtemelen adresin izinli bolgede olmasini istiyor. Ayrilmis bir
  // carve-out o kosulu saglamiyor olabilir. Sirali bir eleme: adres
  // kaynagini degistirip ayni SMC'yi tekrar soruyoruz.
  //
  Adres = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiRuntimeServicesData, 2, &Adres);
  if (EFI_ERROR (Status)) {
    Adres = SEH_DBGMEM;
  } else {
    ZeroMem ((VOID *)(UINTN)Adres, SEH_DBGSIZE);
  }

  mStatus[8] = (UINT32)Adres;
  mStatus[9] = (UINT32)(Adres >> 32);

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = SMC_CMD_SET_DEBUG_MEM;
  Args.Arg1 = Adres;
  Args.Arg2 = SEH_DBGSIZE;
  Args.Arg3 = 0;
  ArmCallSmc (&Args);
  mStatus[1] = (UINT32)Args.Arg0;
  HexYaz (mMetin + 4, mStatus[1]);

  //
  // 2) SEH ADRESI BURADA KAYDEDILMIYOR - BILEREK.
  //
  // Onceki olcum: DriverEntry'deki kayit 0 (kabul), AYNI acilis icindeki
  // ikinci kayit -2 (ret) dondu. Bu, EL3'un SEH'i acilis basina yalnizca
  // BIR KEZ kabul ettigini dusunduruyor.
  //
  // Eger oyleyse tek hakkimizi dogru ana saklamak zorundayiz. Fiziksel
  // adresle simdi kaydetmek, Windows devraldiginda ise yaramayan bir adresi
  // EL3'e vermek ve hakki harcamak demek. Kaydi SetVirtualAddressMap
  // olayina birakiyoruz; orada isleyicinin Windows'ta gecerli olacak sanal
  // adresi elimizde oluyor (olculdu: 0xFFFFF803406BD570).
  //
  // Bu ayni zamanda soruyu da ayiriyor:
  //   olaydaki kayit 0 donerse  -> once "zaten kayitli" diye reddediliyordu
  //   yine -2 donerse           -> EL3 adresin KENDISINI begenmiyor
  //
  mStatus[2] = SANAL_CALISMADI;
  HexYaz (mMetin + 17, mStatus[2]);

  //
  // 3) EL3'e "en son neden kilitlendin" diye sor.
  //
  // Samsung'un EL3 monitorunde bir KILITLENME kavrami var ve Linux bunu
  // sorabiliyor. Sifirlamanin her seferinde tam 10 saniyede olmasi bir
  // kilitlenme dedektorunun imzasina cok benziyor. Donanim watchdog'ini
  // guvensiz taraftan "kapali" okumamiz bunu DISLAMIYOR: yazmac guvenli
  // tarafa aitse EL1'den gordugumuz manzara gercegi yansitmayabilir.
  //
  // 0xFFFFFFFF donerse komut taninmiyor demektir ve fikir kapanir.
  //
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = SMC_CMD_GET_LOCKUP_REASON;
  ArmCallSmc (&Args);
  mStatus[6] = (UINT32)Args.Arg0;
  mStatus[7] = (UINT32)Args.Arg1;

  //
  // PSCI sorgulari. Donen 0xFFFFFFFF "bu islev yok" demek.
  //
  //   [16] PSCI_VERSION      0.2+ olsaydi surum numarasi donerdi
  //   [17] PSCI_FEATURES(SYSTEM_RESET)  sistem sifirlama var mi
  //   [18] MIGRATE_INFO_TYPE
  //   [19] AFFINITY_INFO(cpu0)
  //
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = PSCI_VERSION_FN;
  ArmCallSmc (&Args);
  mStatus[16] = (UINT32)Args.Arg0;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = PSCI_FEATURES_FN;
  Args.Arg1 = 0x84000009;   // SYSTEM_RESET hakkinda SORU, cagri degil
  ArmCallSmc (&Args);
  mStatus[17] = (UINT32)Args.Arg0;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = PSCI_MIGRATE_INFO_FN;
  ArmCallSmc (&Args);
  mStatus[18] = (UINT32)Args.Arg0;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = PSCI_AFFINITY_FN;
  Args.Arg1 = 0;
  Args.Arg2 = 0;
  ArmCallSmc (&Args);
  mStatus[19] = (UINT32)Args.Arg0;

  DEBUG ((EFI_D_ERROR, "B2LSeh: dbgmem=0x%x seh=0x%x lockup=0x%x\n", mStatus[1], mStatus[2], mStatus[6]));

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  VirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mVirtualAddressChangeEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "B2LSeh: sanal adres olayi kurulamadi: %r\n", Status));
  }

  return EFI_SUCCESS;
}
