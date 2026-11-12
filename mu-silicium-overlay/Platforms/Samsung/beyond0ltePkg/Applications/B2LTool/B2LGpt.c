/** @file
  B2LGpt.c - ham LUN'lardan GPT okuma ve bolum arama.

  SALT OKUMA. Hicbir blok yazilmaz. Her ham (LogicalPartition=FALSE)
  BlockIo cihazi denenir: LBA 1'de "EFI PART" imzasi varsa girdiler
  ayristirilir. S10+ olcumunde GPT bolumleri LUN 0 ve LUN 3'te cikmisti
  (bkz. STATUS.md "LUN 2 sorusu"), bu yuzden tek bir LUN varsayilmaz:
  butun ham LUN'lar taranir.
**/

#include "B2LTool.h"

#pragma pack(1)
typedef struct {
  UINT64    Signature;
  UINT32    Revision;
  UINT32    HeaderSize;
  UINT32    HeaderCrc32;
  UINT32    Reserved;
  UINT64    MyLba;
  UINT64    AlternateLba;
  UINT64    FirstUsableLba;
  UINT64    LastUsableLba;
  EFI_GUID  DiskGuid;
  UINT64    PartitionEntryLba;
  UINT32    NumberOfPartitionEntries;
  UINT32    SizeOfPartitionEntry;
  UINT32    PartitionEntryArrayCRC32;
} B2L_GPT_HEADER;

typedef struct {
  EFI_GUID  PartitionTypeGuid;
  EFI_GUID  UniquePartitionGuid;
  UINT64    StartingLba;
  UINT64    EndingLba;
  UINT64    Attributes;
  CHAR16    PartitionName[36];
} B2L_GPT_ENTRY;
#pragma pack()

STATIC CONST UINT8  mGptSig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

STATIC
BOOLEAN
B2LGuidIsZero (
  IN CONST EFI_GUID  *G
  )
{
  STATIC CONST EFI_GUID  Zero = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

  return (BOOLEAN)(CompareMem (G, &Zero, sizeof (EFI_GUID)) == 0);
}

/**
  GPT adi UTF-16, 36 karakterlik sabit alan; sonda dolgu bosluklari olur.
  Kopyalarken kirpip sonlandiricisi ekler.
**/
STATIC
VOID
B2LCopyName (
  OUT CHAR16         *Dst,
  IN  CONST CHAR16   *Src,
  IN  UINTN           Chars
  )
{
  UINTN  I;

  for (I = 0; I < Chars; I++) {
    Dst[I] = Src[I];
  }

  Dst[Chars] = L'\0';

  while ((Chars > 0) && (Dst[Chars - 1] == L' ')) {
    Dst[Chars - 1] = L'\0';
    Chars--;
  }
}

BOOLEAN
B2LNameEq (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  )
{
  while ((*A != L'\0') && (*B != L'\0')) {
    CHAR16  Ca;
    CHAR16  Cb;

    Ca = *A;
    Cb = *B;
    if ((Ca >= L'a') && (Ca <= L'z')) {
      Ca = (CHAR16)(Ca - 0x20);
    }
    if ((Cb >= L'a') && (Cb <= L'z')) {
      Cb = (CHAR16)(Cb - 0x20);
    }
    if (Ca != Cb) {
      return FALSE;
    }
    A++;
    B++;
  }

  return (BOOLEAN)((*A == L'\0') && (*B == L'\0'));
}

