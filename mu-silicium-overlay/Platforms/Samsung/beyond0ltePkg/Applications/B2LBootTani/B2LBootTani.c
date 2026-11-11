/** @file
  B2LBootTani - bootmgfw.efi icin salt UEFI ilerleme ekrani.

  Bu uygulama fiziksel tani kayitlarini, MMIO'yu veya ayrilmis RAM'i okumaz.
  Yalnizca yuklendigi FAT birimindeki BCD/bootmgfw dosyalarini, GOP
  framebuffer'ini ve UEFI olaylarini kullanir.

  Sag ustteki isaret:
    mavi/camgobegi hareket  - Boot Services zamanlayicisi isliyor
    sari                    - ExitBootServices olayi goruldu
    kirmizi                 - bootmgfw StartImage'dan geri dondu
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiImage.h>
#include <Protocol/HiiFont.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#define OLCEK       4U
#define KENAR       24U
#define NABIZ_EN    128U
#define NABIZ_BOY   48U

#define BCD_YOLU    L"\\EFI\\Microsoft\\Boot\\BCD"
#define BOOT_YOLU   L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_HII_FONT_PROTOCOL        *mFont;
STATIC UINT32                       *mFb;
STATIC UINT32                        mSatirBasi;
STATIC UINT32                        mSatir;
STATIC CHAR16                        mTampon[48];
STATIC volatile UINT32               mTik;
STATIC volatile BOOLEAN              mEbsGoruldu;

STATIC
VOID
Dikdortgen (
  IN UINT32 X,
  IN UINT32 Y,
  IN UINT32 Genislik,
  IN UINT32 Yukseklik,
  IN UINT32 Renk
  )
{
  UINT32 I;
  UINT32 J;
  UINT32 EkranEn;
  UINT32 EkranBoy;

  if ((mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
    return;
  }

  EkranEn  = mGop->Mode->Info->HorizontalResolution;
  EkranBoy = mGop->Mode->Info->VerticalResolution;
  mFb      = (UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
  mSatirBasi = mGop->Mode->Info->PixelsPerScanLine;

  if ((X >= EkranEn) || (Y >= EkranBoy)) {
    return;
  }

  if (Genislik > EkranEn - X) {
    Genislik = EkranEn - X;
  }
  if (Yukseklik > EkranBoy - Y) {
    Yukseklik = EkranBoy - Y;
  }

  for (J = 0; J < Yukseklik; J++) {
    for (I = 0; I < Genislik; I++) {
      mFb[(Y + J) * mSatirBasi + X + I] = Renk;
    }
  }
}

STATIC
VOID
EFIAPI
Nabiz (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  UINT32 X;
  UINT32 Renk;

  (VOID)Event;
  (VOID)Context;

  if (mEbsGoruldu || (mGop == NULL) || (mGop->Mode == NULL) ||
      (mGop->Mode->Info == NULL) ||
      (mGop->Mode->Info->HorizontalResolution < NABIZ_EN + 16U)) {
    return;
  }

  mTik++;
  X = mGop->Mode->Info->HorizontalResolution - NABIZ_EN - 16U;
  Dikdortgen (X, 16U, NABIZ_EN, NABIZ_BOY, 0x00000000U);
  Renk = ((mTik & 1U) == 0U) ? 0x0060C0FFU : 0x00FFFF40U;
  Dikdortgen (X + 8U + (mTik % 5U) * 20U, 24U, 16U, 32U, Renk);
}

STATIC
VOID
EFIAPI
ExitBootServicesGoruldu (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  UINT32 X;

  (VOID)Event;
  (VOID)Context;

  mEbsGoruldu = TRUE;
  if ((mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL) ||
      (mGop->Mode->Info->HorizontalResolution < NABIZ_EN + 16U)) {
    return;
  }

  X = mGop->Mode->Info->HorizontalResolution - NABIZ_EN - 16U;
  Dikdortgen (X, 16U, NABIZ_EN, NABIZ_BOY, 0x00FFFF40U);
}

STATIC
VOID
Karakter (
  IN CHAR16 Ch,
  IN UINT32 X,
  IN UINT32 Y,
  IN UINT32 Renk
  )
{
  EFI_IMAGE_OUTPUT *Blt;
  UINT32            Px;
  UINT32            Py;
  UINT32            I;
  UINT32            J;

  Blt = NULL;
  if (EFI_ERROR (mFont->GetGlyph (mFont, Ch, NULL, &Blt, NULL)) || (Blt == NULL)) {
    return;
  }

  for (Py = 0; Py < Blt->Height; Py++) {
    for (Px = 0; Px < Blt->Width; Px++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P;

      P = &Blt->Image.Bitmap[Py * Blt->Width + Px];
      if ((P->Red | P->Green | P->Blue) == 0) {
        continue;
      }

      for (J = 0; J < OLCEK; J++) {
        for (I = 0; I < OLCEK; I++) {
          mFb[(Y + Py * OLCEK + J) * mSatirBasi + X + Px * OLCEK + I] = Renk;
        }
      }
    }
  }

  if (Blt->Image.Bitmap != NULL) {
    gBS->FreePool (Blt->Image.Bitmap);
  }
  gBS->FreePool (Blt);
}

STATIC
VOID
Yaz (
  IN CHAR16 *Metin,
  IN UINT32  Renk
  )
{
  UINT32 X;

  X = KENAR;
  while (*Metin != L'\0') {
    Karakter (*Metin, X, mSatir, Renk);
    X += 8U * OLCEK;
    Metin++;
  }
  mSatir += 19U * OLCEK + 4U;
}

STATIC
VOID
Hex32 (
  OUT CHAR16 *Hedef,
  IN  UINT32  Deger
  )
{
  INTN I;

  for (I = 7; I >= 0; I--) {
    UINT8 Nibble;

    Nibble   = (UINT8)(Deger & 0xFU);
    Hedef[I] = (CHAR16)((Nibble < 10U) ? (L'0' + Nibble) : (L'A' + Nibble - 10U));
    Deger >>= 4;
  }
}

STATIC
VOID
DurumYaz (
  IN CHAR16 *Onek,
  IN UINT32  Deger,
  IN UINT32  Renk
  )
{
  UINTN N;

  N = 0;
  while ((Onek[N] != L'\0') && (N < 32U)) {
    mTampon[N] = Onek[N];
    N++;
  }
  Hex32 (&mTampon[N], Deger);
  mTampon[N + 8U] = L'\0';
  Yaz (mTampon, Renk);
}

STATIC
VOID
EkraniTemizle (
  VOID
  )
{
  mFb        = (UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
  mSatirBasi = mGop->Mode->Info->PixelsPerScanLine;
  mSatir     = KENAR;
  SetMem32 (
    mFb,
    (UINTN)mSatirBasi * mGop->Mode->Info->VerticalResolution * sizeof (UINT32),
    0x00000000U
    );
}

STATIC
BOOLEAN
AyniBirimdeDosyaVar (
  IN EFI_HANDLE DeviceHandle,
  IN CHAR16    *Yol
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Kok;
  EFI_FILE_PROTOCOL               *Dosya;
  BOOLEAN                          Var;

  Fs    = NULL;
  Kok   = NULL;
  Dosya = NULL;
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status) || (Fs == NULL)) {
    return FALSE;
  }

  Status = Fs->OpenVolume (Fs, &Kok);
  if (EFI_ERROR (Status) || (Kok == NULL)) {
    return FALSE;
  }

  Status = Kok->Open (Kok, &Dosya, Yol, EFI_FILE_MODE_READ, 0);
  Var = (BOOLEAN)(!EFI_ERROR (Status) && (Dosya != NULL));
  if (Var) {
    Dosya->Close (Dosya);
  }
  Kok->Close (Kok);
  return Var;
}

STATIC
VOID
TusBekle (
  VOID
  )
{
  EFI_INPUT_KEY Key;
  UINTN         Index;

  gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
}

EFI_STATUS
EFIAPI
B2LBootTaniEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_LOADED_IMAGE_PROTOCOL *Kendi;
  EFI_DEVICE_PATH_PROTOCOL  *BootYolu;
  EFI_HANDLE                 BootImage;
  EFI_EVENT                  NabizEvent;
  EFI_EVENT                  EbsEvent;

  (VOID)SystemTable;
  Kendi      = NULL;
  BootYolu   = NULL;
  BootImage  = NULL;
  NabizEvent = NULL;
  EbsEvent   = NULL;
  mTik       = 0;
  mEbsGoruldu = FALSE;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Kendi
                  );
  if (EFI_ERROR (Status) || (Kendi == NULL) ||
      !AyniBirimdeDosyaVar (Kendi->DeviceHandle, BCD_YOLU) ||
      !AyniBirimdeDosyaVar (Kendi->DeviceHandle, BOOT_YOLU)) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->LocateProtocol (
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  (VOID **)&mGop
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mFont);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EkraniTemizle ();
  Yaz (L"=== BEYOND2LTE BOOT TANI ===", 0x00FFFFFFU);
  Yaz (L"SURUM 34", 0x00808080U);
  Yaz (L"BCD VE BOOTMGFW BULUNDU", 0x0040FF40U);
  Yaz (L"KORUMALI BELLEK: ERISIM YOK", 0x0040FF40U);
  Yaz (L"MAVI NABIZ: BOOT SERVICES CANLI", 0x0060C0FFU);
  Yaz (L"SARI: EXIT BOOT SERVICES", 0x00FFFF40U);
  Yaz (L"KIRMIZI: BOOTMGFW GERI DONDU", 0x00FF4040U);
  Yaz (L"FOTOGRAFLA, SONRA TUSA BAS", 0x00FFFFFFU);
  TusBekle ();

  BootYolu = FileDevicePath (Kendi->DeviceHandle, BOOT_YOLU);
  if (BootYolu == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Hata;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, BootYolu, NULL, 0, &BootImage);
  gBS->FreePool (BootYolu);
  BootYolu = NULL;
  if (EFI_ERROR (Status)) {
    goto Hata;
  }

  EkraniTemizle ();
  Yaz (L"=== BOOTMGFW CALISIYOR ===", 0x00FFFFFFU);
  Yaz (L"SAG USTTEKI NABZI IZLE", 0x0060C0FFU);

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  Nabiz,
                  NULL,
                  &NabizEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Hata;
  }
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  ExitBootServicesGoruldu,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &EbsEvent
                  );
  if (EFI_ERROR (Status)) {
    goto Hata;
  }
  Status = gBS->SetTimer (NabizEvent, TimerPeriodic, 10000000ULL);
  if (EFI_ERROR (Status)) {
    goto Hata;
  }
  Nabiz (NabizEvent, NULL);
  Status = gBS->StartImage (BootImage, NULL, NULL);

Hata:
  if (NabizEvent != NULL) {
    gBS->SetTimer (NabizEvent, TimerCancel, 0);
    gBS->CloseEvent (NabizEvent);
  }
  if (EbsEvent != NULL) {
    gBS->CloseEvent (EbsEvent);
  }
  if (BootImage != NULL) {
    gBS->UnloadImage (BootImage);
  }

  EkraniTemizle ();
  Dikdortgen (
    mGop->Mode->Info->HorizontalResolution - NABIZ_EN - 16U,
    16U,
    NABIZ_EN,
    NABIZ_BOY,
    0x00FF4040U
    );
  Yaz (L"*** BOOTMGFW DONDU VEYA BASLAMADI ***", 0x00FF4040U);
  DurumYaz (L"STATUS HI=", (UINT32)((UINT64)Status >> 32), 0x00FF4040U);
  DurumYaz (L"STATUS LO=", (UINT32)Status, 0x00FF4040U);
  Yaz (L"EKRANI FOTOGRAFLA", 0x00FFFFFFU);
  TusBekle ();
  return Status;
}
