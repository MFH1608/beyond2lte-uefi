/** @file
  B2LGoster - tani kayitlarini BUYUK harflerle ekrana cizer.

  ---------------------------------------------------------------------------
  NEDEN
  ---------------------------------------------------------------------------
  Olcumleri simdiye kadar "dmem" ile okuduk ve her seferinde ayni iki sorun
  cikti: yazi 1440x3040 ekranda 8x19 piksel oldugu icin fotograftan zor
  seciliyor, ve onaltilik dokum satirlari 25 satirlik konsolu doldurup
  aradigimiz degeri yukari kaydiriyor. Ikincisi bize IKI TUR kaybettirdi.

  Konsolun yazi tipi buyutulemez: EFI_GLYPH_WIDTH/HEIGHT MdePkg'de sabit ve
  glif bitmapleri de o boyutta uretiliyor. Makroyu degistirmek yaziyi
  buyutmuyor, sadece harflerin arasini aciyor.

  Cozum: cizimi biz yapiyoruz. Glifleri HII font protokolunden aliyoruz -
  yani konsolun kullandigi yazi tipinin AYNISI, elle yazilmis bir font
  tablosuna gerek yok - ve her pikseli NxN blok olarak framebuffer'a
  basiyoruz.

  Ustelik onaltilik dokum yerine YORUMLANMIS satir yaziyoruz:

      SEH  KAYIT=00000000  DBG=FFFFFFFF
      SEH  YAKALANDI  ESR=96000010
      B2LFB  TIK=0063  BUG=H
      RST_STAT=20000000  YAZILIM

  ---------------------------------------------------------------------------
  ONEMLI: ONCEKI ACILIS KAYDINI OKUYOR
  ---------------------------------------------------------------------------
  B2LSehDxe her acilista canli kaydi (0xCF041000) temizliyor ve temizlemeden
  once bir onceki acilisin kopyasini 0xCF041100'e aliyor. DXE surucileri
  kabuktan ONCE calistigi icin canli kaydi okumak her zaman bos deger verir.
  Burada okunan KOPYA.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiImage.h>
#include <Protocol/HiiFont.h>

//
// B2LSehDxe'deki B2L_SURUM ile AYNI olmali. Onceki acilisin degerleri
// yalnizca damgalar tutuyorsa gosteriliyor.
//
#define B2L_SURUM    23

//
// Ekranin en ustundeki damga. Eski bir kopya calisirsa (birden fazla FAT
// biriminde B2LGOSTR.EFI olabiliyor - bu tur tam olarak oldu ve iki farkli
// ekran cikti) tek bakista anlasilsin diye.
//
#define SURUM_METNI  L"SURUM 32"

#define OLCEK        4      // her glif pikseli 4x4 blok
#define KENAR       24      // sol/ust bosluk

#define SEH_CANLI   0xCF041000ULL
#define SEH_DURUM   0xCF041080ULL
#define SEH_ONCEKI  0xCF041100ULL
#define DUR_ONCEKI  0xCF041180ULL
#define TARA_KAYIT  0xCF041200ULL   // bellek taramasinin ilerlemesi
#define GIC_KAYIT   0xCF041300ULL   // B2LGicSusDxe'nin kaydi
#define GIC_ONCEKI  0xCF041340ULL   // onceki acilisin devir ani
#define GICD        0x10101000ULL   // dagitici
#define GICD_ISPEND 0x200           // bekleyen kesme bitleri
#define FB_KAYIT    0xCF030000ULL   // b2lfb (Windows surucusu)
#define HVC_KAYIT   0xCF030000ULL   // B2LEl2Dxe onceki HVC kaydi
#define HVC_DXE_TANI (HVC_KAYIT + 0x100ULL)
#define RT_KAYIT    0xCF040000ULL   // B2LRtHookDxe
#define PMU_RST     0x15860404ULL

typedef struct {
  UINT64  Base;
  UINT64  Boyut;
} B2L_DTB_REZERV;

// Stok DTB'nin /reserved-memory dugumundeki sabit reg araliklari.
// Android bunlari DTB'den gorur; Windows icin tek yetkili kaynak EFI memory
// map'tir. Bu liste, yeni imajin gercekte hepsini ConventionalMemory disinda
// biraktigini ekranda dogrulamak icindir.
STATIC B2L_DTB_REZERV CONST mDtbRezervleri[] = {
  {0x0000000080000000ULL, 0x00001000ULL},
  {0x0000000091200000ULL, 0x00001000ULL},
  {0x0000000096000000ULL, 0x02200000ULL},
  {0x0000000098200000ULL, 0x00E00000ULL},
  {0x0000000099000000ULL, 0x1B800000ULL},
  {0x00000000C0000000ULL, 0x00080000ULL},
  {0x00000000D0000000ULL, 0x01600000ULL},
  {0x00000000EB308000ULL, 0x00008000ULL},
  {0x00000000EB310000ULL, 0x01500000ULL},
  {0x00000000EC810000ULL, 0x00A00000ULL},
  {0x00000000F0000000ULL, 0x0B900000ULL},
  {0x00000000FB900000ULL, 0x02000000ULL},
  {0x00000000FD900000ULL, 0x00010000ULL},
  {0x00000000FD910000ULL, 0x00200000ULL},
  {0x00000000FDB10000ULL, 0x00400000ULL},
  {0x00000000FDF10000ULL, 0x00100000ULL},
  {0x00000000FE010000ULL, 0x00A00000ULL},
  {0x00000000FEA10000ULL, 0x00100000ULL},
  {0x00000000FEB10000ULL, 0x00400000ULL},
  {0x00000000FEF10000ULL, 0x00400000ULL},
  {0x00000000FF310000ULL, 0x000F0000ULL},
  {0x00000000FF400000ULL, 0x00C00000ULL},
  {0x0000000880000000ULL, 0x0AD00000ULL}
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_HII_FONT_PROTOCOL        *mFont;
STATIC UINT32                       *mFb;
STATIC UINT32                        mSatirBasi;   // piksel/satir (stride)
STATIC UINT32                        mSatir;       // o anki y

/**
  Sabit DTB rezervlerinden kac tanesi EFI'de tam olarak
  EfiReservedMemoryType ile kapsanmiyor? Windows ExitBootServices sonrasinda
  BootServices turlerini geri alabilir; bu nedenle yalnizca "Conventional
  degil" denetimi yeterli degildir. Sifir, Windows'un bu araliklari tahsis
  edemeyecegi anlamina gelir.
**/
STATIC
UINT32
DtbAcikRezervSayisi (
  VOID)
{
  EFI_MEMORY_DESCRIPTOR *Harita;
  UINTN                  Boyut, Anahtar, TanimBoy, I, R;
  UINT32                 Surum;
  UINT32                 Acik;

  Harita = NULL;
  Boyut  = 0;
  Acik   = 0;

  gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum);
  Boyut += 8 * TanimBoy;
  if (EFI_ERROR (gBS->AllocatePool (EfiLoaderData, Boyut, (VOID **)&Harita))) {
    return 0xFFFFFFFF;
  }

  if (!EFI_ERROR (gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum))) {
    for (R = 0; R < ARRAY_SIZE (mDtbRezervleri); R++) {
      UINT64 HedefSon;
      BOOLEAN Korundu;

      HedefSon = mDtbRezervleri[R].Base + mDtbRezervleri[R].Boyut;
      Korundu  = FALSE;

      for (I = 0; I < Boyut / TanimBoy; I++) {
        EFI_MEMORY_DESCRIPTOR *D;
        UINT64                 Son;

        D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Harita + I * TanimBoy);
        if (D->Type != EfiReservedMemoryType) {
          continue;
        }

        Son = D->PhysicalStart + D->NumberOfPages * 4096;
        if (D->PhysicalStart <= mDtbRezervleri[R].Base && Son >= HedefSon) {
          Korundu = TRUE;
          break;
        }
      }

      if (!Korundu) {
        Acik++;
      }
    }
  } else {
    Acik = 0xFFFFFFFF;
  }

  gBS->FreePool (Harita);
  return Acik;
}