STATIC
VOID
B2LReadGptFromLun (
  IN  EFI_BLOCK_IO_PROTOCOL  *Bio,
  OUT B2L_PART               *Table,
  IN  UINT32                  Max,
  IN OUT UINT32              *Count
  )
{
  EFI_STATUS       Status;
  UINT32           BlockSize;
  B2L_GPT_HEADER   *Hdr;
  VOID             *Entries;
  UINTN            EntBytes;
  UINTN            EntBlocks;
  UINT32           NumEnt;
  UINT32           EntSize;
  UINT32           I;

  if ((Bio == NULL) || (Bio->Media == NULL)) {
    return;
  }

  BlockSize = Bio->Media->BlockSize;
  if ((BlockSize == 0) || ((UINTN)BlockSize < sizeof (B2L_GPT_HEADER))) {
    return;
  }

  Hdr = AllocatePool (BlockSize);
  if (Hdr == NULL) {
    return;
  }

  Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 1, BlockSize, Hdr);
  if (EFI_ERROR (Status) || (CompareMem (Hdr, mGptSig, 8) != 0)) {
    FreePool (Hdr);
    return;
  }

  NumEnt  = Hdr->NumberOfPartitionEntries;
  EntSize = Hdr->SizeOfPartitionEntry;

  //
  // Anlamli sinirlar. Kirli/rastgele veri tasma yaratmasin.
  //
  if ((NumEnt == 0) || (NumEnt > 128) || (EntSize < 128) || (EntSize > 4096)) {
    FreePool (Hdr);
    return;
  }

  EntBytes  = (UINTN)NumEnt * EntSize;
  EntBlocks = (EntBytes + BlockSize - 1) / BlockSize;

  Entries = AllocatePool (EntBlocks * BlockSize);
  if (Entries == NULL) {
    FreePool (Hdr);
    return;
  }

  Status = Bio->ReadBlocks (
                   Bio,
                   Bio->Media->MediaId,
                   Hdr->PartitionEntryLba,
                   EntBlocks * BlockSize,
                   Entries
                   );
  if (EFI_ERROR (Status)) {
    FreePool (Entries);
    FreePool (Hdr);
    return;
  }

  for (I = 0; (I < NumEnt) && (*Count < Max); I++) {
    B2L_GPT_ENTRY  *E;
    B2L_PART       *P;

    E = (B2L_GPT_ENTRY *)((UINT8 *)Entries + (UINTN)I * EntSize);
    if (B2LGuidIsZero (&E->PartitionTypeGuid)) {
      continue;
    }
    if (E->EndingLba < E->StartingLba) {
      continue;
    }

    P            = &Table[*Count];
    P->Index     = I + 1;
    B2LCopyName (P->Name, E->PartitionName, 36);
    P->StartLba  = E->StartingLba;
    P->NumBlocks = E->EndingLba - E->StartingLba + 1;
    P->Lun       = Bio;
    (*Count)++;
  }

  FreePool (Entries);
  FreePool (Hdr);
}

EFI_STATUS
B2LGptScan (
  OUT B2L_PART  *Table,
  IN  UINT32     Max,
  OUT UINT32    *Count
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
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (I = 0; (I < HandleCount) && (*Count < Max); I++) {
    EFI_BLOCK_IO_PROTOCOL  *Bio;

    Bio = NULL;
    if (EFI_ERROR (
          gBS->HandleProtocol (
                 Handles[I],
                 &gEfiBlockIoProtocolGuid,
                 (VOID **)&Bio
                 )
          ))
    {
      continue;
    }

    //
    // Yalnizca ham LUN: PartitionDxe cocuklari tekrar taranmasin.
    //
    if ((Bio == NULL) || (Bio->Media == NULL) ||
        !Bio->Media->MediaPresent || Bio->Media->LogicalPartition)
    {
      continue;
    }

    B2LReadGptFromLun (Bio, Table, Max, Count);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

INTN
B2LFindPart (
  IN B2L_PART     *Table,
  IN UINT32        Count,
  IN CONST CHAR16 *Name
  )
{
  UINT32  I;

  for (I = 0; I < Count; I++) {
    if (B2LNameEq (Table[I].Name, Name)) {
      return (INTN)I;
    }
  }

  return -1;
}

VOID
B2LPrintParts (
  IN B2L_PART  *Table,
  IN UINT32     Count
  )
{
  UINT32  I;

  Print (L"  #   name               LBA        blocks    MB\r\n");
  for (I = 0; I < Count; I++) {
    Print (
      L" %3d  %-18s %10d %10d %8d\r\n",
      (UINT32)Table[I].Index,
      Table[I].Name,
      (UINT32)Table[I].StartLba,
      (UINT32)Table[I].NumBlocks,
      (UINT32)((Table[I].NumBlocks * Table[I].Lun->Media->BlockSize) / (1024 * 1024))
      );
  }
}
