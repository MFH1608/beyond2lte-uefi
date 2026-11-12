/** @file
  B2LTool.c - beyond0lte UEFI arac seti (giris noktasi + menu + komutlar).

  Komutlar (Shell'den):
    B2LTool.efi part
    B2LTool.efi recov          -> PMU INFORM3'e reboot-reason yaz + reset (TWRP)
    B2LTool.efi clear
    B2LTool.efi pmu            -> PMU reset-reason registerlarini oku
    B2LTool.efi flash <dosya> <bolum>
    B2LTool.efi               -> ekranda menu

  BDS boot option ile de calisir: OptionalData ile ayni komut gecirilir;
  OptionalData yoksa menu acilir (bkz. B2LPlatformBootManagerLib).
**/

#include "B2LTool.h"

#define B2L_LINE_CHARS  512
#define B2L_TOK_CHARS   160
#define B2L_MAX_TOK     4

//
// S-Boot reboot-reason arayuzu.
//
// sboot.bin (G970FXXUHHXJ1) tersine muhendisliginde: boot sirasinda
// 0x1586080C (PMU INFORM3) okunur ve 32-bit deger boot moduna cevrilir:
//   0x12345670 | n -> setenv REBOOT_MODE = n
//   0xABC30000     -> "setting rtn_config" / "set recovery mode for boot."
//                     yani RECOVERY  (TWRP buradan acilir)
//   0xABC80000     -> set DUMPSINK
//   0xABCD0000     -> set DEBUG_LEVEL
//   0xABCE0000     -> set SWITCH_SEL
// Deger okunur okunmaz register 0'a cekilir (tek-seferlik mailbox), bu
// yuzden reset'ten ONCE yazilmalidir.
//
#define B2L_PMU_BASE        0x15860000ULL
#define B2L_PMU_RSTSTAT     0x404    // reset durum (warm/watchdog vb.)
#define B2L_PMU_INFORM0     0x800
#define B2L_PMU_INFORM1     0x804
#define B2L_PMU_INFORM2     0x808
#define B2L_PMU_INFORM3     0x80C    // reboot reason (S-Boot bunu okur)
#define B2L_PMU_INFORM4     0x840
#define B2L_PMU_INFORM5     0x844
#define B2L_PMU_INFORM7     0x84C

#define B2L_REBOOT_RECOVERY 0xABC30000U

STATIC
VOID
B2LLine (
  IN CONST CHAR16  *Text
  )
{
  Print (L"%s\r\n", Text);
}

STATIC
VOID
B2LKey (
  IN CHAR16  Ch
  )
{
  Print (L"%c", Ch);
}

VOID
B2LWaitKey (
  OUT EFI_INPUT_KEY  *Key
  )
{
  if ((gST->ConIn == NULL) || (gST->ConIn->ReadKeyStroke == NULL)) {
    Key->ScanCode    = 0;
    Key->UnicodeChar = 0;
    return;
  }

  for (;;) {
    //
    // USB seri konsolu olaylari TPL pompalarindan isler; B2LPlatformBootManager
    // ile ayni numara (bkz. B2LSelfTest).
    //
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));
    if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, Key))) {
      return;
    }
    gBS->Stall (10000);
  }
}

/**
  ConIn "lazy": ILK ReadKeyStroke cagrisi ConnectConIn olayini tetikler ve
  konsol yeniden cizilirken framebuffer'i siler. Cizimden ONCE bir kez
  pompalayip bu yeniden cizimi bitiriyoruz; yoksa menunun ilk karesi
  silinebilir (bkz. B2LPlatformBootManager B2LSelfTest ayni numara).
**/
STATIC
VOID
B2LPrimeConIn (
  VOID
  )
{
  EFI_INPUT_KEY  Key;

  if ((gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
    gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));
  }
}

STATIC
VOID
B2LClearScreen (
  VOID
  )
{
  if ((gST->ConOut != NULL) && (gST->ConOut->ClearScreen != NULL)) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }
}