/**
  Tek karakter ciz. Glif HII'den geliyor, her piksel OLCEK x OLCEK blok.
**/
STATIC
VOID
Karakter (
  IN CHAR16  Ch,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Renk)
{
  EFI_IMAGE_OUTPUT *Blt;
  UINT32            Px, Py, I, J;

  Blt = NULL;
  if (EFI_ERROR (mFont->GetGlyph (mFont, Ch, NULL, &Blt, NULL)) || Blt == NULL) {
    return;
  }

  for (Py = 0; Py < Blt->Height; Py++) {
    for (Px = 0; Px < Blt->Width; Px++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P;

      P = &Blt->Image.Bitmap[Py * Blt->Width + Px];

      //
      // HII glifi on plani beyaz, arka plani siyah veriyor. Sifirdan farkli
      // her sey "harfin bir parcasi" sayiliyor - esik aramaya gerek yok.
      //
      if ((P->Red | P->Green | P->Blue) == 0) {
        continue;
      }

      for (J = 0; J < OLCEK; J++) {
        for (I = 0; I < OLCEK; I++) {
          mFb[(Y + Py * OLCEK + J) * mSatirBasi + (X + Px * OLCEK + I)] = Renk;
        }
      }
    }
  }

  if (Blt->Image.Bitmap != NULL) {
    gBS->FreePool (Blt->Image.Bitmap);
  }
  gBS->FreePool (Blt);
}

/**
  Bir satir yaz ve asagi in.
**/
STATIC
VOID
Yaz (
  IN CHAR16 *Metin,
  IN UINT32  Renk)
{
  UINT32 X;

  X = KENAR;
  while (*Metin != L'\0') {
    Karakter (*Metin, X, mSatir, Renk);
    X += 8 * OLCEK;
    Metin++;
  }

  mSatir += 19 * OLCEK + 4;
}

