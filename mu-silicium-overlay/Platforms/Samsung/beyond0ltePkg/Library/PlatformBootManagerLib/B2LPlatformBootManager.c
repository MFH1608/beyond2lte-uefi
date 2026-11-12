/** @file
  B2L platform boot manager.

  Normalde "isletim sistemi bulunamadi" durumunda PlatformBootManagerUnableToBoot
  ekrana logo basip 10 sn sonra cihazi kapatir. Bu dosya o davranisi, S10e
  donanim self-test ekraniyla degistirir.

  Neden: cihazda harf ureten bir klavye yok (keypad yalnizca Up/Down/Enter/Esc
  veriyor) ve FAT/ESP bolumu bulunmadigi icin UEFI Shell'e komut yazilamiyor.
  Bu ekran dogrulama bilgilerini dogrudan framebuffer'a basar; kullanici
  yalnizca ekrani fotograflar.

  Cikis: Bixby'yi basili tutup Volume Up/Down'a basmak (Left/Right ScanCode'u
  uretir) -> ekrandan cikilir, ardindan normal "no OS" yolu cihazi kapatir.
**/

#include <Uefi.h>

#include <Library/DeviceBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiImage.h>
#include <Protocol/HiiFont.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>

//
// S10e'de son DRAM banki 0x980000000'da bitiyor (0x80000000..0x100000000 +
// 0x880000000..0x980000000 = 6 GiB). Bu adresin ustunde, var olmayan DRAM'i
// tarif eden BIR descriptor OLMAMALI; bu satir MemoryMapLib'in dogru
// calistigini gosterir. Yuksek adresli MMIO (varsa) zararsiz sayilir.
//
#define B2L_MMIO_LIMIT  0x980000000ULL

//
// B2LTouchDxe'nin yayinladigi teshis protokolu (bkz.
// Drivers/B2LTouchDxe/B2LTouchDiag.h). Bu paketin .dec dosyasi build'e
// bagli olmadigi icin Include yolu eklenemiyor; GUID ve yerlesim burada
// yerel olarak tekrarlanir. Iki taraf AYNI kalmak zorundadir.
//
#define B2L_TOUCH_DIAG_PROTOCOL_GUID \
  { 0x9c1f4b52, 0x8d36, 0x4c01, { 0xa7, 0x62, 0x51, 0x0e, 0x9b, 0x3d, 0x77, 0xc4 } }

#define B2L_TOUCH_DIAG_EVENT_SIZE  8

#define B2L_TOUCH_DIAG_SIGNATURE  0x31474442U   // "BDG1"

//
// Surucu ile ayni asama numaralari (bkz. B2LTouchDiag.h). "done" asamasina
// ulastiysa entry/probe/bring-up zinciri tamamlanmis demektir.
//
#define B2L_TOUCH_STAGE_DONE      7

typedef struct {
  UINT32  Signature;
  UINT32  Size;
  UINT32  Stage;
  UINT32  PmicLocate;
  UINT64  EntryStatus;
  UINT64  AbsPointer;
  UINT64  LastStatus;
  UINT32  Polls;
  UINT32  Empty;
  UINT32  Errors;
  UINT32  StatusEvents;
  UINT32  CoordEvents;
  UINT32  Ignored;
  UINT32  LdoUv[2];
  UINT32  IntStatus;
  UINT32  TransStatus;
  UINT32  ScanAck[4];
  UINT8   DeviceId[5];
  UINT8   Integrity;
  UINT8   BootStatus;
  UINT8   ResetCalls;
  UINT8   ProbeCalls;
  UINT8   PowerOk;
  UINT8   ScanDone;
  UINT8   LdoSetOk[2];
  UINT8   LastEvent[B2L_TOUCH_DIAG_EVENT_SIZE];
  UINT8   FnBefore[2];
  UINT8   FnAfter[2];
  UINT8   PowerMode;
  UINT8   BringUpOk;
  UINT8   Reserved[5];
} B2L_TOUCH_DIAG;

