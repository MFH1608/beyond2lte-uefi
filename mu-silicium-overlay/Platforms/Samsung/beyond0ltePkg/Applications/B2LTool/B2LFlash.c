/** @file
  B2LFlash.c - FAT biriminden imaj okuyup bir bolume yazma + dogrulama.

  Iki kaynak bicimi:
    .img     : ham imaj, oldugu gibi yazilir
    .tar     : Odin tar'i; icinden ilk ".img" girdisi (yoksa ilk dosya)
               cikarilip yazilir (bkz. twrp-...img.tar -> recovery.img)

  Yazma EFI_BLOCK_IO ile ham LUN uzerinden yapilir. Bolum adi GPT'den
  cozulur (bkz. B2LGpt.c); LBA 0'in aygit tarafindan reddedilmesi bu
  araci etkilemez cunku hedef bolumler sifirdan buyuk LBA'lardadir.
**/

#include "B2LTool.h"

#define B2L_VERIFY_FULL_LIMIT   (64ULL * 1024 * 1024)   // tam dogrulama siniri
#define B2L_VERIFY_SAMPLE       (8ULL * 1024 * 1024)    // buyukte ilk ornek


STATIC
CONST CHAR16 *
B2LBaseName (
  IN CONST CHAR16  *Path
  )
{
  CONST CHAR16  *P;
  CONST CHAR16  *Last;

  Last = Path;
  for (P = Path; *P != L'\0'; P++) {
    if ((*P == L'\\') || (*P == L'/') || (*P == L':')) {
      Last = P + 1;
    }
  }

  return Last;
}

/**
  Yolu bir kok dizinden acmayi dener. Yol ':' iceriyorsa sonrasina bakilir.
**/
STATIC
EFI_STATUS
B2LOpenOnRoot (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT EFI_FILE_PROTOCOL **File,
  OUT UINT64             *Size
  )
{
  EFI_STATUS     Status;
  CONST CHAR16   *P;
  CHAR16         Buf[256];
  EFI_FILE_INFO  *Info;
  UINTN          InfoSize;

  P = Path;
  while ((*P != L'\0') && (*P != L':')) {
    P++;
  }
  if (*P == L':') {
    P++;
  }

  if (StrLen (P) >= (sizeof (Buf) / sizeof (CHAR16))) {
    return EFI_INVALID_PARAMETER;
  }

  UnicodeSPrint (Buf, sizeof (Buf), L"%s", P);

  Status = Root->Open (Root, File, Buf, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = (*File)->GetInfo (*File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    (*File)->Close (*File);
    return EFI_NOT_FOUND;
  }

  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    (*File)->Close (*File);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = (*File)->GetInfo (*File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    (*File)->Close (*File);
    return Status;
  }

  *Size = Info->FileSize;
  FreePool (Info);
  return EFI_SUCCESS;
}

/**
  Verilen addaki dosyayi butun FAT birimlerinin KOK dizininde arar.
**/
STATIC
EFI_STATUS
B2LOpenAnyVolume (
  IN  CONST CHAR16       *Path,
  OUT EFI_FILE_PROTOCOL **File,
  OUT UINT64             *Size
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       I;
  CONST CHAR16 *Base;

  Handles     = NULL;
  HandleCount = 0;
  Base        = B2LBaseName (Path);

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (I = 0; I < HandleCount; I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
    EFI_FILE_PROTOCOL                *Root;

    Fs = NULL;
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[I],
                 &gEfiSimpleFileSystemProtocolGuid,
                 (VOID **)&Fs
                 )
          ))
    {
      continue;
    }

    Root = NULL;
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    //
    // Once tam yol (varsa "fs0:" sonrasi), sonra yalnizca taban ad.
    //
    if (!EFI_ERROR (B2LOpenOnRoot (Root, Path, File, Size))) {
      Root->Close (Root);
      Status = EFI_SUCCESS;
      break;
    }

    if (!B2LNameEq (Path, Base)) {
      if (!EFI_ERROR (B2LOpenOnRoot (Root, Base, File, Size))) {
        Root->Close (Root);
        Status = EFI_SUCCESS;
        break;
      }
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Status;
}

