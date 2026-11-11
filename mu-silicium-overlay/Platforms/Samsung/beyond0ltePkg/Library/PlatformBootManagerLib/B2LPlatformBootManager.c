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
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>

//
// S10e'de son DRAM banki 0x980000000'da bitiyor (0x80000000..0x100000000 +
// 0x880000000..0x980000000 = 6 GiB). Bu adresin ustunde, var olmayan DRAM'i
// tarif eden BIR descriptor OLMAMALI; bu satir MemoryMapLib'in dogru
// calistigini gosterir. Yuksek adresli MMIO (varsa) zararsiz sayilir.
//
#define B2L_MMIO_LIMIT  0x980000000ULL

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
    Print (L"RAM     : GetMemoryMap probe hatasi (%r)\r\n", Status);
    return;
  }

  MapSize += 8 * DescSize;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    Print (L"RAM     : bellek ayrilamadi\r\n");
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    Print (L"RAM     : GetMemoryMap hatasi (%r)\r\n", Status);
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

  Print (L"RAM     : %u MB DRAM, %u MB free\r\n",
         (UINT32)DivU64x32 (DramTotal, 1024 * 1024),
         (UINT32)DivU64x32 (FreeRam, 1024 * 1024));
  Print (L"Memmap  : %u desc, max end 0x%lx\r\n", (UINT32)Count, Highest);
  Print (L"RAM >=0x980000000  : %a\r\n",
         AboveLimit ? "ADA (SALAH)" : "yok (dogru)");

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
    Print (L"Block IO: yok (%r)\r\n", Status);
  } else {
    Print (L"Block IO: %u device\r\n", (UINT32)HandleCount);
    Shown = 0;
    for (Index = 0; (Index < HandleCount) && (Shown < 6); Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
      if (EFI_ERROR (Status) || (Bio->Media == NULL)) {
        continue;
      }

      Bytes = MultU64x64 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
      Print (L"  #%u %u MB blk=%u %a%a\r\n",
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
    Print (L"FAT/FS  : 0 volume\r\n");
  } else {
    Print (L"FAT/FS  : %u volume\r\n", (UINT32)HandleCount);
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
    Print (L"Display : %ux%u (GOP mode %u)\r\n",
           Gop->Mode->Info->HorizontalResolution,
           Gop->Mode->Info->VerticalResolution,
           Gop->Mode->Mode);
  } else {
    Print (L"Display : GOP yok (%r)\r\n", Status);
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
  BOOLEAN                        HasTouch;
  BOOLEAN                        Exit;
  EFI_STATUS                     TouchStatus;
  UINT32                         TouchPolls;
  UINT32                         TouchHits;
  UINTN                          DynRow;
  CHAR16                         LastKey[56];
  CHAR16                         LastTouch[80];

  HasTouch    = FALSE;
  Exit        = FALSE;
  TouchStatus = EFI_NOT_READY;
  TouchPolls  = 0;
  TouchHits   = 0;
  DynRow      = 0;

  if (gST->ConOut != NULL) {
    gST->ConOut->ClearScreen (gST->ConOut);
    gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLACK);
  }

  UnicodeSPrint (LastKey, sizeof (LastKey), L"(henuz yok)");
  UnicodeSPrint (LastTouch, sizeof (LastTouch), L"(henuz yok)");

  Print (L"==============================================================\r\n");
  Print (L"  S10e (beyond0lte) - UEFI SELF TEST\r\n");
  Print (L"  OS/ESP yok; bu ekran dogrulama icindir. Fotografla.\r\n");
  Print (L"==============================================================\r\n");

  B2LDrawDisplay ();
  B2LDrawMemory ();
  B2LDrawDevices ();

  Status   = gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Abs);
  HasTouch = !EFI_ERROR (Status);
  Print (L"Touch   : %a\r\n", HasTouch ? "AbsolutePointer var" : "yok");
  if (HasTouch && (Abs->Mode != NULL)) {
    Print (L"Touch.m : %u x %u\r\n",
           (UINT32)Abs->Mode->AbsoluteMaxX,
           (UINT32)Abs->Mode->AbsoluteMaxY);
  }

  Print (L"--------------------------------------------------------------\r\n");
  Print (L"VolUp=Up  VolDown=Down  Bixby=Enter  Bixby(tahan)=Esc\r\n");
  Print (L"Sentuh layar -> Touch.dx hit artmali; cikis: Bixby(tahan)+Volume\r\n");

  if ((gST->ConOut != NULL) && (gST->ConOut->Mode != NULL)) {
    DynRow = (UINTN)gST->ConOut->Mode->CursorRow;
  }

  for (;;) {
    //
    // Bu dongu kendi govdesinde gBS->Stall ile bekledigi icin TPL_CALLBACK
    // zamanlayicilari (ornegin touch yoklama) kendiliginden islemez.
    // Raise/RestoreTPL cifti bekleyen tum notify fonksiyonlarini pompalar.
    //
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));

    if (gST->ConOut != NULL) {
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, DynRow);
    }

    Print (L"Key     : %s                                \r\n", LastKey);
    Print (L"Touch   : %s                                \r\n", LastTouch);

    if ((gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        if (Key.ScanCode == SCAN_UP) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Up   -> SCAN_UP (0x01)");
        } else if (Key.ScanCode == SCAN_DOWN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Down -> SCAN_DOWN (0x02)");
        } else if (Key.ScanCode == SCAN_ESC) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby tahan -> SCAN_ESC (0x17)");
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby singkat -> ENTER (0x0D)");
        } else if ((Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
          UnicodeSPrint (
            LastKey,
            sizeof (LastKey),
            L"Bixby+Vol -> %a (cikis)",
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

      Print (L"Touch.dx: st=%r poll=%u hit=%u               \r\n",
             TouchStatus,
             (UINT32)TouchPolls,
             (UINT32)TouchHits);
    }

    if (Exit) {
      break;
    }

    gBS->Stall (20000);
  }

  if (gST->ConOut != NULL) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, DynRow);
  }

  Print (L"Cikiliyor; cihaz kapatilacak.                    \r\n");
  Print (L"                                                  \r\n");
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