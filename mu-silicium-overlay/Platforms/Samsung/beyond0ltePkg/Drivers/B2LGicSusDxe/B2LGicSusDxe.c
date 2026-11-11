/** @file
  B2LGicSusDxe - ExitBootServices aninda GIC'teki butun SPI'lari sustur.

  ---------------------------------------------------------------------------
  NEDEN
  ---------------------------------------------------------------------------
  Cihazda UEFI kesmeleri hic acmadigi halde GIC'te SEKIZ SPI bekliyordu.
  Numaralari DTB'de arayinca kimlikleri cikti:

      SPI  39   exynos-adv_tracer     APM posta kutusu
      SPI  43   speedy@15970000       PMIC veriyolu
      SPI 183   decon_f@19030000      ekran denetleyici
      SPI 184   decon_f@19030000      ekran denetleyici
      SPI 190   dsim@19080000         MIPI DSI
      SPI 198   dpp@19071000          ekran islemcisi

  Dordu ekran hatti. S-Boot acilis logosunu gostermek icin DECON/DSIM/DPP'yi
  CALISIR BIRAKIYOR - panelin hala taraniyor olmasinin ve framebuffer'a
  yazdigimizda goruntunun cikmasinin sebebi bu. O bloklar her karede kesme
  uretiyor. Linux bunlari taniyip temizliyor; UEFI temizlemiyor, Windows'un
  ise surucusu yok.

  Bu aslinda bir firmware hatasi: UEFI belirtimi ExitBootServices oncesinde
  aygitlarin susturulmasini ve kesmelerin kapatilmasini sart kosuyor.
  Mu-Silicium bu bloklari S-Boot'tan devraldigi icin hic susturmuyor.

  ---------------------------------------------------------------------------
  NEDEN BU, GOZLEMLERIN TAMAMINA UYUYOR
  ---------------------------------------------------------------------------
    - EbsTest 30+ saniye yasiyor      -> kesmeleri hic acmiyor
    - Windows ~10 saniyede oluyor     -> kesmeleri aciyor
    - Bugcheck yok                    -> karar Windows'un degil
    - ResetSystem cagrilmiyor         -> sifirlamayi Windows istemiyor
    - EL3 istisnasi yok               -> EL3 de patlamiyor
    - Is temelli, sure temelli degil  -> kesmeleri actigi ana bagli
    - CPU %99 dolu                    -> temizlenemeyen kesme yeniden giriyor

  ---------------------------------------------------------------------------
  NE YAPIYOR
  ---------------------------------------------------------------------------
  ExitBootServices olayinda dagiticidaki BUTUN SPI'lari kapatiyor ve bekleyen
  bitlerini temizliyor:

      GICD_ICENABLER (0x180)  1 yazmak o kesmeyi KAPATIR
      GICD_ICPENDR   (0x280)  1 yazmak bekleyen bitini TEMIZLER

  SGI/PPI (ilk 32) ELLENMIYOR: mimari zamanlayici orada ve Windows onu kendi
  kuruyor; bankali oldugu icin buradan kapatmak zaten kalici olmazdi.

  Butun SPI'lari kapatmak guvenli: Windows GIC'i ACPI MADT'ye gore sifirdan
  programliyor ve ihtiyaci olani kendisi aciyor. Zaten bu bloklarin hicbiri
  icin surucusu yok.

  Kayit 0xCF041300:
      +0x00  "GICQ"
      +0x08  once bekleyen SPI sayisi
      +0x10  sonra bekleyen SPI sayisi   (0 olmali)
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define GICD          0x10101000ULL
#define GICD_ICENABLE 0x180
#define GICD_ICPEND   0x280
#define GICD_ISPEND   0x200
#define GICD_ISENABLE 0x100
#define GICD_IGROUP   0x080

#define GIC_KAYIT     0xCF041300ULL
#define GIC_ONCEKI    0xCF041340ULL

STATIC EFI_EVENT mCikisOlayi;

STATIC
UINT32
BekleyenSay (
  VOID)
{
  volatile UINT32 *P;
  UINT32           Kac;
  UINTN            I;

  P   = (volatile UINT32 *)(UINTN)(GICD + GICD_ISPEND);
  Kac = 0;

  //
  // Ilk kelime (SGI/PPI) sayilmiyor: zamanlayici orada ve her zaman
  // hareketli. Bir dedektorun surekli hareket eden bir kaynagi saymasi
  // gurultu uretir - bu dersi INTID 30'u "bulgu" sanarak aldik.
  //
  for (I = 1; I < 32; I++) {
    UINT32 V = P[I];

    while (V != 0) {
      Kac++;
      V &= (V - 1);
    }
  }

  return Kac;
}

STATIC
VOID
EFIAPI
CikistaSustur (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  volatile UINT32 *Kapat;
  volatile UINT32 *Temizle;
  UINT64          *Kayit;
  UINTN            I;

  Kapat   = (volatile UINT32 *)(UINTN)(GICD + GICD_ICENABLE);
  Temizle = (volatile UINT32 *)(UINTN)(GICD + GICD_ICPEND);
  Kayit   = (UINT64 *)(UINTN)GIC_KAYIT;

  Kayit[0] = 0x0000005143494747ULL;   // "GGICQ" imzasi
  Kayit[1] = BekleyenSay ();

  //
  // Once kapat, SONRA temizle. Ters sirada yapsaydik kapatmayla temizleme
  // arasinda kaynak yeniden tetikleyip bekleyen biti geri kaldirabilirdi -
  // DECON her karede bir kesme uretiyor, arada 16 milisaniye var.
  //
  for (I = 1; I < 32; I++) {
    Kapat[I] = 0xFFFFFFFF;
  }

  for (I = 1; I < 32; I++) {
    Temizle[I] = 0xFFFFFFFF;
  }

  Kayit[2] = BekleyenSay ();

  //
  // YAZDIGINI DOGRULA - IKI FARKLI SONUCU AYIRT EDIYOR.
  //
  // Ilk olcumde "once 8, sonra 8" cikti ve bunu "kaynak sinyali basili
  // tutuyor" diye okudum. Ama ikinci bir aciklama daha var ve sinanmadi:
  // YAZIMIZ HIC GECMEMIS olabilir.
  //
  // GICv2'de guvenlik uzantisi varsa Grup 0 kesmeleri GUVENLI tarafa aittir
  // ve Non-secure taraftan yapilan GICD_ICENABLER yazmalari SESSIZCE YOK
  // SAYILIR. Ekranda ikisi de ayni gorunuyor.
  //
  // Ayrimin sonucu buyuk:
  //   yazma gecti, bekleyen geri geliyor -> aygiti (DECON) kapatmaliyiz
  //   yazma hic gecmedi                  -> o kesmeler EL3'un, dokunamayiz
  //                                         ve FIQ olarak EL3'e gidiyorlar
  //
  // Bu yuzden ISENABLER geri okunuyor (kac SPI HALA ACIK) ve IGROUPR
  // dokuluyor (kac SPI Grup 1, yani Non-secure).
  //
  {
    volatile UINT32 *Acik  = (volatile UINT32 *)(UINTN)(GICD + GICD_ISENABLE);
    volatile UINT32 *Grup  = (volatile UINT32 *)(UINTN)(GICD + GICD_IGROUP);
    UINT32           A = 0, G = 0;

    for (I = 1; I < 32; I++) {
      UINT32 V = Acik[I];
      UINT32 W = Grup[I];

      while (V != 0) { A++; V &= (V - 1); }
      while (W != 0) { G++; W &= (W - 1); }
    }

    Kayit[3] = A;   // susturmadan SONRA hala acik olan SPI sayisi
    Kayit[4] = G;   // Grup 1 (Non-secure) olarak okunan SPI sayisi
  }
}

EFI_STATUS
EFIAPI
B2LGicSusEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  //
  // ONCE KOPYALA, SONRA TEMIZLE.
  //
  // Ilk surumde kayit dogrudan sifirlaniyordu ve sonucu HIC goremezdik:
  // olay Windows'a devrederken atesleniyor, DriverEntry ise bir sonraki
  // acilisin en basinda calisiyor. Yani her acilis, bir onceki devrin
  // sonucunu okunmadan siliyordu.
  //
  // Bu hatanin birebir aynisini SEH kaydinda da yapmistik. Kaydini bir
  // sonraki acilista okunacak her olcum, temizlemeden once kopyalanmali.
  //
  CopyMem ((VOID *)(UINTN)GIC_ONCEKI, (VOID *)(UINTN)GIC_KAYIT, 0x40);
  ZeroMem ((VOID *)(UINTN)GIC_KAYIT, 0x40);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  CikistaSustur,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mCikisOlayi);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "B2LGicSus: cikis olayi kurulamadi: %r\n", Status));
  }

  return EFI_SUCCESS;
}