EFI_STATUS
B2LReadWholeFile (
  IN  CONST CHAR16  *Path,
  OUT VOID         **Data,
  OUT UINTN         *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT64             FileSize;
  UINT8              *Buf;
  UINTN              ReadSize;

  File = NULL;
  Status = B2LOpenAnyVolume (Path, &File, &FileSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (FileSize == 0) {
    File->Close (File);
    return EFI_NOT_FOUND;
  }

  if (FileSize > (512ULL * 1024 * 1024)) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Buf = AllocatePool ((UINTN)FileSize);
  if (Buf == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  File->SetPosition (File, 0);
  ReadSize = (UINTN)FileSize;
  Status   = File->Read (File, &ReadSize, Buf);
  File->Close (File);

  if (EFI_ERROR (Status) || (ReadSize != (UINTN)FileSize)) {
    FreePool (Buf);
    return EFI_DEVICE_ERROR;
  }

  *Data = Buf;
  *Size = (UINTN)FileSize;
  return EFI_SUCCESS;
}

STATIC
UINT64
B2LTarOctal (
  IN CONST UINT8  *Field,
  IN UINTN         Len
  )
{
  UINT64  V;
  UINTN   I;

  V = 0;
  for (I = 0; I < Len; I++) {
    if ((Field[I] == ' ') || (Field[I] == '\0')) {
      continue;
    }
    if ((Field[I] < '0') || (Field[I] > '7')) {
      break;
    }
    V = (V << 3) + (Field[I] - '0');
  }

  return V;
}

STATIC
BOOLEAN
B2LHasSuffix (
  IN CONST CHAR8  *Name,
  IN CONST CHAR8  *Suffix
  )
{
  UINTN  Nl;
  UINTN  Sl;
  UINTN  I;

  Nl = AsciiStrLen (Name);
  Sl = AsciiStrLen (Suffix);
  if (Nl < Sl) {
    return FALSE;
  }

  for (I = 0; I < Sl; I++) {
    CHAR8  A;
    CHAR8  B;

    A = Name[Nl - Sl + I];
    B = Suffix[I];
    if ((A >= 'a') && (A <= 'z')) {
      A = (CHAR8)(A - 0x20);
    }
    if ((B >= 'a') && (B <= 'z')) {
      B = (CHAR8)(B - 0x20);
    }
    if (A != B) {
      return FALSE;
    }
  }

  return TRUE;
}

EFI_STATUS
B2LTarFindImage (
  IN  VOID   *Tar,
  IN  UINTN   TarSize,
  OUT VOID  **ImgOut,
  OUT UINTN  *ImgSizeOut
  )
{
  UINT8  *O;
  UINTN   Off;
  VOID   *FirstData;
  UINTN   FirstSize;

  O         = (UINT8 *)Tar;
  Off       = 0;
  FirstData = NULL;
  FirstSize = 0;

  while ((Off + 512) <= TarSize) {
    UINT8   *H;
    BOOLEAN  AllZero;
    UINTN    I;
    UINT64   Sz;
    CHAR8    Name[101];
    UINT8    Type;
    UINTN    NameLen;

    H       = O + Off;
    AllZero = TRUE;
    for (I = 0; I < 512; I++) {
      if (H[I] != 0) {
        AllZero = FALSE;
        break;
      }
    }
    if (AllZero) {
      break;
    }

    //
    // ustar imzasi yoksa bu bir tar degil.
    //
    if (CompareMem (H + 257, "ustar", 5) != 0) {
      return EFI_NOT_FOUND;
    }

    Sz   = B2LTarOctal (H + 124, 12);
    Type = H[156];

    NameLen = 0;
    while ((NameLen < 100) && (H[NameLen] != 0)) {
      Name[NameLen] = (CHAR8)H[NameLen];
      NameLen++;
    }
    Name[NameLen] = 0;

    Off += 512;

    if (((Type == 0) || (Type == '0')) && (Sz > 0) && ((UINTN)Sz <= (TarSize - Off))) {
      if (FirstData == NULL) {
        FirstData = O + Off;
        FirstSize = (UINTN)Sz;
      }
      if (B2LHasSuffix (Name, ".img")) {
        *ImgOut     = O + Off;
        *ImgSizeOut = (UINTN)Sz;
        return EFI_SUCCESS;
      }
    }

    Off += (UINTN)(((Sz + 511) / 512) * 512);
  }

  if (FirstData != NULL) {
    *ImgOut     = FirstData;
    *ImgSizeOut = FirstSize;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
B2LVerifyData (
  IN B2L_PART  *Part,
  IN VOID      *Data,
  IN UINTN      Size
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize;
  UINTN       VerifyBytes;
  UINTN       Offset;
  UINT8       *Buf;
  UINTN       Chunk;
  UINTN       ReadSize;

  BlockSize  = Part->Lun->Media->BlockSize;
  VerifyBytes = ((UINT64)Size <= B2L_VERIFY_FULL_LIMIT) ? Size : B2L_VERIFY_SAMPLE;
  if (VerifyBytes > Size) {
    VerifyBytes = Size;
  }

  Chunk = 1024 * 1024;
  if (Chunk > VerifyBytes) {
    Chunk = VerifyBytes;
  }
  //
  // Chunk blok katina yuvarlanmali (ReadBlocks sarti).
  //
  Chunk = (Chunk / BlockSize) * BlockSize;
  if (Chunk == 0) {
    Chunk = BlockSize;
  }

  Buf = AllocatePool (Chunk);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EFI_SUCCESS;
  for (Offset = 0; Offset + BlockSize <= VerifyBytes; ) {
    ReadSize = Chunk;
    if (Offset + ReadSize > VerifyBytes) {
      ReadSize = VerifyBytes - Offset;
    }
    ReadSize = (ReadSize / BlockSize) * BlockSize;

    Status = Part->Lun->ReadBlocks (
                           Part->Lun,
                           Part->Lun->Media->MediaId,
                           Part->StartLba + (Offset / BlockSize),
                           ReadSize,
                           Buf
                           );
    if (EFI_ERROR (Status)) {
      break;
    }

    if (CompareMem (Buf, (UINT8 *)Data + Offset, ReadSize) != 0) {
      Status = EFI_CRC_ERROR;
      break;
    }

    Offset += ReadSize;
  }

  FreePool (Buf);
  return Status;
}

EFI_STATUS
B2LFlashData (
  IN B2L_PART  *Part,
  IN VOID      *Data,
  IN UINTN      Size
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize;
  UINT64      Blocks;
  UINTN       Whole;
  UINTN       Tail;

  if ((Part == NULL) || (Data == NULL) || (Size == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  BlockSize = Part->Lun->Media->BlockSize;
  if (BlockSize == 0) {
    return EFI_DEVICE_ERROR;
  }

  Blocks = ((UINT64)Size + BlockSize - 1) / BlockSize;
  if (Blocks > Part->NumBlocks) {
    Print (
      L"  ERROR: image %d MB, partition only %d MB\r\n",
      (UINT32)(Size / (1024 * 1024)),
      (UINT32)((Part->NumBlocks * BlockSize) / (1024 * 1024))
      );
    return EFI_VOLUME_FULL;
  }

  Whole = ((UINTN)Size / BlockSize) * BlockSize;

  if (Whole > 0) {
    Status = Part->Lun->WriteBlocks (
                           Part->Lun,
                           Part->Lun->Media->MediaId,
                           Part->StartLba,
                           Whole,
                           Data
                           );
    if (EFI_ERROR (Status)) {
      Print (L"  ERROR: WriteBlocks whole blocks -> %r\r\n", Status);
      return Status;
    }
  }

  Tail = (UINTN)Size - Whole;
  if (Tail > 0) {
    UINT8  *Last;

    Last = AllocatePool (BlockSize);
    if (Last == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    ZeroMem (Last, BlockSize);
    CopyMem (Last, (UINT8 *)Data + Whole, Tail);

    Status = Part->Lun->WriteBlocks (
                           Part->Lun,
                           Part->Lun->Media->MediaId,
                           Part->StartLba + (Whole / BlockSize),
                           BlockSize,
                           Last
                           );
    FreePool (Last);
    if (EFI_ERROR (Status)) {
      Print (L"  ERROR: WriteBlocks tail block -> %r\r\n", Status);
      return Status;
    }
  }

  Part->Lun->FlushBlocks (Part->Lun);

  Print (L"  Written; reading back to verify...\r\n");
  Status = B2LVerifyData (Part, Data, Size);
  return Status;
}

EFI_STATUS
B2LListImages (
  OUT B2L_IMAGE  *List,
  IN  UINT32      Max,
  OUT UINT32     *Count
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       I;

  Handles     = NULL;
  HandleCount = 0;
  *Count      = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (I = 0; (I < HandleCount) && (*Count < Max); I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
    EFI_FILE_PROTOCOL                *Root;

    Fs = NULL;
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[I],
                 &gEfiSimpleFileSystemProtocolGuid,
                 (VOID **)&Fs
                 )
          ))
    {
      continue;
    }

    Root = NULL;
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    Root->SetPosition (Root, 0);
    for (;;) {
      UINTN          InfoSize;
      EFI_FILE_INFO  *Info;
      EFI_STATUS     RdStatus;

      InfoSize = SIZE_OF_EFI_FILE_INFO + 512 * sizeof (CHAR16);
      Info     = AllocatePool (InfoSize);
      if (Info == NULL) {
        break;
      }

      RdStatus = Root->Read (Root, &InfoSize, Info);
      if (EFI_ERROR (RdStatus) || (InfoSize == 0)) {
        FreePool (Info);
        break;
      }

      if ((Info->Attribute & EFI_FILE_DIRECTORY) == 0) {
        CHAR8  Ansi[B2L_IMG_NAME];

        UnicodeStrToAsciiStrS (Info->FileName, Ansi, sizeof (Ansi));
        if (B2LHasSuffix (Ansi, ".img") || B2LHasSuffix (Ansi, ".tar")) {
          UnicodeSPrint (
            List[*Count].Name,
            sizeof (List[*Count].Name),
            L"%s",
            Info->FileName
            );
          List[*Count].Volume = Handles[I];
          (*Count)++;
        }
      }

      FreePool (Info);
      if (*Count >= Max) {
        break;
      }
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
B2LOpenImage (
  IN  B2L_IMAGE  *Image,
  OUT VOID      **Data,
  OUT UINTN      *Size
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINT64                           FileSize;
  UINT8                            *Buf;
  UINTN                            ReadSize;

  Fs = NULL;
  if (EFI_ERROR (
        gBS->HandleProtocol (
               Image->Volume,
               &gEfiSimpleFileSystemProtocolGuid,
               (VOID **)&Fs
               )
        ))
  {
    return EFI_NOT_FOUND;
  }

  Root = NULL;
  if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
    return EFI_NOT_FOUND;
  }

  Status = B2LOpenOnRoot (Root, Image->Name, &File, &FileSize);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((FileSize == 0) || (FileSize > (512ULL * 1024 * 1024))) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Buf = AllocatePool ((UINTN)FileSize);
  if (Buf == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  File->SetPosition (File, 0);
  ReadSize = (UINTN)FileSize;
  Status   = File->Read (File, &ReadSize, Buf);
  File->Close (File);

  if (EFI_ERROR (Status) || (ReadSize != (UINTN)FileSize)) {
    FreePool (Buf);
    return EFI_DEVICE_ERROR;
  }

  *Data = Buf;
  *Size = (UINTN)FileSize;
  return EFI_SUCCESS;
}