/**
  MISC bolumune BCB yazar (ya da temizler).

  bootloader_message yerlesimi (AOSP):
    0   command[32]   "boot-recovery"
    32  status[32]
    64  recovery[768] "recovery\n"
  Ilk 832 bayt sifirlanir; sonra alanlar doldurulur. Bolumun geri kalani
  korunur (oku-degistir-yaz).
**/
STATIC
EFI_STATUS
B2LWriteBcb (
  IN B2L_PART  *Part,
  IN BOOLEAN    Clear
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize;
  UINT8       *Buf;

  BlockSize = Part->Lun->Media->BlockSize;
  if (BlockSize < 2048) {
    return EFI_UNSUPPORTED;
  }

  Buf = AllocatePool (BlockSize);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Part->Lun->ReadBlocks (
                         Part->Lun,
                         Part->Lun->Media->MediaId,
                         Part->StartLba,
                         BlockSize,
                         Buf
                         );
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }

  ZeroMem (Buf, 32 + 32 + 768);

  if (!Clear) {
    CopyMem (Buf, "boot-recovery", 13);
    CopyMem (Buf + 64, "recovery\n", 9);
  }

  Status = Part->Lun->WriteBlocks (
                         Part->Lun,
                         Part->Lun->Media->MediaId,
                         Part->StartLba,
                         BlockSize,
                         Buf
                         );
  Part->Lun->FlushBlocks (Part->Lun);
  FreePool (Buf);
  return Status;
}

STATIC
EFI_STATUS
B2LDoPart (
  VOID
  )
{
  EFI_STATUS  Status;
  B2L_PART    Table[B2L_MAX_PARTS];
  UINT32      Count;
  UINT32      I;

  Status = B2LGptScan (Table, B2L_MAX_PARTS, &Count);
  if (EFI_ERROR (Status)) {
    Print (L"GPT scan failed: %r\r\n", Status);
    return Status;
  }

  Print (L"-- GPT partitions (%d) --\r\n", Count);
  B2LPrintParts (Table, Count);

  for (I = 0; I < Count; I++) {
    if (B2LNameEq (Table[I].Name, L"misc") ||
        B2LNameEq (Table[I].Name, L"boot") ||
        B2LNameEq (Table[I].Name, L"recovery"))
    {
      Print (
        L"  -> '%s' LBA=%d blocks=%d\r\n",
        Table[I].Name,
        (UINT32)Table[I].StartLba,
        (UINT32)Table[I].NumBlocks
        );
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
B2LDoRecov (
  VOID
  )
{
  UINT32  Old;
  UINT32  Rd;
  UINT32  Rst;

  //
  // S-Boot, RECOVERY'yi MISC BCB ile degil, PMU INFORM3 (0x1586080C)
  // icindeki reboot-reason degeri ile secer. Bir kez okununca register
  // temizlendigi icin reset'ten ONCE yaziyoruz (bkz. dosya basi).
  //
  Rst = MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_RSTSTAT));
  Old = MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3));

  Print (
    L"PMU  : base=0x%lx RSTSTAT=0x%08x INFORM3(old)=0x%08x\r\n",
    (UINT64)B2L_PMU_BASE,
    Rst,
    Old
    );

  MmioWrite32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3), B2L_REBOOT_RECOVERY);
  Rd = MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3));

  Print (
    L"PMU  : INFORM3 <- 0x%08x (readback 0x%08x)\r\n",
    B2L_REBOOT_RECOVERY,
    Rd
    );

  if (Rd != B2L_REBOOT_RECOVERY) {
    //
    // Okuma tutmadiysa yine de resetliyoruz: yazma gercekten olduysa
    // S-Boot TWRP'yi acar, olmadiysa normal boot'a doner (zararsiz).
    //
    Print (L"WARN : INFORM3 readback mismatch; resetting anyway.\r\n");
  }

  Print (L"PMU reboot-reason = recovery. Resetting to boot TWRP...\r\n");
  gBS->Stall (1000000);

  //
  // EfiResetWarm de EfiResetCold de bu platformda SWRESET'e gider
  // (bkz. ResetSystemLib); PMU INFORM3 SWRESET boyunca korunur.
  //
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  //
  // ResetSystem donmez; donerse bir sorun var.
  //
  Print (L"ERROR: ResetSystem returned.\r\n");
  return EFI_DEVICE_ERROR;
}

