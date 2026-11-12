/** @file
  HSI2C10 transport for beyond0lte (sec_ts).

  B2LTouchDxe does not need to own the transfer machinery: SamsungPkg's
  EFI_HSI2C_PROTOCOL already configures the controller (GPIO mux, USI mode,
  timings) and drives the data phase.  Its ReadBuffer/WriteBuffer primitives
  map exactly onto the sec_ts command protocol:

      ReadBuffer (bus, slave, reg, data, len)  ->  write 1 command byte,
                                                  repeated-start read len
      WriteBuffer(bus, slave, reg, data, len)  ->  write 1 command byte + len

  The previous private implementation re-drove the controller registers by
  hand.  It was byte-for-byte the same sequence as HsI2cXferMsg, but it
  called SW_RST before *every* transfer and, more importantly, could not tell
  a slave NACK from a timeout (it folded both into EFI_DEVICE_ERROR).  This
  driver now defers to the tested path and keeps the raw register snapshot
  only for diagnostics.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/EFIHsI2c.h>

#include "B2LHsI2c.h"

// SamsungPkg's protocol header predates the usual GUID extern declaration.
extern EFI_GUID  gEfiHsI2cProtocolGuid;

#define B2L_HSI2C_BUS            10
#define B2L_HSI2C_SLAVE          0x48
#define B2L_HSI2C_BASE           0x104B0000UL
#define B2L_USI_CONFIG_BASE      0x10421000UL
#define B2L_USI_CONFIG_SIZE      0x00001000UL

// HsI2cInitBus() -> UsiSetMode() does a plain MmioWrite32 to the USI
// configuration register at 0x1042101C.  That page is MMIO, not DRAM, and
// nothing else maps it, so the driver maps it before InitBus.
#define B2L_HSI2C_INT_STATUS     0x24
#define B2L_HSI2C_TRANS_STATUS   0x50

#define B2L_HSI2C_SCAN_FIRST     0x08
#define B2L_HSI2C_SCAN_LAST      0x78

STATIC EFI_HSI2C_PROTOCOL  *mHsI2c;

EFI_STATUS
B2LHsI2cInitialize (
  VOID
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (
                  &gEfiHsI2cProtocolGuid,
                  NULL,
                  (VOID **)&mHsI2c
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Bu sayfa zaten bir baska surucu tarafindan acilmissa AddMemorySpace
  // EFI_ACCESS_DENIED doner; bu durumda sayfa kullanilabilir demektir ve
  // hatayi yutmamiz gerekir.
  //
  Status = MapMemoryRegion (
             B2L_USI_CONFIG_BASE,
             B2L_USI_CONFIG_SIZE,
             EfiMemoryMappedIO
             );
  if (EFI_ERROR (Status) &&
      (Status != EFI_ACCESS_DENIED) &&
      (Status != EFI_ALREADY_STARTED))
  {
    DEBUG ((
      DEBUG_ERROR,
      "B2LTouch: failed to map HSI2C10 USI MMIO page: %r\n",
      Status
      ));
    return Status;
  }

  return mHsI2c->InitBus (B2L_HSI2C_BUS);
}

EFI_STATUS
B2LHsI2cWriteRead (
  IN  CONST UINT8  *WriteBuffer OPTIONAL,
  IN  UINTN         WriteLength,
  OUT UINT8        *ReadBuffer OPTIONAL,
  IN  UINTN         ReadLength
  )
{
  UINT8  *Payload;

  if (mHsI2c == NULL) {
    return EFI_NOT_READY;
  }

  if ((WriteLength == 0) || (WriteBuffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // sec_ts komut protokolu tek kayit baytiyla calisir: WriteBuffer[0] her
  // zaman komuttur; geri kalan baytlar (varsa) onun yukudur.  Okuma ise
  // komut + repeated-start seklindedir; bu yuzden WriteLength == 1 olmalidir.
  //
  if (ReadLength != 0) {
    if ((WriteLength != 1) || (ReadBuffer == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

    return mHsI2c->ReadBuffer (
                     B2L_HSI2C_BUS,
                     B2L_HSI2C_SLAVE,
                     WriteBuffer[0],
                     ReadBuffer,
                     ReadLength
                     );
  }

  Payload = (UINT8 *)(UINTN)WriteBuffer + 1;

  return mHsI2c->WriteBuffer (
                   B2L_HSI2C_BUS,
                   B2L_HSI2C_SLAVE,
                   WriteBuffer[0],
                   Payload,
                   WriteLength - 1
                   );
}

VOID
B2LHsI2cSnapshot (
  OUT UINT32  *IntStatus,
  OUT UINT32  *TransStatus
  )
{
  if ((IntStatus == NULL) || (TransStatus == NULL)) {
    return;
  }

  *IntStatus   = MmioRead32 ((UINTN)(B2L_HSI2C_BASE + B2L_HSI2C_INT_STATUS));
  *TransStatus = MmioRead32 ((UINTN)(B2L_HSI2C_BASE + B2L_HSI2C_TRANS_STATUS));
}

EFI_STATUS
B2LHsI2cScan (
  OUT UINT32  ScanAck[4]
  )
{
  UINT8   Dummy;
  UINT8   Address;

  if (ScanAck == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mHsI2c == NULL) {
    return EFI_NOT_READY;
  }

  ZeroMem (ScanAck, 4 * sizeof (UINT32));

  //
  // HSI2C10 uzerinde yalnizca sec_ts var (bkz. dts fragment@31).  Her
  // adrese tek baytlik no-op komut gondererek ACK kim veriyor gorelim:
  // 0x48 yanit vermiyorsa denetleyici ayaga kalkmamis ya da panel guc
  // vermiyordur; baska bir adres yanit veriyorsa da tarama yolu bozuk
  // demektir.
  //
  for (Address = B2L_HSI2C_SCAN_FIRST; Address < B2L_HSI2C_SCAN_LAST; Address++) {
    if (!EFI_ERROR (
          mHsI2c->WriteBuffer (
                    B2L_HSI2C_BUS,
                    Address,
                    0x00,
                    &Dummy,
                    0
                    )
          ))
    {
      ScanAck[Address >> 5] |= (UINT32)(1U << (Address & 31));
    }
  }

  return EFI_SUCCESS;
}
