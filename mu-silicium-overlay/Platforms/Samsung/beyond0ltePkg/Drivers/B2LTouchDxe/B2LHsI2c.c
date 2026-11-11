/** @file
  Minimal combined-transfer transport for beyond0lte HSI2C10.

  SamsungPkg's public HSI2C protocol initializes the controller but its data
  API always emits a four-byte register address.  sec_ts uses a one-byte
  command followed by a repeated-start read, so this platform driver performs
  only that data phase against the already initialized HSI2C10 controller.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/EFIHsI2c.h>

#include "B2LHsI2c.h"

// SamsungPkg's protocol header predates the usual GUID extern declaration.
extern EFI_GUID  gEfiHsI2cProtocolGuid;

#define B2L_HSI2C_BUS           10
#define B2L_HSI2C_SLAVE         0x48
#define B2L_HSI2C_BASE          0x104B0000UL
#define B2L_USI_CONFIG_BASE      0x10421000UL
#define B2L_USI_CONFIG_SIZE      0x00001000UL
#define B2L_HSI2C_RETRIES       3
#define B2L_HSI2C_TIMEOUT_US    20000

#define HSI2C_CTRL              0x00
#define HSI2C_FIFO_CTRL         0x04
#define HSI2C_TRAIL_CTRL        0x08
#define HSI2C_INT_ENABLE        0x20
#define HSI2C_INT_STATUS        0x24
#define HSI2C_FIFO_STATUS       0x30
#define HSI2C_TX_DATA           0x34
#define HSI2C_RX_DATA           0x38
#define HSI2C_CONF              0x40
#define HSI2C_AUTO_CONF         0x44
#define HSI2C_TIMEOUT           0x48
#define HSI2C_ADDRESS           0x74

#define HSI2C_MASTER            BIT3
#define HSI2C_RXCHON            BIT6
#define HSI2C_TXCHON            BIT7
#define HSI2C_SW_RST            BIT31

#define HSI2C_TRAILING_COUNT    0x00FFFFFF
#define HSI2C_AUTO_MODE         BIT31
#define HSI2C_TIMEOUT_EN        BIT31

#define HSI2C_RXFIFO_EN         BIT0
#define HSI2C_TXFIFO_EN         BIT1
#define HSI2C_RXFIFO_LEVEL(x)   ((x) << 4)
#define HSI2C_TXFIFO_LEVEL(x)   ((x) << 16)

#define HSI2C_READ_WRITE        BIT16
#define HSI2C_STOP_AFTER_TRANS  BIT17
#define HSI2C_MASTER_RUN        BIT31

#define HSI2C_INT_TRANSFER_DONE BIT7

#define HSI2C_TX_FIFO_LEVEL(x)  ((x) & 0x7F)
#define HSI2C_TX_FIFO_EMPTY     BIT8
#define HSI2C_RX_FIFO_EMPTY     BIT24

STATIC
UINTN
Reg (
  IN UINTN  Offset
  )
{
  return B2L_HSI2C_BASE + Offset;
}

STATIC
VOID
ResetController (
  VOID
  )
{
  MmioOr32  (Reg (HSI2C_CTRL), HSI2C_SW_RST);
  MmioAnd32 (Reg (HSI2C_CTRL), ~HSI2C_SW_RST);

  MmioWrite32 (Reg (HSI2C_CTRL), HSI2C_MASTER);
  MmioWrite32 (Reg (HSI2C_TRAIL_CTRL), HSI2C_TRAILING_COUNT);
  MmioOr32    (Reg (HSI2C_CONF), HSI2C_AUTO_MODE);
}

STATIC
EFI_STATUS
TransferMessage (
  IN OUT UINT8    *Buffer,
  IN     UINTN     BufferLength,
  IN     BOOLEAN   IsRead,
  IN     BOOLEAN   Stop
  )
{
  UINT32  AutoConfig;
  UINT32  FifoControl;
  UINT32  FifoStatus;
  UINT32  InterruptStatus;
  UINT32  TargetAddress;
  UINT32  TriggerLevel;
  UINTN   Index;
  UINTN   Timeout;

  if ((Buffer == NULL) || (BufferLength == 0) || (BufferLength > MAX_UINT16)) {
    return EFI_INVALID_PARAMETER;
  }

  MmioAnd32 (Reg (HSI2C_TIMEOUT), ~HSI2C_TIMEOUT_EN);

  TriggerLevel = (BufferLength >= 8) ? 8 : (UINT32)BufferLength;
  FifoControl  = HSI2C_RXFIFO_EN                   |
                 HSI2C_TXFIFO_EN                   |
                 HSI2C_RXFIFO_LEVEL (TriggerLevel) |
                 HSI2C_TXFIFO_LEVEL (TriggerLevel);
  MmioWrite32 (Reg (HSI2C_FIFO_CTRL), FifoControl);

  MmioAndThenOr32 (
    Reg (HSI2C_CTRL),
    IsRead ? ~HSI2C_TXCHON : ~HSI2C_RXCHON,
    IsRead ? HSI2C_RXCHON : HSI2C_TXCHON
    );

  TargetAddress  = MmioRead32 (Reg (HSI2C_ADDRESS));
  TargetAddress &= ~((0x3FFU << 10) | (0x3FFU << 0) | (0xFFU << 24));
  TargetAddress |= (B2L_HSI2C_SLAVE & 0x7F) << 10;
  MmioWrite32 (Reg (HSI2C_ADDRESS), TargetAddress);

  AutoConfig  = MmioRead32 (Reg (HSI2C_AUTO_CONF));
  AutoConfig &= ~(HSI2C_READ_WRITE | HSI2C_STOP_AFTER_TRANS |
                  HSI2C_MASTER_RUN | 0xFFFFU);
  AutoConfig |= IsRead ? HSI2C_READ_WRITE : 0;
  AutoConfig |= Stop ? HSI2C_STOP_AFTER_TRANS : 0;
  AutoConfig |= (UINT32)BufferLength;
  MmioWrite32 (Reg (HSI2C_AUTO_CONF), AutoConfig);

  InterruptStatus = MmioRead32 (Reg (HSI2C_INT_STATUS));
  if (InterruptStatus != 0) {
    MmioWrite32 (Reg (HSI2C_INT_STATUS), InterruptStatus);
  }

  MmioWrite32 (Reg (HSI2C_INT_ENABLE), HSI2C_INT_TRANSFER_DONE);
  MmioOr32 (Reg (HSI2C_AUTO_CONF), HSI2C_MASTER_RUN);

  Index   = 0;
  Timeout = B2L_HSI2C_TIMEOUT_US;

  if (IsRead) {
    while (Timeout != 0) {
      Timeout--;
      if ((Index < BufferLength) &&
          ((MmioRead32 (Reg (HSI2C_FIFO_STATUS)) & HSI2C_RX_FIFO_EMPTY) == 0)) {
        Buffer[Index++] = (UINT8)MmioRead32 (Reg (HSI2C_RX_DATA));
      }

      if (Index >= BufferLength) {
        InterruptStatus = MmioRead32 (Reg (HSI2C_INT_STATUS));
        if ((InterruptStatus & HSI2C_INT_TRANSFER_DONE) != 0) {
          MmioWrite32 (Reg (HSI2C_INT_STATUS), InterruptStatus);
          return EFI_SUCCESS;
        }
      }

      gBS->Stall (1);
    }
  } else {
    while ((Timeout != 0) && (Index < BufferLength)) {
      FifoStatus = MmioRead32 (Reg (HSI2C_FIFO_STATUS));
      if (HSI2C_TX_FIFO_LEVEL (FifoStatus) < 16) {
        MmioWrite32 (Reg (HSI2C_TX_DATA), Buffer[Index++]);
      }

      Timeout--;
      gBS->Stall (1);
    }

    while (Timeout != 0) {
      Timeout--;
      InterruptStatus = MmioRead32 (Reg (HSI2C_INT_STATUS));
      FifoStatus      = MmioRead32 (Reg (HSI2C_FIFO_STATUS));
      if (((InterruptStatus & HSI2C_INT_TRANSFER_DONE) != 0) &&
          ((FifoStatus & HSI2C_TX_FIFO_EMPTY) != 0)) {
        MmioWrite32 (Reg (HSI2C_INT_STATUS), InterruptStatus);
        return EFI_SUCCESS;
      }

      gBS->Stall (1);
    }
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
B2LHsI2cInitialize (
  VOID
  )
{
  EFI_HSI2C_PROTOCOL  *HsI2c;
  EFI_STATUS           Status;

  Status = gBS->LocateProtocol (
                  &gEfiHsI2cProtocolGuid,
                  NULL,
                  (VOID **)&HsI2c
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // HsI2cDxe maps the HSI2C controller, then UsiDxe selects its mode through
  // the separate USI configuration register at 0x1042101C.  Map only that
  // device-MMIO page before InitBus; it is not DRAM and must not be added to
  // the platform's reserved-memory map.
  //
  Status = MapMemoryRegion (
             B2L_USI_CONFIG_BASE,
             B2L_USI_CONFIG_SIZE,
             EfiMemoryMappedIO
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "B2LTouch: failed to map HSI2C10 USI MMIO page: %r\n",
      Status
      ));
    return Status;
  }

  return HsI2c->InitBus (B2L_HSI2C_BUS);
}

EFI_STATUS
B2LHsI2cWriteRead (
  IN  CONST UINT8  *WriteBuffer OPTIONAL,
  IN  UINTN         WriteLength,
  OUT UINT8        *ReadBuffer OPTIONAL,
  IN  UINTN         ReadLength
  )
{
  EFI_STATUS  Status;
  UINTN       Retry;

  if (((WriteLength == 0) && (ReadLength == 0)) ||
      ((WriteLength != 0) && (WriteBuffer == NULL)) ||
      ((ReadLength != 0) && (ReadBuffer == NULL)) ||
      (WriteLength > MAX_UINT16) || (ReadLength > MAX_UINT16)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Retry = 0; Retry < B2L_HSI2C_RETRIES; Retry++) {
    // The stock DT explicitly requests reset-before-trans for HSI2C10.
    ResetController ();

    if (WriteLength != 0) {
      Status = TransferMessage (
                 (UINT8 *)(VOID *)WriteBuffer,
                 WriteLength,
                 FALSE,
                 ReadLength == 0
                 );
      if (EFI_ERROR (Status)) {
        gBS->Stall (100);
        continue;
      }
    }

    if (ReadLength != 0) {
      Status = TransferMessage (ReadBuffer, ReadLength, TRUE, TRUE);
      if (EFI_ERROR (Status)) {
        gBS->Stall (100);
        continue;
      }
    }

    return EFI_SUCCESS;
  }

  ResetController ();
  DEBUG ((DEBUG_ERROR, "B2LTouch: HSI2C10 transfer failed: %r\n", Status));
  return EFI_DEVICE_ERROR;
}