/**
  PMU reset-reason registerlarini SALT OKU ve bas. recov'un yazdigi
  INFORM3 degerini dogrulamak/teshis etmek icin (sboot ile ayni yerlesim).
**/
STATIC
EFI_STATUS
B2LDoPmu (
  VOID
  )
{
  Print (L"-- PMU reset-reason (base=0x%lx) --\r\n", (UINT64)B2L_PMU_BASE);
  Print (L"  RSTSTAT (0x%03x) = 0x%08x\r\n", B2L_PMU_RSTSTAT, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_RSTSTAT)));
  Print (L"  INFORM0 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM0, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM0)));
  Print (L"  INFORM1 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM1, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM1)));
  Print (L"  INFORM2 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM2, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM2)));
  Print (L"  INFORM3 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM3, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3)));
  Print (L"  INFORM4 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM4, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM4)));
  Print (L"  INFORM5 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM5, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM5)));
  Print (L"  INFORM7 (0x%03x) = 0x%08x\r\n", B2L_PMU_INFORM7, MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM7)));
  Print (L"  (recovery value = 0x%08x)\r\n", B2L_REBOOT_RECOVERY);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
B2LDoClear (
  VOID
  )
{
  EFI_STATUS  Status;
  B2L_PART    Table[B2L_MAX_PARTS];
  UINT32      Count;
  INTN        Idx;

  Status = B2LGptScan (Table, B2L_MAX_PARTS, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Idx = B2LFindPart (Table, Count, L"misc");
  if (Idx < 0) {
    Print (L"ERROR: 'misc' not found.\r\n");
    return EFI_NOT_FOUND;
  }

  Status = B2LWriteBcb (&Table[Idx], TRUE);
  Print (L"BCB cleared (%r).\r\n", Status);

  //
  // Bekleyen bir recovery reboot-reason'i varsa iptal et (S-Boot
  // normal boot'a doner).
  //
  MmioWrite32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3), 0);
  Print (
    L"PMU INFORM3 cleared (readback 0x%08x).\r\n",
    MmioRead32 ((UINTN)(B2L_PMU_BASE + B2L_PMU_INFORM3))
    );

  return Status;
}

STATIC
EFI_STATUS
B2LFlashPathToPart (
  IN CONST CHAR16  *Path,
  IN CONST CHAR16  *Target
  )
{
  EFI_STATUS  Status;
  B2L_PART    Table[B2L_MAX_PARTS];
  UINT32      Count;
  INTN        Idx;
  VOID        *Raw;
  UINTN       RawSize;
  VOID        *Img;
  UINTN       ImgSize;

  Raw = NULL;
  Img = NULL;

  Status = B2LGptScan (Table, B2L_MAX_PARTS, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Idx = B2LFindPart (Table, Count, Target);
  if (Idx < 0) {
    Print (L"ERROR: partition '%s' not found.\r\n", Target);
    return EFI_NOT_FOUND;
  }

  Print (L"Source : %s\r\n", Path);
  Print (
    L"Target : %s (LBA=%d blocks=%d)\r\n",
    Table[Idx].Name,
    (UINT32)Table[Idx].StartLba,
    (UINT32)Table[Idx].NumBlocks
    );

  Status = B2LReadWholeFile (Path, &Raw, &RawSize);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: cannot read file (%r).\r\n", Status);
    return Status;
  }

  Print (L"File   : %d bytes read\r\n", (UINT32)RawSize);

  //
  // .tar ise icinden .img cikar. Uzantiya bakmadan da denenir cunku
  // Shell bazen buyuk harfli ad verebilir.
  //
  Img     = Raw;
  ImgSize = RawSize;
  if (!EFI_ERROR (B2LTarFindImage (Raw, RawSize, &Img, &ImgSize))) {
    Print (L"TAR    : inner image %d bytes\r\n", (UINT32)ImgSize);
  }

  Status = B2LFlashData (&Table[Idx], Img, ImgSize);
  if (EFI_ERROR (Status)) {
    Print (L"ERROR: write/verify failed (%r).\r\n", Status);
  } else {
    Print (L"OK: %s -> %s written and verified.\r\n", Path, Target);
  }

  FreePool (Raw);
  return Status;
}

/**
  Menu yardimcisi: 0..Count-1 arasi bir secim. Cursor VolUp/VolDown (yukari/
  asagi), onay Bixby (Enter), cikis Esc.
**/
STATIC
INTN
B2LSelect (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Items,
  IN UINT32         Count
  )
{
  UINT32         Sel;
  EFI_INPUT_KEY  Key;
  UINT32         I;

  Sel = 0;
  for (;;) {
    B2LClearScreen ();
    B2LLine (Title);
    B2LLine (L"VolUp/Down = select, Bixby = confirm, hold Bixby = cancel");
    B2LLine (L"----------------------------------------------");
    for (I = 0; I < Count; I++) {
      CONST CHAR16  *P;
      UINT32        Line;

      Line = 0;
      P    = Items;
      while ((Line < I) && (*P != L'\0')) {
        P++;
        if (*P == L'\n') {
          Line++;
        }
      }
      if (*P == L'\n') {
        P++;
      }

      Print (L" %c ", (I == Sel) ? L'>' : L' ');
      while ((*P != L'\n') && (*P != L'\0')) {
        B2LKey (*P);
        P++;
      }
      Print (L"\r\n");
    }
    B2LLine (L"----------------------------------------------");

    B2LWaitKey (&Key);
    if ((Key.ScanCode == SCAN_UP) || (Key.UnicodeChar == L'w')) {
      if (Sel > 0) {
        Sel--;
      }
    } else if ((Key.ScanCode == SCAN_DOWN) || (Key.UnicodeChar == L's')) {
      if (Sel + 1 < Count) {
        Sel++;
      }
    } else if ((Key.ScanCode == SCAN_ESC)) {
      return -1;
    } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
               (Key.UnicodeChar == L'\r') || (Key.UnicodeChar == L'\n'))
    {
      return (INTN)Sel;
    }
  }
}

STATIC
VOID
B2LDoFlashInteractive (
  VOID
  )
{
  B2L_IMAGE  Images[B2L_MAX_IMAGES];
  CHAR16     Items[B2L_MAX_IMAGES * B2L_IMG_NAME];
  UINT32     Count;
  UINT32     I;
  UINTN      Off;
  INTN       Choice;
  B2L_PART   Table[B2L_MAX_PARTS];
  UINT32     PCount;
  CHAR16     Parts[B2L_MAX_PARTS * 48];
  INTN       PChoice;
  EFI_INPUT_KEY  Key;

  Key.ScanCode    = 0;
  Key.UnicodeChar = 0;

  if (EFI_ERROR (B2LListImages (Images, B2L_MAX_IMAGES, &Count)) || (Count == 0)) {
    Print (L"No image found (.img / .tar in root).\r\n");
    Print (L"From Shell: B2LTool.efi flash <file> <partition>\r\n");
    B2LWaitKey (&Key);
    return;
  }

  Off = 0;
  for (I = 0; I < Count; I++) {
    UnicodeSPrint (
      Items + Off,
      sizeof (Items) - Off * sizeof (CHAR16),
      L"[%d] %s\n",
      I + 1,
      Images[I].Name
      );
    Off += StrLen (Items + Off);
  }
  Items[Off] = L'\0';

  Choice = B2LSelect (L"Select image to flash:", Items, Count);
  if (Choice < 0) {
    return;
  }

  if (EFI_ERROR (B2LGptScan (Table, B2L_MAX_PARTS, &PCount))) {
    return;
  }

  Off = 0;
  for (I = 0; I < PCount; I++) {
    UnicodeSPrint (
      Parts + Off,
      sizeof (Parts) - Off * sizeof (CHAR16),
      L"[%d] %s  (%d MB)\n",
      I + 1,
      Table[I].Name,
      (UINT32)((Table[I].NumBlocks * Table[I].Lun->Media->BlockSize) / (1024 * 1024))
      );
    Off += StrLen (Parts + Off);
  }
  Parts[Off] = L'\0';

  PChoice = B2LSelect (L"Select target partition:", Parts, PCount);
  if (PChoice < 0) {
    return;
  }

  {
    VOID   *Raw;
    UINTN  RawSize;
    VOID   *Img;
    UINTN  ImgSize;
    EFI_STATUS  St;

    if (EFI_ERROR (B2LOpenImage (&Images[Choice], &Raw, &RawSize))) {
      Print (L"ERROR: cannot read image.\r\n");
      B2LWaitKey (&Key);
      return;
    }

    Img     = Raw;
    ImgSize = RawSize;
    B2LTarFindImage (Raw, RawSize, &Img, &ImgSize);

    B2LClearScreen ();
    Print (
      L"%s (%d bytes) -> writing to %s...\r\n",
      Images[Choice].Name,
      (UINT32)ImgSize,
      Table[PChoice].Name
      );
    St = B2LFlashData (&Table[PChoice], Img, ImgSize);
    Print (St == EFI_SUCCESS ? L"OK.\r\n" : L"FAILED (%r).\r\n", St);
    FreePool (Raw);
    B2LLine (L"Press any key to continue.");
    B2LWaitKey (&Key);
  }
}

STATIC
VOID
B2LMenu (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  UINT32         Sel;
  CONST CHAR16   *Items;
  UINT32         Count;

  Items =
    L"[1] Dump GPT (read-only)\n"
    L"[2] Reboot to recovery (TWRP)\n"
    L"[3] Clear BCB + reboot reason\n"
    L"[4] Flash image (ESP to partition)\n"
    L"[5] Dump PMU reboot reason\n"
    L"[6] Reboot\n"
    L"[7] Power off\n"
    L"[0] Exit (self-test screen)\n";
  Count = 8;
  Sel   = 0;

  B2LPrimeConIn ();

  for (;;) {
    B2LClearScreen ();
    B2LLine (L"==============================================");
    B2LLine (L"  S10e (beyond0lte) - UEFI TOOL");
    B2LLine (L"==============================================");

    {
      UINT32  I;
      UINT32  Line;
      CONST CHAR16  *P;

      for (I = 0; I < Count; I++) {
        Line = 0;
        P    = Items;
        while ((Line < I) && (*P != L'\0')) {
          P++;
          if (*P == L'\n') {
            Line++;
          }
        }
        if (*P == L'\n') {
          P++;
        }

        Print (L"  %c ", (I == Sel) ? L'>' : L' ');
        while ((*P != L'\n') && (*P != L'\0')) {
          B2LKey (*P);
          P++;
        }
        Print (L"\r\n");
      }
    }

    B2LLine (L"----------------------------------------------");
    B2LLine (L"VolUp/Down = select   Bixby = confirm");

    B2LWaitKey (&Key);
    if (Key.ScanCode == SCAN_UP) {
      if (Sel > 0) {
        Sel--;
      }
    } else if (Key.ScanCode == SCAN_DOWN) {
      if (Sel + 1 < Count) {
        Sel++;
      }
    } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.ScanCode == SCAN_ESC)) {
      if (Key.ScanCode == SCAN_ESC) {
        return;
      }

      switch (Sel) {
        case 0:
          B2LClearScreen ();
          B2LDoPart ();
          B2LLine (L"Press any key to continue.");
          B2LWaitKey (&Key);
          break;
        case 1:
          B2LDoRecov ();
          break;
        case 2:
          B2LDoClear ();
          B2LLine (L"Press any key to continue.");
          B2LWaitKey (&Key);
          break;
        case 3:
          B2LDoFlashInteractive ();
          break;
        case 4:
          B2LClearScreen ();
          B2LDoPmu ();
          B2LLine (L"Press any key to continue.");
          B2LWaitKey (&Key);
          break;
        case 5:
          gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
          break;
        case 6:
          gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
          break;
        default:
          return;
      }
    }
  }
}

