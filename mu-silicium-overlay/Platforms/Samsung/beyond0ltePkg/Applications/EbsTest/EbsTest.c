/** @file
  EbsTest - ExitBootServices'ten sonra hicbir sey yapmayan bir kalp atisi.

  ----------------------------------------------------------------------------
  NEDEN
  ----------------------------------------------------------------------------
  Windows ~15 saniye sonra sessizce sifirlaniyor. ACPI'nin her parcasi stock
  DTB'ye karsi dogrulandi ve dogru cikti; kanca hic atesmedi; hicbir log
  kanali calismadi; bilinen iyi noktaya (43ef06c^) tam donus bile davranisi
  geri getirmedi.

  Hic sormadigimiz soru: SIFIRLAMA WINDOWS'LA ILGILI MI?

  Linux ExitBootServices cagirip 120 saniye yasiyor - ama zamanlayiciyi,
  GIC'i, kesme yonlendirmesini hepsini kuruyor. Windows cagirip 15 saniyede
  oluyor. Arada ucuncu bir durum var ve hic denenmedi:

      ExitBootServices cagir, SONRA HICBIR SEY KURMA.

  ----------------------------------------------------------------------------
  ILK DENEMEDE HIC KARE GORULMEDI - IKI HATA
  ----------------------------------------------------------------------------
  1) Bekleme dongusu cok kisaydi. 8 milyon volatile artirma bu cekirdekte
     saniye degil ~10 ms surer; kareler goz acip kapayana kadar ekrani
     doldurup biterdi. Sabit 60 kat buyutuldu.

  2) Daha onemlisi: uygulamanin ExitBootServices'ten ONCE mi SONRA mi
     durdugunu ayirt edemiyorduk. Print() kullanmak ise ise yaramiyor -
     EBS'ten sonra konsol yok.

     Cozum: her asama icin EKRANDA SABIT YERDE BIR RENK SERIDI. Konsola hic
     bagli degil, ve ekranda ne kaldiysa uygulamanin tam olarak nereye kadar
     geldigini soyluyor.

  ----------------------------------------------------------------------------
  EKRAN OKUMASI - yukaridan asagiya
  ----------------------------------------------------------------------------
    KIRMIZI serit yok        -> uygulama hic calismadi ya da GOP bulunamadi
    KIRMIZI var, MAVI yok    -> GetMemoryMap patladi
    MAVI var, SARI yok       -> ExitBootServices BASARISIZ (konsol hala var,
                                ekranda hata metni de olmali)
    SARI var, kare yok       -> EBS gecti ama hemen sonrasinda oluyoruz
    SARI + kareler           -> EBS gecti, dongu donuyor. KARELERI SAY.

  ----------------------------------------------------------------------------
  SONUC
  ----------------------------------------------------------------------------
    ~15 karede sifirliyor -> sorun Windows'ta DEGIL. Platform, EBS'ten sonra
        servis edilmeyen bir seye takiliyor ve Linux onu tesadufen tatmin
        ediyor. ACPI'de arayacak bir sey kalmaz.

    Kareler dolmaya devam ediyor -> sifirlama gercekten Windows'a ozel.
        Ilk kez tahminle degil olcumle soyleyebiliriz.
**/

#include <Uefi.h>

#include <Library/ArmLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>

#define SERIT_Y   0     // asama seritleri burada baslar
#define SERIT_H   28    // her seridin yuksekligi
#define KARE      28
#define BOSLUK     6

//
// EL gostergesinin bandi. Aşama seritlerinin ALTINDA, tik izgarasinin
// USTUNDE ve ekranin ortasinda - kavisli kosenin uzaginda.
//
#define EL_Y      (SERIT_Y + 3 * SERIT_H + 40)
#define EL_H      200

#define RENK_KIRMIZI  0x00FF0000
#define RENK_MAVI     0x000060FF
#define RENK_SARI     0x00FFD000
#define RENK_YESIL    0x0000FF00
#define RENK_SIYAH    0x00000000

STATIC UINT32 *mFb;
STATIC UINTN   mPps;
STATIC UINTN   mW;
STATIC UINTN   mH;

STATIC
VOID
Doldur (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Genislik,
  IN UINTN  Yukseklik,
  IN UINT32 Renk)
{
  UINTN Sx, Sy;

  for (Sy = Y; (Sy < Y + Yukseklik) && (Sy < mH); Sy++) {
    for (Sx = X; (Sx < X + Genislik) && (Sx < mW); Sx++) {
      mFb[Sy * mPps + Sx] = Renk;
    }
  }
}

