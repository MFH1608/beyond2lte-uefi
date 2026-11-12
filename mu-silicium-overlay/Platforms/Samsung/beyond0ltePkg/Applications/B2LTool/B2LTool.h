/** @file
  B2LTool - beyond0lte UEFI arac seti (ortak tanimlar).

  Ne yapar:
    part   : LUN'larin GPT tablosunu OKUR (yazma yok)
    recov  : PMU INFORM3'e reboot-reason yazar, sonra reset (TWRP)
    clear  : MISC BCB'yi ve PMU reboot-reason'i temizler
    pmu    : PMU reset-reason registerlarini OKUR (yazma yok)
    flash  : bir imaji (img / img.tar) bir bolume yazar ve dogrular
    menu   : argumansiz cagrilirsa ekranda menusu acilir

  Gerekce: bu cihazda guc dugmesi yok; TWRP'ye yalnizca Samsung
  "Vol Up + Bixby + POWER" ile girilebiliyordu. S-Boot (sboot.bin)
  RECOVERY secimini MISC BCB ile DEGIL, PMU INFORM3 (base 0x15860000 +
  0x80C = 0x1586080C) icindeki "reboot reason" degeri ile yapar:
  sboot boot sirasinda 0x1586080C'yi okur ve 0xABC3xxxx ->
  "set recovery mode for boot." (RECOVERY), 0xABC80000 -> DUMPSINK,
  0xABCD0000 -> DEBUG_LEVEL, ... diye ayirt eder; okunduktan sonra
  register'i 0'a ceker (tek-seferlik mailbox). Bu yuzden recov, SWRESET
  ONCESI INFORM3'e 0xABC30000 yazar; reset sonrasi S-Boot TWRP'yi acar.
  Boylece hicbir fiziksel tus gerekmez.

  Not: AOSP BCB token'i "boot-recovery" bu S-Boot tarafindan OKUNMAZ
  (sboot.bin icinde boyle bir literal yok); clear yalnizca kalinti
  temizligi icindir.

  Yazma yolu: PMU yazmasi MmioWrite32 ile yapilir (PMU bolgesi zaten
  MMAP_IO olarak haritali; bkz. ResetSystemLib). UFS yazma (flash) icin
  UfsDxe Media.ReadOnly = FALSE ve WriteBlocks mevcut
  (bkz. STATUS.md "UFS yazma CALISIYOR").
**/

#ifndef _B2L_TOOL_H_
#define _B2L_TOOL_H_

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Guid/FileInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ShellParameters.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>

//
// B2LTool'un FV dosya GUID'i. B2LPlatformBootManagerLib ayni GUID ile
// boot option kaydeder; iki taraf AYNI kalmak zorundadir (B2LTool.inf
// icindeki FILE_GUID ile birebir).
//
#define B2L_TOOL_FILE_GUID \
  { 0xC7E1A2B3, 0x4D5F, 0x4061, { 0x8A, 0x92, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F, 0x80 } }

#define B2L_MAX_PARTS   128
#define B2L_NAME_CHARS  37     // GPT ad alani 36 UTF-16 + sonlandirici

#define B2L_MAX_IMAGES  32
#define B2L_IMG_NAME    80

//
// Bir GPT bolumu. StartLba/NumBlocks, Lun'un mantiksal blok birimindedir.
//
typedef struct {
  UINT32                 Index;       // GPT girdi numarasi (1 tabanli)
  CHAR16                 Name[B2L_NAME_CHARS];
  UINT64                 StartLba;
  UINT64                 NumBlocks;
  EFI_BLOCK_IO_PROTOCOL  *Lun;        // bu bolumun ait oldugu ham LUN
} B2L_PART;

//
// Bir imaj dosyasi adayi (menudeki "imaj sec" icin).
//
typedef struct {
  EFI_HANDLE  Volume;               // SimpleFileSystem handle
  CHAR16      Name[B2L_IMG_NAME];   // kok dizindeki dosya adi
} B2L_IMAGE;

//
// B2LGpt.c
//
EFI_STATUS
B2LGptScan (
  OUT B2L_PART  *Table,
  IN  UINT32     Max,
  OUT UINT32    *Count
  );

INTN
B2LFindPart (
  IN B2L_PART     *Table,
  IN UINT32        Count,
  IN CONST CHAR16 *Name
  );

BOOLEAN
B2LNameEq (
  IN CONST CHAR16  *A,
  IN CONST CHAR16  *B
  );

VOID
B2LPrintParts (
  IN B2L_PART  *Table,
  IN UINT32     Count
  );

//
// B2LFlash.c
//
EFI_STATUS
B2LReadWholeFile (
  IN  CONST CHAR16  *Path,
  OUT VOID         **Data,
  OUT UINTN         *Size
  );

EFI_STATUS
B2LTarFindImage (
  IN  VOID   *Tar,
  IN  UINTN   TarSize,
  OUT VOID  **ImgOut,
  OUT UINTN  *ImgSizeOut
  );

EFI_STATUS
B2LFlashData (
  IN B2L_PART  *Part,
  IN VOID      *Data,
  IN UINTN      Size
  );

EFI_STATUS
B2LListImages (
  OUT B2L_IMAGE  *List,
  IN  UINT32      Max,
  OUT UINT32     *Count
  );

EFI_STATUS
B2LOpenImage (
  IN  B2L_IMAGE  *Image,
  OUT VOID      **Data,
  OUT UINTN      *Size
  );

//
// B2LTool.c yardimcilari
//
VOID
B2LWaitKey (
  OUT EFI_INPUT_KEY  *Key
  );

#endif // _B2L_TOOL_H_
