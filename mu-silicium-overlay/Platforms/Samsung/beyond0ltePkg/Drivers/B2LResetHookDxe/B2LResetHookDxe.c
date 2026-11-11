/** @file
  B2LResetHookDxe - Windows'un cagirdigi ResetSystem'i yakalar.

  ----------------------------------------------------------------------------
  NEDEN
  ----------------------------------------------------------------------------
  WinPE cekirdegi basliyor, donen daire ~12 saniye donuyor, sonra cihaz
  sessizce yeniden basliyor. Ekran olu, Windows'un butun log kanallari
  kapali (sos sessiz, bootstat.dat yazilamiyor - UFS LBA 0 aygit tarafinda
  reddediliyor), UART yok.

  PMU RST_STAT olcumu belirleyici oldu:

      temiz yeniden baslatma (kurtarma menusunden)  -> 0x20000000
      Windows coktukten sonra                       -> 0x20000000

  Ayni. Watchdog biti yok, soguk sifirlama biti yok. Yani sifirlamayi
  DONANIM zorlamiyor, YAZILIM istiyor. ARM64 UEFI'de yazilimin sifirlama
  istemesinin tek yolu var: EFI_RUNTIME_SERVICES.ResetSystem.

  Ve ResetSystem BIZIM kodumuz. ExitBootServices'ten sonra da bizde kaliyor;
  Windows onu cagirmak zorunda. Yani Windows'u tam kacis aninda yakalayabiliriz.

  ----------------------------------------------------------------------------
  EKRANA NEDEN YAZMIYORUZ
  ----------------------------------------------------------------------------
  Kanca SetVirtualAddressMap'ten sonra, Windows'un sayfa tablolariyla
  calisiyor. gST->ConOut yok, DEBUG yok, gBS yok. Framebuffer'a dogrudan
  yazmak icin onu runtime bolge olarak isaretleyip isaretcisini cevirmek
  gerekirdi - yapilabilir ama bir de kendi font rutinimizi yazmak demek.

  Daha ucuzu var: DRAM sicak sifirlamayi atlatiyor. Android'in ramoops'u tam
  olarak buna dayaniyor ve bu SoC'de calistigi biliniyor. Kanca kaydi sabit
  bir fiziksel adrese yaziyor, gercek sifirlamayi yapiyor; sonraki acilista
  EFI Shell zaten calisan "dmem" komutuyla okuyor. Yeni bir cikti yolu icat
  etmiyoruz, elimizdekini kullaniyoruz.

  ----------------------------------------------------------------------------
  BELLEK YERLESIMI  (0xCF000000, MemoryMapLib'de "B2L Log")
  ----------------------------------------------------------------------------
    +0x00  BU acilisin kaydi     - kanca buraya yazar
    +0x40  imza + acilis sayaci  - surucunun yuklendigini gosterir
    +0x80  ONCEKI acilisin kaydi - Shell'in okudugu yer

  Surucu her acilista +0x00'i +0x80'e tasiyip +0x00'i temizliyor. Boylece
  Shell'in gordugu kayit her zaman BIR ONCEKI acilisin, yani cokusun kaydi.
  Kanca ile Shell ayni acilista calismadigi icin bu kaydirma sart.

  ----------------------------------------------------------------------------
  OKUMA
  ----------------------------------------------------------------------------
    dmem 0xCF000000 192

  +0x80'de "42 32 4C 52" (B2LR) goruyorsan Windows gercekten ResetSystem
  cagirdi ve devaminda ResetType ile ResetStatus duruyor.
  Gormuyorsan kanca hic cagrilmadi: sifirlama ResetSystem'den gecmiyor,
  daha alt bir seviyede (istisna, PSCI'ye dogrudan SMC, ya da guc devresi).
  IKI SONUC DA BILGI; su an hangisi oldugunu bilmiyoruz.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/EventGroup.h>

//
// ==========================================================================
// KILITLENME MODU - ACIK, AMA ARTIK YAZMADAN SONRA
// ==========================================================================
// 1. TUR: kanca kaydi bos geldi (+0x80 bastan sona FF), ama +0x40'ta "B2LI"
//         vardi ve acilis sayaci 0x403 -> 0x404 diye tam bir artmisti.
//         Surucu her acilista calisiyor, bolge sifirlamayi atlatiyor;
//         negatif sonuc guvenilir.
//
// 2. TUR: kanca HICBIR BELLEGE DOKUNMADAN en basta kilitlenecek sekilde
//         kuruldu. Cihaz YINE yeniden basladi. Bu, "kanca cagrildi ama
//         yazarken fault verdi" ihtimalini ELEDI.
//
// KALAN TEK ACIKLAMA: Windows ResetSystem'i hic cagirmiyor. Linux'un
// verdigi ipucu bunu tamamliyor:
//     psci: PSCIv65535.65535 detected in firmware.
// yani PSCI_VERSION 0xFFFFFFFF donuyor (fonksiyon yok) ama CPU_ON
// calisiyor - Samsung'un EL3 monitoru PSCI'yi kismen uygulamis. Windows
// FADT'deki PSCI_COMPLIANT bayragini gorup sifirlama icin dogrudan SMC ile
// PSCI SYSTEM_RESET cagiriyor olmali, UEFI'ye hic ugramadan.
//
// 3. TUR: FACP'de PSCI_COMPLIANT sifirlandi. Windows'un elinde sifirlamak
//         icin tek yol kaliyor: bizim ResetSystem'imiz.
//
// Kilitlenme artik kayit YAZILDIKTAN SONRA, cunku kayit lazim:
//   cihaz DONARSA   -> kanca ates etti. Guc + ses kisma ile kapat, ac;
//                      +0x80 dokumunde ResetStatus duruyor.
//   yine sifirlarsa -> kanca yine cagrilmadi, sifirlama Windows
//                      yaziliminin disindan geliyor.
//
// Ayrim netlestikten sonra 0 yap: cihaz normal sifirlanir, kayit yine kalir.
//
#define B2L_HOOK_HANG     1

#define B2L_LOG_BASE      0xCF000000ULL
#define B2L_OFF_CURRENT   0x00
#define B2L_OFF_INFO      0x40
#define B2L_OFF_PREVIOUS  0x80

//
// Little-endian yazildiginda dmem ciktisinda "42 32 4C 52" = "B2LR" okunur.
//
#define B2L_MAGIC_RESET   0x524C3242
#define B2L_MAGIC_INIT    0x494C3242  // "B2LI"

#pragma pack(1)
typedef struct {
  UINT32 Magic;         // +0x00  B2LR
  UINT32 Seq;           // +0x04  kanca kac kez cagrildi
  UINT32 ResetType;     // +0x08  0=Cold 1=Warm 2=Shutdown 3=PlatformSpecific
  UINT32 Reserved;      // +0x0C
  UINT64 ResetStatus;   // +0x10  EFI_STATUS - ASIL ARADIGIMIZ SEY
  UINT64 DataSize;      // +0x18
  UINT8  Data[32];      // +0x20  ResetData, genelde UCS-2 metin
} B2L_RESET_RECORD;     // 64 bayt
#pragma pack()

STATIC volatile UINT8      *mLog      = (volatile UINT8 *)(UINTN)B2L_LOG_BASE;
STATIC EFI_RESET_SYSTEM     mOldReset = NULL;
STATIC EFI_EVENT            mVirtualAddressChangeEvent = NULL;

/**
  ResetSystem kancasi. ExitBootServices SONRASI da calisir.

  BURADA NE YAPILAMAZ: gBS, DEBUG, protokol cagrisi, donusturulmemis
  isaretci. Yalnizca kendi imajimiz, mLog ve mOldReset kullanilabilir -
  ucu de SetVirtualAddressMap'te cevriliyor.
**/
STATIC
VOID
EFIAPI
B2LResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID           *ResetData OPTIONAL)
{
  volatile B2L_RESET_RECORD *Rec;
  UINTN                      Copy;
  UINTN                      Index;

  Rec = (volatile B2L_RESET_RECORD *)(mLog + B2L_OFF_CURRENT);

  Rec->Seq         = Rec->Seq + 1;
  Rec->ResetType   = (UINT32)ResetType;
  Rec->Reserved    = 0;
  Rec->ResetStatus = (UINT64)ResetStatus;
  Rec->DataSize    = (UINT64)DataSize;

  //
  // CopyMem cagirmiyoruz: BaseMemoryLib'in runtime'da guvenli oldugundan
  // emin olmak yerine elle kopyalamak daha ucuz.
  //
  Copy = DataSize;
  if (Copy > sizeof (Rec->Data)) {
    Copy = sizeof (Rec->Data);
  }

  for (Index = 0; Index < sizeof (Rec->Data); Index++) {
    if ((ResetData != NULL) && (Index < Copy)) {
      Rec->Data[Index] = ((volatile UINT8 *)ResetData)[Index];
    } else {
      Rec->Data[Index] = 0;
    }
  }

  //
  // Imza EN SON. Yarim kalmis bir kayit gecerli gorunmesin.
  //
  Rec->Magic = B2L_MAGIC_RESET;

#if B2L_HOOK_HANG
  //
  // KAYIT YAZILDIKTAN SONRA KILITLEN, gercek sifirlamayi yapma.
  //
  // Kilitlenme artik yazmadan ONCE degil SONRA. Sirasi onemli: "hicbir
  // seye dokunmadan hemen kilitlen" turu zaten kosuldu ve cihaz yine
  // yeniden basladi, yani kancanin cagrilip yazarken fault vermesi
  // ihtimali ELENDI. Geriye kancanin hic cagrilmamasi kalmisti.
  //
  // Bu yuzden simdi once yaziyoruz (kayit lazim), sonra kilitleniyoruz:
  //   cihaz DONARSA  -> kanca ates etti. Guc + ses kisma ile kapat, ac,
  //                     ekrandaki +0x80 dokumunde ResetStatus duruyor.
  //   yine sifirlarsa-> kanca yine cagrilmadi.
  //
  while (TRUE) {
  }
#endif

  //
  // Gercek sifirlamayi yap. Cihazi burada kilitlemiyoruz: kaydi okumak
  // icin bir sonraki acilisa ihtiyacimiz var.
  //
  if (mOldReset != NULL) {
    mOldReset (ResetType, ResetStatus, DataSize, ResetData);
  }

  //
  // Buraya normalde hic gelinmez.
  //
  while (TRUE) {
  }
}