typedef struct {
  B2L_TOUCH_DIAG  Diag;
} B2L_TOUCH_DIAG_PROTOCOL;

STATIC CONST EFI_GUID  mB2LTouchDiagGuid = B2L_TOUCH_DIAG_PROTOCOL_GUID;

//
// Konsolun yazi tipi 8x19 pikselde sabittir (EFI_GLYPH_WIDTH/HEIGHT
// degistirilemez), bu yuzden 1080x2280 ekranda fotograftan okumak zor.
// B2LGoster'daki gibi glifleri HII font protokolunden alip OLCEK kat
// buyuterek dogrudan framebuffer'a ciziyoruz. Boylece self-test ekrani
// buyuk ve net okunur.
//
#define B2L_UI_OLCEK    3                 // glif basina 3x3 piksel blogu
#define B2L_UI_GLIF_W   8                 // HII varsayilan glif genisligi
#define B2L_UI_GLIF_H   19                // HII varsayilan glif yuksekligi
#define B2L_UI_SATIR    (B2L_UI_GLIF_H * B2L_UI_OLCEK + 4)
#define B2L_UI_KENAR    8                 // sol kenar boslugu

#define B2L_UI_BEYAZ    0x00FFFFFF
#define B2L_UI_GRI      0x00A0A0A0
#define B2L_UI_YESIL    0x0040FF40
#define B2L_UI_KIRMIZI  0x00FF4040
#define B2L_UI_SARI     0x00FFFF40
#define B2L_UI_MAVI     0x0060C0FF

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mUiGop;
STATIC EFI_HII_FONT_PROTOCOL         *mUiFont;
STATIC UINT32                        *mUiFb;      // framebuffer (32-bit piksel)
STATIC UINT32                         mUiStride;  // piksel/satir
STATIC UINT32                         mUiY;       // o anki cizim satiri
STATIC BOOLEAN                        mUiReady;