STATIC
VOID
B2LTokenize (
  IN  CHAR16  *Line,
  OUT CHAR16   Tok[B2L_MAX_TOK][B2L_TOK_CHARS],
  OUT UINTN   *TokCount
  )
{
  UINTN  N;
  CHAR16 *P;

  N = 0;
  P = Line;

  while ((*P != L'\0') && (N < B2L_MAX_TOK)) {
    UINTN  I;

    while ((*P == L' ') || (*P == L'\t')) {
      P++;
    }
    if (*P == L'\0') {
      break;
    }

    I = 0;
    while ((*P != L'\0') && (*P != L' ') && (*P != L'\t') && (I + 1 < B2L_TOK_CHARS)) {
      Tok[N][I] = *P;
      I++;
      P++;
    }
    Tok[N][I] = L'\0';
    N++;
  }

  *TokCount = N;
}

EFI_STATUS
EFIAPI
B2LToolEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_SHELL_PARAMETERS_PROTOCOL    *Shell;
  EFI_LOADED_IMAGE_PROTOCOL        *Loaded;
  CHAR16                           Line[B2L_LINE_CHARS];
  CHAR16                           Tok[B2L_MAX_TOK][B2L_TOK_CHARS];
  UINTN                            TokCount;

  (VOID)SystemTable;

  Line[0] = L'\0';
  Shell   = NULL;
  Loaded  = NULL;

  //
  // Argumanlar: once Shell parametre protokolu (Shell'den cagrildi), yoksa
  // LoadedImage LoadOptions (BDS boot option OptionalData).
  //
  Status = gBS->LocateProtocol (
                  (EFI_GUID *)(VOID *)&gEfiShellParametersProtocolGuid,
                  NULL,
                  (VOID **)&Shell
                  );
  if (!EFI_ERROR (Status) && (Shell != NULL) && (Shell->Argc > 1)) {
    UINTN  I;

    Line[0] = L'\0';
    for (I = 1; I < Shell->Argc; I++) {
      if (I > 1) {
        StrCatS (Line, B2L_LINE_CHARS, L" ");
      }
      StrCatS (Line, B2L_LINE_CHARS, Shell->Argv[I]);
    }
  } else {
    Status = gBS->HandleProtocol (
                    ImageHandle,
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&Loaded
                    );
    if (!EFI_ERROR (Status) && (Loaded != NULL) &&
        (Loaded->LoadOptions != NULL) && (Loaded->LoadOptionsSize > 0))
    {
      UINTN  Chars;
      UINTN  I;

      Chars = Loaded->LoadOptionsSize / sizeof (CHAR16);
      if (Chars >= B2L_LINE_CHARS) {
        Chars = B2L_LINE_CHARS - 1;
      }
      for (I = 0; I < Chars; I++) {
        Line[I] = ((CHAR16 *)Loaded->LoadOptions)[I];
      }
      Line[Chars] = L'\0';
    }
  }

  B2LTokenize (Line, Tok, &TokCount);

  if (TokCount == 0) {
    B2LMenu ();
    return EFI_SUCCESS;
  }

  if (B2LNameEq (Tok[0], L"part")) {
    Status = B2LDoPart ();
    return Status;
  }

  if (B2LNameEq (Tok[0], L"recov") || B2LNameEq (Tok[0], L"recovery")) {
    return B2LDoRecov ();
  }

  if (B2LNameEq (Tok[0], L"clear")) {
    return B2LDoClear ();
  }

  if (B2LNameEq (Tok[0], L"pmu")) {
    return B2LDoPmu ();
  }

  if (B2LNameEq (Tok[0], L"flash")) {
    if (TokCount < 3) {
      Print (L"Usage: B2LTool.efi flash <file> <partition>\r\n");
      return EFI_INVALID_PARAMETER;
    }
    return B2LFlashPathToPart (Tok[1], Tok[2]);
  }

  Print (L"Unknown command: %s\r\n", Tok[0]);
  Print (L"Commands: part | recov | clear | pmu | flash <file> <partition>\r\n");
  return EFI_INVALID_PARAMETER;
}