/**
  Asama seridi. Sirayla asagi dogru diziliyor; ekranda hangi seritler kaldiysa
  uygulama oraya kadar gelmis demektir.
**/
STATIC
VOID
Asama (
  IN UINTN  Sira,
  IN UINT32 Renk)
{
  Doldur (0, SERIT_Y + Sira * SERIT_H, mW, SERIT_H - 4, Renk);
}

/**
  Kaba gecikme. Zamanlayici yok, kesme yok - elimizde yalnizca mesgul dongu
  var. Sabit deneysel; tam olmasi gerekmiyor, onemli olan kare SAYISININ
  denemeler arasinda karsilastirilabilir olmasi.

  ILK SURUMDE 8000000 IDI VE COK KISAYDI. volatile bir UINT64 artirma
  yaklasik uc cevrim; 8 milyon ~2 GHz'de 10 ms bile etmiyor. 500 milyon
  saniye mertebesine getiriyor.
**/
STATIC
VOID
Bekle (
  VOID)
{
  //
  // MESGUL DONGU YERINE MIMARI ZAMANLAYICI.
  //
  // Ilk surum sabit sayida volatile artirma yapiyordu. O sayinin kac saniye
  // ettigi cekirdek frekansina, onbellek durumuna ve DVFS'e bagli - yani
  // "kac kare ciktigi" ile Windows tarafindaki "TIK=nnnn" karsilastirilamaz
  // hale geliyordu. Oysa bu testin BUTUN degeri o karsilastirmada.
  //
  // MicroSecondDelay ArmArchTimerLib uzerinden CNTPCT'yi okuyor: ne kesme
  // ister ne boot servisi, yani ExitBootServices'ten sonra da gecerli.
  // Artik her kare TAM BIR SANIYE ve dogrudan TIK ile ayni birim.
  //
  MicroSecondDelay (1000000);
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop      = NULL;
  EFI_MEMORY_DESCRIPTOR        *Harita   = NULL;
  UINTN                         Boyut    = 0;
  UINTN                         Anahtar  = 0;
  UINTN                         TanimBoy = 0;
  UINT32                        Surum    = 0;
  EFI_STATUS                    Status;
  UINTN                         Tik;
  UINTN                         Sutun;
  UINTN                         Satir;
  UINTN                         SutunSay;
  UINTN                         KareY0;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL)) {
    Print (L"EbsTest: GOP bulunamadi (%r). Test yapilamaz.\n", Status);
    return Status;
  }

  mFb  = (UINT32 *)(UINTN)Gop->Mode->FrameBufferBase;
  mPps = Gop->Mode->Info->PixelsPerScanLine;
  mW   = Gop->Mode->Info->HorizontalResolution;
  mH   = Gop->Mode->Info->VerticalResolution;

  if ((mFb == NULL) || (mW == 0) || (mH == 0)) {
    Print (L"EbsTest: framebuffer gecersiz (0x%lx %ux%u)\n", (UINT64)(UINTN)mFb, (UINT32)mW, (UINT32)mH);
    return EFI_DEVICE_ERROR;
  }

  //
  // Butun Print'ler BURADA bitiyor. Bundan sonra ekrana kendimiz yaziyoruz;
  // once yazsak GraphicsConsoleDxe uzerine basardi.
  //
  //
  // HANGI EXCEPTION LEVEL'DAYIZ?
  //
  // Impalabs'in RKP incelemesi: "SBOOT boots the Linux kernel in EL1, with a
  // backdoor in TrustZone implemented to load and execute custom code in EL2.
  // Samsung's kernel uses init/vmm.c to load the proprietary RKP hypervisor
  // through an SMC call interface."
  //
  // Yani RKP kendiliginden yuklenmiyor - Samsung'un CEKIRDEGI yukluyor. Bizim
  // UEFI'miz yuklemiyor, Windows da yuklemiyor. Mu-Silicium'un SEC kodu da
  // (Sec/AArch64/ModuleEntryPoint.S, EL1_OR_EL2 makrosu) bir seviye
  // dayatmiyor, S-Boot hangi seviyede birakirsa ona gore vektor kuruyor.
  //
  // Ikisi birlestiginde EL1 bekliyoruz ve EL2'nin BOS olmasi gerekiyor.
  // Olcum bunu dogruluyor: EL1 cikarsa ortada hipervizor yok, RKP hipotezi
  // tamamen kapanir. EL2 cikarsa varsayimlarimizin ikisi de yanlis demektir.
  //
  Print (L"\nEbsTest %ux%u @ 0x%lx\n", (UINT32)mW, (UINT32)mH, (UINT64)(UINTN)mFb);
  Print (L"CurrentEL = EL%u\n", (UINT32)(ArmReadCurrentEL () >> 2));
  Print (L"Seritler: kirmizi=basladi mavi=harita sari=EBS gecti, sonra kareler.\n");
  Print (L"Kareleri say. Ekran temizleniyor...\n");

  Doldur (0, 0, mW, mH, RENK_SIYAH);
  Asama (0, RENK_KIRMIZI);        // 1: calisiyoruz, GOP tamam

  //
  // EL'i ekrana da bas: EL sayisi kadar BUYUK beyaz kare.
  //
  // ILK YERLESIM EKRANIN SOL UST KOSESINDEYDI VE OKUNAMADI: bu panelin
  // kavisli kosesi orayi yutuyor. Olcum aracinin kendisi gorunmuyorsa
  // olcum yok demektir - ayni dersi 25 satirlik konsolda da almistik.
  //
  // Simdi ekranin ORTASINDA, kenarlardan uzakta ve iri. Tek bakista
  // sayilabilsin diye aralarinda genis bosluk var.
  //
  //   1 kare -> EL1. Windows'un SMC'lerini tuzaklayamayiz, EL2 bos degil
  //             ya da bize kapali.
  //   2 kare -> EL2. HCR_EL2.TSC ile EL1'den gelen HER SMC bize duser;
  //             TrustZone arka kapisina hic gerek kalmaz.
  //
  {
    UINTN El = ArmReadCurrentEL () >> 2;
    UINTN I;
    UINTN Kenar = 140;
    UINTN Ara   = 60;
    UINTN Y     = EL_Y;
    UINTN X0;

    X0 = (mW - (El * Kenar + (El - 1) * Ara)) / 2;

    for (I = 0; I < El; I++) {
      Doldur (X0 + I * (Kenar + Ara), Y, Kenar, Kenar, 0x00FFFFFF);
    }
  }

  //
  // Bellek haritasi. Iki asamali: ilk cagri boyutu veriyor, AllocatePool
  // haritayi degistirebildigi icin ikinci cagri sart ve MapKey ondan
  // aliniyor.
  //
  Status = gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum);
  Boyut += 8 * TanimBoy;

  Status = gBS->AllocatePool (EfiLoaderData, Boyut, (VOID **)&Harita);
  if (EFI_ERROR (Status)) {
    Print (L"EbsTest: AllocatePool %r\n", Status);
    return Status;
  }

  Status = gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum);
  if (EFI_ERROR (Status)) {
    Print (L"EbsTest: GetMemoryMap %r\n", Status);
    return Status;
  }

  Asama (1, RENK_MAVI);           // 2: harita alindi

  Status = gBS->ExitBootServices (ImageHandle, Anahtar);
  if (EFI_ERROR (Status)) {
    //
    // Anahtar bayatladiysa bir kez daha dene. Basarisiz olursa boot
    // servisleri hala acik demektir; Print calisir.
    //
    Boyut = 0;
    gBS->GetMemoryMap (&Boyut, NULL, &Anahtar, &TanimBoy, &Surum);
    Boyut += 8 * TanimBoy;
    gBS->AllocatePool (EfiLoaderData, Boyut, (VOID **)&Harita);
    Status = gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum);
    if (!EFI_ERROR (Status)) {
      Status = gBS->ExitBootServices (ImageHandle, Anahtar);
    }

    if (EFI_ERROR (Status)) {
      Print (L"EbsTest: ExitBootServices %r  (MAVI var, SARI yok)\n", Status);
      return Status;
    }
  }

  //
  // BURADAN SONRA HICBIR SERVIS YOK. Ne gBS, ne Print, ne konsol.
  // Yalnizca framebuffer.
  //
  Asama (2, RENK_SARI);           // 3: EBS gecti

  SutunSay = (mW - BOSLUK) / (KARE + BOSLUK);
  if (SutunSay == 0) {
    SutunSay = 1;
  }

  //
  // Tik izgarasi EL bandinin ALTINDAN basliyor; ustune yazsaydi gostergeyi
  // birkac saniye icinde silerdi.
  //
  KareY0 = EL_Y + EL_H;

  Tik = 0;
  for (;;) {
    Sutun = Tik % SutunSay;
    Satir = Tik / SutunSay;

    Doldur (
      BOSLUK + Sutun * (KARE + BOSLUK),
      KareY0 + Satir * (KARE + BOSLUK),
      KARE,
      KARE,
      RENK_YESIL);

    //
    // Sari seridin sag ucunda yanip sonen bir isaret: dongu donuyor mu,
    // kareler ekrandan tastiktan sonra da gorunsun.
    //
    Doldur (
      mW - 2 * KARE,
      SERIT_Y + 2 * SERIT_H,
      KARE,
      SERIT_H - 4,
      ((Tik & 1) != 0) ? RENK_SIYAH : RENK_KIRMIZI);

    Bekle ();
    Tik++;
  }
}