/**
  Tek glifi OLCEK kat buyutup framebuffer'a bas.
**/
STATIC
VOID
B2LUiGlyph (
  IN CHAR16  Ch,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Color
  )
{
  EFI_IMAGE_OUTPUT  *Blt;
  UINT32             HRes;
  UINT32             VRes;
  UINT32             Px;
  UINT32             Py;
  UINT32             I;
  UINT32             J;

  Blt = NULL;
  if (EFI_ERROR (mUiFont->GetGlyph (mUiFont, Ch, NULL, &Blt, NULL)) || (Blt == NULL)) {
    return;
  }

  HRes = mUiGop->Mode->Info->HorizontalResolution;
  VRes = mUiGop->Mode->Info->VerticalResolution;

  for (Py = 0; Py < Blt->Height; Py++) {
    for (Px = 0; Px < Blt->Width; Px++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *P;

      P = &Blt->Image.Bitmap[Py * Blt->Width + Px];

      //
      // HII glifi on plani acik renk veriyor; sifirdan farkli her piksel
      // "harfin parcasi" sayilir.
      //
      if ((P->Red | P->Green | P->Blue) == 0) {
        continue;
      }

      for (J = 0; J < B2L_UI_OLCEK; J++) {
        for (I = 0; I < B2L_UI_OLCEK; I++) {
          UINT32  Fx;
          UINT32  Fy;

          Fx = X + Px * B2L_UI_OLCEK + I;
          Fy = Y + Py * B2L_UI_OLCEK + J;
          if ((Fx < HRes) && (Fy < VRes)) {
            mUiFb[Fy * mUiStride + Fx] = Color;
          }
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
  Tek satir yaz ve asagi in.
**/
STATIC
VOID
B2LUiLine (
  IN CONST CHAR16  *Text,
  IN UINT32         Color
  )
{
  UINT32  X;

  X = B2L_UI_KENAR;
  while (*Text != L'\0') {
    B2LUiGlyph (*Text, X, mUiY, Color);
    X += B2L_UI_GLIF_W * B2L_UI_OLCEK;
    Text++;
  }

  mUiY += B2L_UI_SATIR;
}

/**
  Printf benzeri satir: metni bicimlendirip buyuk yaziyla cizer.
**/
STATIC
VOID
B2LUiPrint (
  IN UINT32         Color,
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR16   Buffer[160];

  VA_START (Marker, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  B2LUiLine (Buffer, Color);
}

/**
  Y0 satirindan ekranin altina kadar siyahla temizle.
**/
STATIC
VOID
B2LUiClearFrom (
  IN UINT32  Y0
  )
{
  UINT32  VRes;

  VRes = mUiGop->Mode->Info->VerticalResolution;
  if (Y0 >= VRes) {
    return;
  }

  SetMem32 (
    &mUiFb[Y0 * mUiStride],
    (UINTN)(VRes - Y0) * mUiStride * sizeof (UINT32),
    0x00000000
    );
}

STATIC
BOOLEAN
B2LIsMmio (
  IN EFI_MEMORY_TYPE  Type
  )
{
  return (Type == EfiMemoryMappedIO) || (Type == EfiMemoryMappedIOPortSpace);
}

STATIC
VOID
B2LDrawMemory (
  VOID
  )
{
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINTN                  Count;
  UINTN                  Index;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT64                 DramTotal;
  UINT64                 FreeRam;
  UINT64                 Highest;
  UINT64                 End;
  BOOLEAN                AboveLimit;

  MapSize = 0;
  Status  = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : GetMemoryMap probe error (%r)", Status);
    return;
  }

  MapSize += 8 * DescSize;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : allocation failed");
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : GetMemoryMap error (%r)", Status);
    FreePool (Map);
    return;
  }

  DramTotal  = 0;
  FreeRam    = 0;
  Highest    = 0;
  AboveLimit = FALSE;
  Count      = MapSize / DescSize;

  for (Index = 0; Index < Count; Index++) {
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);

    End = D->PhysicalStart + MultU64x64 (D->NumberOfPages, 4096);

    //
    // MMIO haritada yuksek adreslerde gorunebilir; onu DRAM saymadan
    // yalnizca RAM'i topla. S10e'de 6 GiB'in TAMAMI (rezerv dahil) tarif
    // edilmis olmali -> toplam 6144 MB cikar.
    //
    if (!B2LIsMmio (D->Type)) {
      if (D->PhysicalStart < B2L_MMIO_LIMIT) {
        DramTotal += MultU64x64 (D->NumberOfPages, 4096);
      }

      if (D->PhysicalStart >= B2L_MMIO_LIMIT) {
        AboveLimit = TRUE;
      }
    }

    if (D->Type == EfiConventionalMemory) {
      FreeRam += MultU64x64 (D->NumberOfPages, 4096);
    }

    if (End > Highest) {
      Highest = End;
    }
  }

  B2LUiPrint (B2L_UI_BEYAZ, L"RAM     : %u MB DRAM, %u MB free",
         (UINT32)DivU64x32 (DramTotal, 1024 * 1024),
         (UINT32)DivU64x32 (FreeRam, 1024 * 1024));
  B2LUiPrint (B2L_UI_BEYAZ, L"Memmap  : %u desc, max end 0x%lx", (UINT32)Count, Highest);
  B2LUiPrint (
    AboveLimit ? B2L_UI_KIRMIZI : B2L_UI_YESIL,
    L"RAM >=0x980000000  : %a",
    AboveLimit ? "PRESENT (WRONG)" : "none (ok)"
    );

  FreePool (Map);
}

STATIC
VOID
B2LDrawDevices (
  VOID
  )
{
  EFI_STATUS                    Status;
  UINTN                         HandleCount;
  UINTN                         Index;
  UINTN                         Shown;
  EFI_HANDLE                    *Handles;
  EFI_BLOCK_IO_PROTOCOL         *Bio;
  UINT64                        Bytes;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiBlockIoProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"Block IO: none (%r)", Status);
  } else {
    B2LUiPrint (B2L_UI_BEYAZ, L"Block IO: %u device", (UINT32)HandleCount);
    Shown = 0;
    for (Index = 0; (Index < HandleCount) && (Shown < 6); Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
      if (EFI_ERROR (Status) || (Bio->Media == NULL)) {
        continue;
      }

      Bytes = MultU64x64 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
      B2LUiPrint (B2L_UI_GRI, L"  #%u %u MB blk=%u %a%a",
             (UINT32)(Shown + 1),
             (UINT32)DivU64x32 (Bytes, 1024 * 1024),
             (UINT32)Bio->Media->BlockSize,
             Bio->Media->RemovableMedia ? "removable" : "fixed",
             Bio->Media->MediaPresent ? "" : " (no media)");
      Shown++;
    }
    FreePool (Handles);
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_GRI, L"FAT/FS  : 0 volume");
  } else {
    B2LUiPrint (B2L_UI_BEYAZ, L"FAT/FS  : %u volume", (UINT32)HandleCount);
    FreePool (Handles);
  }
}