STATIC CHAR16 mTampon[64];

STATIC
VOID
Hex32 (
  IN OUT CHAR16 *Hedef,
  IN     UINT32  Deger)
{
  INTN I;

  for (I = 7; I >= 0; I--) {
    UINT8 D = (UINT8)(Deger & 0xF);
    Hedef[I] = (CHAR16)((D < 10) ? (L'0' + D) : (L'A' + D - 10));
    Deger >>= 4;
  }
}

/**
  "ONEK=XXXXXXXX" bicimini kurup yazdirir.
**/
STATIC
VOID
Deger (
  IN CHAR16 *Onek,
  IN UINT32  Sayi,
  IN CHAR16 *Not,
  IN UINT32  Renk)
{
  UINTN N;

  N = 0;
  while (Onek[N] != L'\0' && N < 32) {
    mTampon[N] = Onek[N];
    N++;
  }

  Hex32 (&mTampon[N], Sayi);
  N += 8;

  if (Not != NULL) {
    mTampon[N++] = L' ';
    mTampon[N++] = L' ';
    while (*Not != L'\0' && N < 60) {
      mTampon[N++] = *Not++;
    }
  }

  mTampon[N] = L'\0';
  Yaz (mTampon, Renk);
}

EFI_STATUS
EFIAPI
B2LGosterEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS      Status;
  UINT32         *Durum;
  UINT32         *Onc;
  UINT64         *Onceki;
  UINT32         *RtLog;
  UINT8          *FbLog;
  UINT32         *HvcLog;
  UINT32         *HvcDxe;
  UINT32          Rst;
  UINT32          AcikRezerv;
  EFI_INPUT_KEY   Key;
  UINTN           Index;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    Print (L"B2LGoster: GOP yok: %r\n", Status);
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mFont);
  if (EFI_ERROR (Status)) {
    Print (L"B2LGoster: HII font yok: %r\n", Status);
    return Status;
  }

  mFb        = (UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
  mSatirBasi = mGop->Mode->Info->PixelsPerScanLine;
  mSatir     = KENAR;

  //
  // Ekrani temizle. dmem ciktisinin ustune yazmak yerine bos sayfa: tek
  // amaci fotograftan tek bakista okunabilmesi.
  //
  SetMem32 (mFb, (UINTN)mSatirBasi * mGop->Mode->Info->VerticalResolution * 4, 0x00000000);

  Durum  = (UINT32 *)(UINTN)SEH_DURUM;
  Onc    = (UINT32 *)(UINTN)DUR_ONCEKI;
  Onceki = (UINT64 *)(UINTN)SEH_ONCEKI;
  RtLog  = (UINT32 *)(UINTN)RT_KAYIT;
  FbLog  = (UINT8  *)(UINTN)FB_KAYIT;
  HvcLog = (UINT32 *)(UINTN)HVC_KAYIT;
  HvcDxe = (UINT32 *)(UINTN)HVC_DXE_TANI;
  Rst    = *(volatile UINT32 *)(UINTN)PMU_RST;

  Yaz (L"=== BEYOND2LTE TANI ===", 0x00FFFFFF);
  Yaz (SURUM_METNI, 0x00808080);

  //
  // PSCI HVC koprusu yalnizca "yap.sh --psci-hvc" ile yuklenir. Kayit
  // ayni sicak-sifirlama bolgesinde oldugu icin b2lfb kaydiyla cakisir;
  // imzalar farkli oldugundan ikisini de acikca ayiriyoruz. Bu satirlar
  // bellek taramasindan ONCE cizilir ki sonraki acilisin asil cevabi
  // fotograf karesinde kalsin.
  //
  if (HvcLog[0] == 0x56483242) {  // "B2HV"
    Yaz (L"*** PSCI HVC KOPRUSU ***", 0x0040FF40);
    Deger (L"HVC SAYI =", HvcLog[1], NULL, 0x0040FF40);
    Deger (L"SON PSCI =", HvcLog[2], NULL, 0x0040FF40);
  } else {
    Yaz (L"PSCI HVC KAYDI YOK", 0x00808080);
  }

  if (HvcDxe[0] == 0x44483242) {  // "B2HD": HVC DXE yukleyicisi
    Yaz (L"*** HVC DXE DURUMU ***", 0x0060C0FF);
    Deger (L"EL2 SMC =", HvcDxe[1],
           (HvcDxe[1] == 2) ? L"GERI" : L"CIKMADI", 0x0060C0FF);
    Deger (L"VMM X0 =", HvcDxe[3], NULL, 0x0060C0FF);
    Deger (L"VMM X1 =", HvcDxe[4], NULL, 0x0060C0FF);
  } else {
    Yaz (L"HVC DXE KAYDI YOK", 0x00FF4040);
  }

  mSatir += 12;

  //
  // BEYAN EDILEN RAM. Bir PCD degistirdik diye degistigini varsaymak
  // yeterli degil - Mu-Silicium DRAM boyutunu calisma zamaninda da
  // hesaplayabiliyor ve PCD'yi gormezden gelebilir. Deneyin bagimli
  // degiskeni degismediyse deney yapilmamis demektir.
  //
  {
    EFI_MEMORY_DESCRIPTOR *Harita;
    UINTN                  Boyut, Anahtar, TanimBoy, I;
    UINT32                 Surum2;
    UINT64                 Sayfa;
    UINT64                 Tepe;
    UINT32                 Parca;

    Harita = NULL;
    Boyut  = 0;
    Sayfa  = 0;
    Tepe   = 0;
    Parca  = 0;

    gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum2);
    Boyut += 8 * TanimBoy;

    if (!EFI_ERROR (gBS->AllocatePool (EfiLoaderData, Boyut, (VOID **)&Harita)) &&
        !EFI_ERROR (gBS->GetMemoryMap (&Boyut, Harita, &Anahtar, &TanimBoy, &Surum2))) {
      for (I = 0; I < Boyut / TanimBoy; I++) {
        EFI_MEMORY_DESCRIPTOR *D;

        D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Harita + I * TanimBoy);
        if (D->Type != EfiReservedMemoryType && D->Type != EfiMemoryMappedIO) {
          UINT64 Son;

          Sayfa += D->NumberOfPages;
          Parca++;

          Son = D->PhysicalStart + D->NumberOfPages * 4096;
          if (Son > Tepe) {
            Tepe = Son;
          }
        }
      }

      gBS->FreePool (Harita);
    }

    //
    // RAM/TEPE/PARCA SATIRLARI KALDIRILDI - SORULARI KAPANDI.
    //
    // Dort banka dogru beyan edilmis (tepe 0xA00000000, 8 GB), hepsi gercek
    // ve takla atmiyor, 7.75 GB'in tamami tarandi. Ekranda yer birakmak
    // icin tek dogrulama satirina indirildi; sayilar gerektiginde
    // gecmisten okunabilir.
    //
    Deger (L"RAM MB  =", (UINT32)((Sayfa * 4) / 1024), NULL, 0x00808080);
  }

  AcikRezerv = DtbAcikRezervSayisi ();
  Deger (L"DTB ACIK=", AcikRezerv,
         (AcikRezerv == 0) ? L"KORUNDU" :
         (AcikRezerv == 0xFFFFFFFF) ? L"OKUNAMADI" : L"WINDOWS GOREBILIR",
         (AcikRezerv == 0) ? 0x0040FF40 : 0x00FF4040);

  // Banka yaz/geri-oku deneyi onceki turlarda tamamlandi. Dinamik Android
  // rezervlerinin fiziksel yeri kesin degilken bir sayfaya bile yazmak, bu
  // koruyucu Windows turunda gereksiz risk olur.
  Yaz (L"RAM YAZMA TARAMASI ATLANDI", 0x00808080);
  mSatir += 12;

  //
  // 1) EL3 SEH - bu turun asil sorusu.
  //
  if (Durum[0] != 0x52484553) {
    Yaz (L"SEH SURUCUSU YUKLENMEDI", 0x00FF4040);
  } else {
    //
    // SANAL degeri BU acilistan degil ONCEKINDEN okunuyor. Yazan olay
    // (SetVirtualAddressMap) kabuktan sonra calisiyor; bu acilisin degeri
    // her zaman "henuz calismadi" olur.
    //

    //
    // PSCI: hangi islevler GERCEKTEN var?
    //
    // Stok DTB "arm,psci" diyor, yani 0.1 - uc islev disinda hicbir sey yok.
    // ACPI bunu ifade edemiyor ve Windows 0.2+ sart kosuyor. FFFFFFFF donen
    // her satir, Windows'un var sandigi ama olmayan bir islev demek.
    //
    Deger (L"PSCI VER=", Durum[16],
           (Durum[16] == 0xFFFFFFFFUL) ? L"YOK" : L"VAR",
           (Durum[16] == 0xFFFFFFFFUL) ? 0x00FF4040 : 0x0040FF40);
    Deger (L"PSCI RST=", Durum[17],
           (Durum[17] == 0xFFFFFFFFUL) ? L"YOK" : L"VAR",
           (Durum[17] == 0xFFFFFFFFUL) ? 0x00FF4040 : 0x0040FF40);
    Deger (L"PSCI AFF=", Durum[19],
           (Durum[19] == 0xFFFFFFFFUL) ? L"YOK" : L"VAR", 0x0060C0FF);

    //
    // ONCEKI ACILIS - yalnizca ayni surum yazdiysa. Damga tutmuyorsa
    // oradaki sayilar temizlenmemis DRAM olabilir ve gosterilmesi
    // olcum sanilir. Bu tur tam olarak o oldu.
    //
    if (Onc[0] == 0x52484553 && Onc[15] == B2L_SURUM) {
      //
      // Olayin calistigi SMC'den BAGIMSIZ damgayla dogrulaniyor. Onceki
      // turda bunu donus degerinden cikarmaya calistik ve nobetci deger
      // EL3'un cevabiyla cakisti.
      //
      if (Onc[12] != 0x4E414C53UL) {
        Yaz (L"SANAL OLAY HIC CALISMADI", 0x00FF4040);
      } else {
        Deger (L"SANAL ONC=", Onc[3],
               (Onc[3] == 0) ? L"KABUL" : L"RET",
               (Onc[3] == 0) ? 0x0040FF40 : 0x00FF4040);
      }

      //
      // Sanal kayit reddedildiyse SEBEBI burada ayrisiyor:
      //   CONV != 0        -> ceviri hic olmadi, EL3'e ayni fiziksel
      //                       adresi ikinci kez verdik
      //   CONV == 0 ama VA  -> ceviri oldu, adresi EL3 begenmedi
      // Ustteki iki durum bambaska iki isi gerektiriyor.
      //
    } else {
      Yaz (L"ONCEKI ACILIS BASKA SURUM", 0x00808080);
    }
  }

  mSatir += 12;

  //
  // 2) Onceki acilistan kalan EL3 istisnasi. ARADIGIMIZ SEY.
  //
  if (Onceki[0] == 0x0000002145534853ULL) {
    Yaz (L"*** EL3 ISTISNASI YAKALANDI ***", 0x0040FF40);
    Deger (L"ESR EL3 =", (UINT32)Onceki[3], NULL, 0x0040FF40);
    Deger (L"EC      =", (UINT32)(Onceki[3] >> 26) & 0x3F, NULL, 0x0040FF40);
    Deger (L"ELR LO  =", (UINT32)Onceki[2], NULL, 0x00C0C0C0);
    Deger (L"ELR HI  =", (UINT32)(Onceki[2] >> 32), NULL, 0x00C0C0C0);
    Deger (L"ID      =", (UINT32)Onceki[1], NULL, 0x00C0C0C0);
  } else {
    Yaz (L"EL3 ISTISNASI YOK", 0x00808080);
  }

  mSatir += 12;

  //
  // 3) Windows surucusunun olum sonrasi kaydi ve calisma zamani sayaclari.
  //
  if (FbLog[0] == 'B' && FbLog[1] == '2' && FbLog[2] == 'L' && FbLog[3] == 'F') {
    // UFS CAP/VER satiri 25 gorunur karakterdir; onceki 24 elemanli
    // tampon sadece eski 16/23 karakterlik b2lfb damgalari icindi.
    CHAR16 Tik[32];
    UINTN  K;

    //
    // +0x30: Windows boot-driver aninda, PnP/storufs HCI'yi sahiplenmeden
    // once alinan SALT-OKUNUR UFS goruntusu. Bu satirlar once cizilir:
    // ekranin altina tasip sessizce kaybolmamali. Tam kayit iki satirdir;
    // "UFS GIRIS" ve "UFS MAP=YOK" ise snapshot'in nerede kaldigini
    // gosteren tek-satirlik ara durumlardir.
    //
    if (FbLog[48] == 'U' && FbLog[49] == 'F' && FbLog[50] == 'S') {
      for (K = 0; K < 25; K++) {
        Tik[K] = (CHAR16)FbLog[48 + K];
        if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
          Tik[K] = L' ';
        }
      }
      Tik[25] = L'\0';
      Yaz (Tik, 0x0060C0FF);

      if (FbLog[52] == 'C' && FbLog[53] == '=') {
        Tik[0] = L'U'; Tik[1] = L'F'; Tik[2] = L'S';
        for (K = 0; K < 22; K++) {
          Tik[3 + K] = (CHAR16)FbLog[48 + 25 + K];
          if (Tik[3 + K] < 0x20 || Tik[3 + K] > 0x7E) {
            Tik[3 + K] = L' ';
          }
        }
        Tik[25] = L'\0';
        Yaz (Tik, 0x0060C0FF);
      }
    } else {
      Yaz (L"UFS KAYDI YOK", 0x00FF4040);
    }

    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[16 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);


    //
    // MAX: bir tikte gorulen en buyuk tur sayisi = dongunun serbest hizi.
    // SPN'i bununla bolunce Windows'un gercekte ne kadar CPU kullandigi
    // cikiyor; TIK tek basina bunu soyleyemiyor.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[96 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0x20: bosta dongusunun tur sayaci. "CPU hic bosa dusmedi" iddiasinin
    // tek kaniti bu - turkuaz bant yalnizca dongunun BASLADIGINI soyluyor.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[32 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0x70: I/O Manager'in boot-driver reinitialization siniri.
    // BRI=REG callback'in kaydedildigini ama aygitlarin tumunun henuz
    // baslatilmadigini; BRI=nnnn C=001 ise tamamlandigi andaki 100 ms
    // tikini gosterir. Bu, onceki image/process callback'leri gibi yeni
    // bir kanca degil; boot surucusunun belgelenmis yasam dongusu noktasi.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[112 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0x80: DPC tikinin duvar saatiyle karsilastirmasi. QPD/F saniyedir.
    // TIK tek basina zamanlayici DPC'sinin gecikmedigini varsayar; bu ham
    // oran o varsayimi bir sonraki acilista sinar.
    //
    for (K = 0; K < 23; K++) {
      Tik[K] = (CHAR16)FbLog[128 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[23] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0xA0: ayri Start=2 b2lauto.sys'nin SCM auto-start damgasi.
    // AUT=REG C=000 b2lfb'nin taze baslangic degeridir; AUT=SCM C=001
    // ise Service Control Manager'in bu surucuyu baslattigini kanitlar.
    // Bu, riskli image/process bildirimi kullanmadan kullanici-modu/SCM
    // sinirine dair bir sonraki guvenli ayrimdir.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[160 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0xB0: ayri Start=1 b2lsys.sys'nin sistem-ilklendirme damgasi.
    // SYS=REG C=000 b2lfb'nin taze baslangic degeridir; SYS=INI C=001
    // ise Windows'un SERVICE_SYSTEM_START asamasinin bu surucuye kadar
    // ilerledigini kanitlar. Start=2/SCM damgasi bundan sonra gelir.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[176 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0xC0: System Bus Extender grubundeki Start=1 b2lsg0 damgasi.
    // SYG=REG C=000, grupsuz b2lsys'den onceki sirali bir grup calismadan
    // reset oldugunu; SYG=INI C=001 ise bu grubun en az bu noktaya kadar
    // ilerledigini gosterir.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[192 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // +0xD0: startnet.cmd'nin cmd.exe /k satirindan ONCE baslattigi
    // b2lcmd.exe damgasi. USR=CMD, Win32 ve startnet'in bu noktaya
    // ulastigini kanitlar; kabugun acildigini kanitlamaz. USR=REG ise
    // b2lcmd olayi gorulmedi demektir: reset, CreateEventW basarisizligi
    // ve kernelin olayi acamama durumlarini birbirinden ayiramaz.
    //
    for (K = 0; K < 16; K++) {
      Tik[K] = (CHAR16)FbLog[208 + K];
      if (Tik[K] < 0x20 || Tik[K] > 0x7E) {
        Tik[K] = L' ';
      }
    }

    Tik[16] = L'\0';
    Yaz (Tik, 0x00FFFF40);

    //
    // CNT/IMG SATIRLARI KALDIRILDI.
    //
    // Kaynagi olan kanca hem coktu (0x1E / b2lfb.sys) hem sinyal
    // vermiyordu: ayni goruntuyle CNT 16, 3 ve 21 cikti. Bos ya da
    // guvenilmez bir satiri ekranda tutmak, bir sonraki turda onu olcum
    // sanmaya davetiye.
    //

    //
    // +0x60: ilk ve son imajin tiki. TIK artik 100 ms birimi.
    //
    //   SN, TIK'e YAKIN  -> olum yuklemenin icinde
    //   SN cok erken     -> yukleme cabuk bitmis, Windows baska bir seyde
    //                       donuyor ve surucu adi kovalamak bosuna
  } else {
    Yaz (L"B2LFB KAYDI YOK", 0x00808080);
  }

  if (RtLog[0] == 0x524C3242) {
    Deger (L"GETVAR GIR=", RtLog[3], NULL, 0x00808080);
    Deger (L"GETVAR CIK=", RtLog[4], NULL, 0x00808080);
    Deger (L"RESET     =", RtLog[5], NULL, 0x00808080);
  }

  mSatir += 12;

  Deger (L"RST STAT=", Rst,
         (Rst == 0x20000000) ? L"YAZILIM" :
         (Rst == 0x40010000) ? L"GUC" : L"BASKA",
         0x0060C0FF);

  //
  // BELLEK TARAMASI + GIC BEKLEYEN KESME DENETIMI
  //
  // Stok kernel kaynagi (exynos-ppmpu.c) taramanin ilk halini gecersiz
  // kildi: PPMPU ihlali SENKRON BIR HATA DEGIL. CPU durmuyor, erisim
  // tamamlaniyor ve yalnizca bir KESME kalkiyor. Yani yaz/oku testi
  // ihlalden hic haberdar olmaz, sessizce "temiz" der ve bellegi
  // yanlislikla aklardik.
  //
  // Ustelik surucudeki su yorum zincirin tamamini aciklıyor:
  //
  //     Interrupt status register in PPMPU will clear in this SMC handler
  //     exynos_smc(SMC_CMD_GET_PPMPU_FAIL_INFO, ...)
  //
  // Durum yazmacini YALNIZCA EL3 temizleyebiliyor. Windows o kesmeyi ne
  // tanir ne temizleyebilir; seviye tetiklemeli bir kesme temizlenmedigi
  // surece kesmeler acilir acilmaz sonsuza kadar yeniden girer.
  //
  // Bu yuzden dogru dedektor yaz/oku degil, GIC'in BEKLEYEN KESME bitleri.
  // DTB'den (exynos9820-base.dts):
  //
  //     ppmpu  SPI 350,351,357,358,364,365,371,372  -> INTID 382..404
  //     itmon  SPI  52, 53,121,127,128, 54          -> INTID  84..160
  //
  // Once anlik goruntu aliniyor, sonra her 1 MB'ta bir yeniden okunuyor.
  // Yeni bir bit kalkarsa hem HANGI kesme oldugu hem de o sirada
  // dokunulan adres elimizde oluyor - tek acilista 1 MB cozunurlukle
  // bisect.
  //
  // Bu turda hicbir belirsiz RAM sayfasina yazilmayacak. GIC goruntusu de
  // onceki turlarda kayda alindi; yeni bellek koruma sonucunu karistirmasin.
  Yaz (L"RAM TARAMASI ATLANDI", 0x00808080);
#if 0
  {
    UINT64          *T = (UINT64 *)(UINTN)TARA_KAYIT;
    volatile UINT32 *G = (volatile UINT32 *)(UINTN)(GICD + GICD_ISPEND);
    UINT32           Ilk[32];
    UINTN            K;

    //
    // SGI/PPI (INTID 0-31) HARIC.
    //
    // Ilk denemede dedektor INTID 30'u yakaladi ve "kesme kalkti" dedi.
    // 30 mimari zamanlayicinin PPI'si; UEFI onu zaten kullaniyor, yani
    // tarama sirasinda sadece zaman gecti. Bir dedektorun her zaman
    // hareketli olan bir kaynagi isaret etmesi bulgu degil, gurultu.
    // Yalnizca SPI'lar (32 ve ustu) anlamli.
    //
    Ilk[0] = 0xFFFFFFFF;
    for (K = 1; K < 32; K++) {
      Ilk[K] = G[K];
    }

    if (T[0] == 0x0000004152415419ULL && T[2] == 1) {
      Yaz (L"*** TARAMA SIRASINDA OLDU ***", 0x00FF4040);
      Deger (L"ADRES HI=", (UINT32)(T[1] >> 32), NULL, 0x00FF4040);
      Deger (L"ADRES LO=", (UINT32)T[1], NULL, 0x00FF4040);
      T[2] = 3;
    } else {
      EFI_MEMORY_DESCRIPTOR *H;
      UINTN                  Bo, An, Td, I;
      UINT32                 Sr;
      UINT64                 Adet;
      UINT32                 Bulunan;

      H       = NULL;
      Bo      = 0;
      Adet    = 0;
      Bulunan = 0xFFFFFFFF;

      T[0] = 0x0000004152415419ULL;
      T[1] = 0;
      T[2] = 1;
      T[3] = 0;

      gBS->GetMemoryMap (&Bo, H, &An, &Td, &Sr);
      Bo += 8 * Td;

      if (!EFI_ERROR (gBS->AllocatePool (EfiLoaderData, Bo, (VOID **)&H)) &&
          !EFI_ERROR (gBS->GetMemoryMap (&Bo, H, &An, &Td, &Sr))) {
        for (I = 0; (I < Bo / Td) && (Bulunan == 0xFFFFFFFF); I++) {
          EFI_MEMORY_DESCRIPTOR *D;
          UINT64                 A, Son;

          D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)H + I * Td);
          if (D->Type != EfiConventionalMemory) {
            continue;
          }

          Son = D->PhysicalStart + D->NumberOfPages * 4096;

          for (A = D->PhysicalStart; A + 8 <= Son; A += 4096) {
            volatile UINT64 *P = (volatile UINT64 *)(UINTN)A;
            UINT64           Eski;

            //
            // ADRESI ONCE YAZ, SONRA DOKUN. Erisim cihazi sifirlarsa
            // geriye kalan tek sey bu kayit.
            //
            T[1] = A;

            Eski = *P;
            *P   = 0xB2B2B2B2B2B2B2B2ULL;
            *P   = Eski;
            Adet++;

            //
            // Her 1 MB'ta bir GIC'e bak. Her sayfada bakmak taramayi
            // dakikalara cikarirdi ve 1 MB zaten daraltmak icin fazlasiyla
            // yeterli.
            //
            if ((Adet & 0xFF) == 0) {
              for (K = 1; K < 32; K++) {
                UINT32 Yeni = G[K] & ~Ilk[K];

                if (Yeni != 0) {
                  UINTN B;

                  for (B = 0; B < 32; B++) {
                    if ((Yeni >> B) & 1) {
                      Bulunan = (UINT32)(K * 32 + B);
                      break;
                    }
                  }
                  break;
                }
              }

              if (Bulunan != 0xFFFFFFFF) {
                break;
              }
            }
          }
        }

        gBS->FreePool (H);
      }

      T[3] = Adet;
      T[2] = 2;

      if (Bulunan != 0xFFFFFFFF) {
        Yaz (L"*** KESME KALKTI ***", 0x0040FF40);
        Deger (L"INTID   =", Bulunan,
               (Bulunan >= 382 && Bulunan <= 404) ? L"PPMPU" :
               (Bulunan >= 84  && Bulunan <= 160) ? L"ITMON" : L"BASKA",
               0x0040FF40);
        Deger (L"ADRES HI=", (UINT32)(T[1] >> 32), NULL, 0x0040FF40);
        Deger (L"ADRES LO=", (UINT32)T[1], NULL, 0x0040FF40);
      } else {
        Yaz (L"TARAMA TEMIZ, KESME YOK", 0x00808080);
      }

      Deger (L"SAYFA   =", (UINT32)Adet, NULL, 0x00808080);
    }

    //
    // Tarama olmasa bile acilista zaten bekleyen bir kesme var mi?
    // UEFI kesmeleri acmiyor, yani buradaki her bit "kimsenin
    // temizlemedigi bir kaynak" demek.
    //
    //
    // ACILISTA ZATEN BEKLEYEN SPI'LAR - BU TURUN ASIL BULGUSU.
    //
    // Ilk olcumde sekiz tane cikti. UEFI kesmeleri hic acmiyor, dolayisiyla
    // buradaki her bit "kimsenin temizlemedigi, kaynagi hala isaret eden bir
    // kesme" demek. Windows kesmeleri acar acmaz bunlarin her biri aday bir
    // firtina kaynagi - hele PPMPU gibi yalnizca EL3'un temizleyebildigi bir
    // kaynaksa Windows'un yapabilecegi hicbir sey yok.
    //
    // Sayiyi degil NUMARALARI yaziyoruz: "sekiz tane var" hicbir yere
    // goturmuyor, "INTID 382" dogrudan DTB'deki dugumu veriyor.
    //
    {
      UINT32 Kac = 0;

      //
      // NUMARALAR TEK SEFER OKUNDU, ARTIK SADECE SAYI.
      //
      // Kimlikleri DTB'den cikti ve degismiyor: 39 adv_tracer, 43 speedy,
      // 183/184 decon, 190 dsim, 198 dpp. Alti satiri her acilista
      // tekrarlamak ekranda yer yiyor; degisirse sayi degisir.
      //
      for (K = 1; K < 32; K++) {
        UINT32 V = G[K];

        while (V != 0) {
          Kac++;
          V &= (V - 1);
        }
      }

      Deger (L"BEKLEYEN=", Kac, L"SPI", 0x0060C0FF);
    }
  }