/**
  SetVirtualAddressMap. Runtime'da kullanacagimiz her isaretci burada cevrilir;
  biri unutulursa kanca ilk calistiginda fault verir ve hicbir sey ogrenemeyiz.
**/
STATIC
VOID
EFIAPI
B2LVirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID      *Context)
{
  EfiConvertPointer (0x0, (VOID **)&mLog);
  EfiConvertPointer (0x0, (VOID **)&mOldReset);
}

EFI_STATUS
EFIAPI
B2LResetHookEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS                 Status;
  volatile B2L_RESET_RECORD *Cur  = (volatile B2L_RESET_RECORD *)(mLog + B2L_OFF_CURRENT);
  volatile B2L_RESET_RECORD *Prev = (volatile B2L_RESET_RECORD *)(mLog + B2L_OFF_PREVIOUS);
  volatile UINT32           *Info = (volatile UINT32 *)(mLog + B2L_OFF_INFO);
  UINTN                      Index;

  //
  // Onceki acilisin kaydini kenara al, sonra bu acilis icin temizle.
  // Kanca ile Shell ayni acilista calismadigi icin bu kaydirma sart:
  // Shell'in okudugu +0x80 her zaman COKUSUN kaydidir.
  //
  if (Cur->Magic == B2L_MAGIC_RESET) {
    for (Index = 0; Index < sizeof (B2L_RESET_RECORD); Index++) {
      ((volatile UINT8 *)Prev)[Index] = ((volatile UINT8 *)Cur)[Index];
    }

    DEBUG ((
      EFI_D_ERROR,
      "B2L RESET: onceki acilis ResetSystem'den gecti - type=%u status=0x%lx\n",
      Prev->ResetType,
      Prev->ResetStatus));
  } else {
    DEBUG ((EFI_D_ERROR, "B2L RESET: onceki acilista kanca kaydi yok\n"));
  }

  for (Index = 0; Index < sizeof (B2L_RESET_RECORD); Index++) {
    ((volatile UINT8 *)Cur)[Index] = 0;
  }

  Info[0] = B2L_MAGIC_INIT;
  Info[1] = Info[1] + 1;

  //
  // Kancayi tak. Runtime Services basligindaki CRC32 yeniden hesaplanmali;
  // aksi halde tabloyu dogrulayan yukleyiciler bozuk gorur.
  //
  mOldReset        = gRT->ResetSystem;
  gRT->ResetSystem = B2LResetSystem;

  gRT->Hdr.CRC32 = 0;
  gBS->CalculateCrc32 ((VOID *)gRT, gRT->Hdr.HeaderSize, &gRT->Hdr.CRC32);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  B2LVirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mVirtualAddressChangeEvent);
  ASSERT_EFI_ERROR (Status);

  DEBUG ((EFI_D_ERROR, "B2L RESET: ResetSystem kancasi takildi, kayit 0x%lx\n", B2L_LOG_BASE));

  return EFI_SUCCESS;
}