STATIC
VOID
B2LDrawDisplay (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL)) {
    B2LUiPrint (B2L_UI_BEYAZ, L"Display : %ux%u (GOP mode %u)",
           Gop->Mode->Info->HorizontalResolution,
           Gop->Mode->Info->VerticalResolution,
           Gop->Mode->Mode);
  } else {
    B2LUiPrint (B2L_UI_KIRMIZI, L"Display : no GOP (%r)", Status);
  }
}

STATIC
VOID
B2LSelfTest (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Abs;
  EFI_ABSOLUTE_POINTER_STATE     TouchState;
  EFI_INPUT_KEY                  Key;
  B2L_TOUCH_DIAG_PROTOCOL        *TouchDiag;
  BOOLEAN                        HasTouch;
  BOOLEAN                        DiagOk;
  BOOLEAN                        Exit;
  EFI_STATUS                     TouchStatus;
  UINT32                         TouchPolls;
  UINT32                         TouchHits;
  UINT32                         DynTop;
  CHAR16                         LastKey[56];
  CHAR16                         LastTouch[80];

  HasTouch    = FALSE;
  DiagOk      = FALSE;
  Exit        = FALSE;
  Abs         = NULL;
  TouchStatus = EFI_NOT_READY;
  TouchPolls  = 0;
  TouchHits   = 0;
  TouchDiag   = NULL;
  DynTop      = 0;

  //
  // Buyuk yazi icin GOP + HII font gerekli. Ikisi de FV'de
  // (GraphicsConsoleDxe ve HiiDatabaseDxe); bulunamazsa konsol yazisina
  // dusmek yerine acikca bildir.
  //
  mUiReady = FALSE;
  Status   = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mUiGop);
  if (!EFI_ERROR (Status) && (mUiGop->Mode != NULL) && (mUiGop->Mode->Info != NULL)) {
    Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mUiFont);
    if (!EFI_ERROR (Status) && (mUiFont != NULL)) {
      mUiFb     = (UINT32 *)(UINTN)mUiGop->Mode->FrameBufferBase;
      mUiStride = mUiGop->Mode->Info->PixelsPerScanLine;
      mUiReady  = TRUE;
    }
  }

  if (!mUiReady) {
    Print (L"B2L self-test: GOP/HII font unavailable (big text disabled).\r\n");
    return;
  }

  B2LUiClearFrom (0);
  mUiY = B2L_UI_KENAR;

  UnicodeSPrint (LastKey, sizeof (LastKey), L"(not yet)");
  UnicodeSPrint (LastTouch, sizeof (LastTouch), L"(not yet)");

  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");
  B2LUiPrint (B2L_UI_BEYAZ, L" S10e (beyond0lte) - UEFI SELF TEST");
  B2LUiPrint (B2L_UI_GRI,   L" No ESP; verification screen. Photograph.");
  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");

  B2LDrawDisplay ();
  B2LDrawMemory ();
  B2LDrawDevices ();

  //
  // Surucu teshis protokolu. Once bunu arariz: hem entry asamasini gosterir
  // hem de kurdugu AbsolutePointer protokolunun isaretcisini tasir. UI
  // ConSplitter'in toplu (agregat) protokolu yerine DOGRUDAN bizim
  // protokolla konusmak zorunda: ConSplitter mode'u 0x10000 (65536) ve
  // bizim surucumuz oraya bagli degil.
  //
  // Signature/Size, iki modulde elle tutulan yerlesimin ayni oldugunu
  // dogrular; tutmazsa teshis alanlari okunmaz.
  //
  Status = gBS->LocateProtocol (
                  (EFI_GUID *)(VOID *)&mB2LTouchDiagGuid,
                  NULL,
                  (VOID **)&TouchDiag
                  );
  if (!EFI_ERROR (Status) &&
      (TouchDiag != NULL) &&
      (TouchDiag->Diag.Signature == B2L_TOUCH_DIAG_SIGNATURE) &&
      (TouchDiag->Diag.Size == (UINT32)sizeof (B2L_TOUCH_DIAG)))
  {
    DiagOk = TRUE;

    if (TouchDiag->Diag.AbsPointer != 0) {
      Abs      = (EFI_ABSOLUTE_POINTER_PROTOCOL *)(UINTN)TouchDiag->Diag.AbsPointer;
      HasTouch = TRUE;
    }
  } else {
    TouchDiag = NULL;
  }

  if (!HasTouch) {
    Status   = gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Abs);
    HasTouch = !EFI_ERROR (Status);
  }

  //
  // Surucu entry'de bring-up yapmisti; ama entry APRIORI sirasinda calisir ve
  // o an tsp_io/tsp_avdd raylarini acan PmicDxe yuklu olmayabilir, yani
  // panel I2C'ye hic yanit vermemis olabilir. Self-test baslarken tum DXE
  // suruculeri yuklendigi icin Reset(TRUE) bir kez daha guc + bring-up
  // dener. Basarisiz olsa da ekran calismaya devam eder.
  //
  if (HasTouch && (Abs->Reset != NULL)) {
    Abs->Reset (Abs, TRUE);
  }

  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");
  B2LUiPrint (B2L_UI_GRI,   L"VolUp=Up VolDown=Down Bixby=Enter");
  B2LUiPrint (B2L_UI_GRI,   L"Bixby(hold)=Esc  Exit: Bixby+Vol");
  B2LUiPrint (B2L_UI_GRI,   L"Touch.dx hit and Touch.ev co should grow");
  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");

  //
  // Statik bolge buraya kadar cizildi. Asagisi her karede temizlenip
  // yeniden cizilecek canli bolge.
  //
  DynTop = mUiY;

  for (;;) {
    //
    // Bu dongu kendi govdesinde gBS->Stall ile bekledigi icin TPL_CALLBACK
    // zamanlayicilari (ornegin touch yoklama) kendiliginden islemez.
    // Raise/RestoreTPL cifti bekleyen tum notify fonksiyonlarini pompalar.
    //
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));

    B2LUiClearFrom (DynTop);
    mUiY = DynTop;

    if ((gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        if (Key.ScanCode == SCAN_UP) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Up   -> SCAN_UP (0x01)");
        } else if (Key.ScanCode == SCAN_DOWN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Down -> SCAN_DOWN (0x02)");
        } else if (Key.ScanCode == SCAN_ESC) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby hold -> SCAN_ESC (0x17)");
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby short -> ENTER (0x0D)");
        } else if ((Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
          UnicodeSPrint (
            LastKey,
            sizeof (LastKey),
            L"Bixby+Vol -> %a (exit)",
            (Key.ScanCode == SCAN_LEFT) ? "LEFT" : "RIGHT"
            );
          Exit = TRUE;
        } else {
          UnicodeSPrint (
            LastKey,
            sizeof (LastKey),
            L"Scan=0x%04x Uni=0x%04x",
            Key.ScanCode,
            Key.UnicodeChar
            );
        }
      }
    }

    if (HasTouch) {
      //
      // Teshis: st=GetState sonucu, poll=kac kez cagrildi, hit=state donen
      // cagri sayisi. st surekli NOT_READY ise denetleyici olay uretmiyor;
      // poll artmiyorsa GetState hic cagrilmiyor demektir.
      //
      TouchPolls++;
      TouchStatus = Abs->GetState (Abs, &TouchState);
      if (!EFI_ERROR (TouchStatus)) {
        TouchHits++;
        UnicodeSPrint (
          LastTouch,
          sizeof (LastTouch),
          L"X=%u Y=%u Z=%u btn=0x%x",
          TouchState.CurrentX,
          TouchState.CurrentY,
          TouchState.CurrentZ,
          TouchState.ActiveButtons
          );
      }
    }

    //
    // Teshis blogu. Her satir surucunun doldurdugu bir alani gosterir; amac
    // tek flash'ta "I2C mi, guc mu, parser mi" ayrimini ekrandan okumak.
    // Sozlesme (Signature/Size) tutmazsa blok hic cizilmez.
    //
    if (DiagOk) {
      B2LUiPrint (
        (TouchDiag->Diag.Stage == B2L_TOUCH_STAGE_DONE) ? B2L_UI_YESIL : B2L_UI_KIRMIZI,
        L"Touch.ent: stage=%u",
        TouchDiag->Diag.Stage
        );
      B2LUiPrint (
        !EFI_ERROR ((EFI_STATUS)(UINTN)TouchDiag->Diag.EntryStatus) ? B2L_UI_YESIL : B2L_UI_SARI,
        L"Touch.ent: st=%r",
        (EFI_STATUS)(UINTN)TouchDiag->Diag.EntryStatus
        );
      B2LUiPrint (
        B2L_UI_BEYAZ,
        L"Touch.id : %02x %02x %02x %02x %02x",
        TouchDiag->Diag.DeviceId[0],
        TouchDiag->Diag.DeviceId[1],
        TouchDiag->Diag.DeviceId[2],
        TouchDiag->Diag.DeviceId[3],
        TouchDiag->Diag.DeviceId[4]
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.id : iv=%02x bs=%02x rc=%u pc=%u",
        TouchDiag->Diag.Integrity,
        TouchDiag->Diag.BootStatus,
        TouchDiag->Diag.ResetCalls,
        TouchDiag->Diag.ProbeCalls
        );
      B2LUiPrint (
        TouchDiag->Diag.BringUpOk ? B2L_UI_YESIL : B2L_UI_KIRMIZI,
        L"Touch.fn : be=%02x%02x af=%02x%02x ok=%u",
        TouchDiag->Diag.FnBefore[0],
        TouchDiag->Diag.FnBefore[1],
        TouchDiag->Diag.FnAfter[0],
        TouchDiag->Diag.FnAfter[1],
        TouchDiag->Diag.BringUpOk
        );
      B2LUiPrint (
        TouchDiag->Diag.PowerOk ? B2L_UI_YESIL : B2L_UI_KIRMIZI,
        L"Touch.pm : loc=%u set=%u%u pwr=%u",
        TouchDiag->Diag.PmicLocate,
        TouchDiag->Diag.LdoSetOk[0],
        TouchDiag->Diag.LdoSetOk[1],
        TouchDiag->Diag.PowerOk
        );
      B2LUiPrint (
        B2L_UI_BEYAZ,
        L"Touch.pm : io=%u av=%u uV",
        TouchDiag->Diag.LdoUv[0],
        TouchDiag->Diag.LdoUv[1]
        );
      B2LUiPrint (
        (TouchDiag->Diag.LastStatus == 0) ? B2L_UI_YESIL : B2L_UI_KIRMIZI,
        L"Touch.err: %r",
        (EFI_STATUS)(UINTN)TouchDiag->Diag.LastStatus
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.di : int=%08x tr=%08x",
        TouchDiag->Diag.IntStatus,
        TouchDiag->Diag.TransStatus
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.sc : %08x %08x",
        TouchDiag->Diag.ScanAck[0],
        TouchDiag->Diag.ScanAck[1]
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.sc : %08x %08x",
        TouchDiag->Diag.ScanAck[2],
        TouchDiag->Diag.ScanAck[3]
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.ev : p=%u e=%u x=%u",
        TouchDiag->Diag.Polls,
        TouchDiag->Diag.Empty,
        TouchDiag->Diag.Errors
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.ev : s=%u c=%u i=%u",
        TouchDiag->Diag.StatusEvents,
        TouchDiag->Diag.CoordEvents,
        TouchDiag->Diag.Ignored
        );
      B2LUiPrint (
        B2L_UI_GRI,
        L"Touch.raw: %02x %02x %02x %02x %02x %02x %02x %02x",
        TouchDiag->Diag.LastEvent[0],
        TouchDiag->Diag.LastEvent[1],
        TouchDiag->Diag.LastEvent[2],
        TouchDiag->Diag.LastEvent[3],
        TouchDiag->Diag.LastEvent[4],
        TouchDiag->Diag.LastEvent[5],
        TouchDiag->Diag.LastEvent[6],
        TouchDiag->Diag.LastEvent[7]
        );
    } else {
      B2LUiPrint (B2L_UI_KIRMIZI, L"Touch.diag: no protocol/layout");
    }

    if (HasTouch && (Abs->Mode != NULL)) {
      B2LUiPrint (
        B2L_UI_BEYAZ,
        L"Touch.m : %u x %u",
        (UINT32)Abs->Mode->AbsoluteMaxX,
        (UINT32)Abs->Mode->AbsoluteMaxY
        );
    }

    B2LUiPrint (B2L_UI_BEYAZ, L"Key     : %s", LastKey);
    B2LUiPrint (HasTouch ? B2L_UI_YESIL : B2L_UI_GRI, L"Touch   : %s", LastTouch);

    if (HasTouch) {
      B2LUiPrint (
        EFI_ERROR (TouchStatus) ? B2L_UI_SARI : B2L_UI_YESIL,
        L"Touch.dx: st=%r poll=%u hit=%u",
        TouchStatus,
        (UINT32)TouchPolls,
        (UINT32)TouchHits
        );
    }

    if (Exit) {
      break;
    }

    gBS->Stall (20000);
  }

  B2LUiClearFrom (DynTop);
  mUiY = DynTop;
  B2LUiPrint (B2L_UI_BEYAZ, L"Exiting; device will power off.");
  gBS->Stall (2000000);
}

//
// DeviceBootManagerLib implements. The rest is the same as the Silicon Null
// implementation; only UnableToBoot does real work on this device.
//

EFI_HANDLE
EFIAPI
DeviceBootManagerBeforeConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath,
  OUT BDS_CONSOLE_CONNECT_ENTRY **PlatformConsoles
  )
{
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerAfterConsole (
  VOID
  )
{
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerOnDemandConInConnect (
  VOID
  )
{
  return NULL;
}

VOID
EFIAPI
DeviceBootManagerProcessBootCompletion (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return;
}

EFI_STATUS
EFIAPI
DeviceBootManagerPriorityBoot (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return EFI_SUCCESS;
}

VOID
EFIAPI
DeviceBootManagerUnableToBoot (
  VOID
  )
{
  B2LSelfTest ();
}

VOID
EFIAPI
DeviceBootManagerBdsEntry (
  VOID
  )
{
  return;
}