#endif

  //
  // B2LGicSusDxe: ExitBootServices aninda SPI'lar susturuldu mu?
  //
  // Bu, "Windows'a devretmeden once kesmeleri kapattik" iddiasinin tek
  // kaniti. Onceki acilistan kaliyor cunku olay Windows'a gecerken
  // atesleniyor - bu turdaki kayit, bir onceki acilisin devir anini
  // anlatiyor.
  //
  {
    UINT64 *Q = (UINT64 *)(UINTN)GIC_ONCEKI;

    if (Q[0] == 0x0000005143494747ULL) {
      Deger (L"SUS ONCE=", (UINT32)Q[1], NULL, 0x0060C0FF);
      Deger (L"SUS SONRA=", (UINT32)Q[2],
             (Q[2] == 0) ? L"TEMIZ" : L"KALDI",
             (Q[2] == 0) ? 0x0040FF40 : 0x00FF4040);

      //
      // ACIK: susturma yazisi GECTI mi? Sifirsa gecti ve bekleyen bitler
      // kaynaktan geri geliyor demektir. Sifirdan buyukse yazi hic
      // gecmemis, yani o kesmeler guvenli tarafa ait ve bizim
      // dokunmamiza izin yok.
      //
      // GRUP1: Non-secure olarak okunan SPI sayisi. Toplamdan cok
      // kucukse cogu kesme guvenli tarafta demektir.
      //
      Deger (L"ACIK    =", (UINT32)Q[3],
             (Q[3] == 0) ? L"YAZI GECTI" : L"YAZI GECMEDI",
             (Q[3] == 0) ? 0x0040FF40 : 0x00FFFF40);
      Deger (L"GRUP1   =", (UINT32)Q[4], NULL, 0x0060C0FF);
    } else {
      Yaz (L"GIC SUSTURMA: DEVIR YOK", 0x00808080);
    }
  }

  mSatir += 12;
  Yaz (L"FOTOGRAFLA, SONRA TUSA BAS", 0x00FFFFFF);

  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);

  return EFI_SUCCESS;
}
