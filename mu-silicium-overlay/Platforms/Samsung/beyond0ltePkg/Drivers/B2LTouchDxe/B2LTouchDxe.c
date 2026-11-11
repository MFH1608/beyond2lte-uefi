/** @file
  Samsung sec_ts touchscreen driver for Galaxy S10+ (beyond0lte).

  The stock device tree places sec_ts at address 0x48 on HSI2C10 and reports
  12-bit coordinates.  This DXE driver exposes the first supported contact as
  EFI_ABSOLUTE_POINTER_PROTOCOL.  It neither updates firmware nor power-cycles
  the boot-on touchscreen regulators.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/AbsolutePointer.h>

#include "B2LHsI2c.h"
#include "B2LSecTsParser.h"

#define SEC_TS_CMD_SENSE_ON          0x10
#define SEC_TS_READ_FIRMWARE_INTEGRITY 0x21
#define SEC_TS_READ_DEVICE_ID        0x22
#define SEC_TS_READ_BOOT_STATUS      0x55
#define SEC_TS_READ_ONE_EVENT        0x60
#define SEC_TS_READ_ALL_EVENT        0x61
#define SEC_TS_CMD_CLEAR_EVENT_STACK 0x62

#define SEC_TS_STATUS_APP_MODE       0x20
#define SEC_TS_MAX_EVENT_COUNT       32
#define B2L_POINTER_QUEUE_SIZE       32
#define B2L_TOUCH_POLL_INTERVAL      200000ULL // 20 ms in 100 ns units
#define B2L_TOUCH_ERROR_BACKOFF_TICKS 50       // Retry failed I2C after 1 s

typedef struct {
  EFI_ABSOLUTE_POINTER_PROTOCOL  Protocol;
  EFI_ABSOLUTE_POINTER_MODE      Mode;
  B2L_SEC_TS_POINTER             Pointer;
  EFI_ABSOLUTE_POINTER_STATE     Queue[B2L_POINTER_QUEUE_SIZE];
  UINTN                          QueueHead;
  UINTN                          QueueTail;
  UINTN                          QueueCount;
  BOOLEAN                        Polling;
  UINTN                          ErrorCount;
  EFI_EVENT                      PollTimer;
  UINTN                          PollBackoffTicks;
} B2L_TOUCH_CONTEXT;

STATIC B2L_TOUCH_CONTEXT  mTouch;

STATIC
EFI_STATUS
ReadCommand (
  IN  UINT8  Command,
  OUT UINT8 *Data,
  IN  UINTN  DataLength
  )
{
  return B2LHsI2cWriteRead (&Command, 1, Data, DataLength);
}

STATIC
EFI_STATUS
WriteCommand (
  IN UINT8  Command
  )
{
  return B2LHsI2cWriteRead (&Command, 1, NULL, 0);
}

STATIC
BOOLEAN
QueueIsEmpty (
  IN CONST B2L_TOUCH_CONTEXT  *Context
  )
{
  return Context->QueueCount == 0;
}

STATIC
VOID
QueuePointerState (
  IN OUT B2L_TOUCH_CONTEXT  *Context
  )
{
  EFI_ABSOLUTE_POINTER_STATE  *State;

  if (Context->QueueCount == B2L_POINTER_QUEUE_SIZE) {
    Context->QueueHead = (Context->QueueHead + 1) % B2L_POINTER_QUEUE_SIZE;
    Context->QueueCount--;
  }

  State                = &Context->Queue[Context->QueueTail];
  State->CurrentX      = Context->Pointer.X;
  State->CurrentY      = Context->Pointer.Y;
  State->CurrentZ      = Context->Pointer.Pressure;
  State->ActiveButtons = Context->Pointer.Active ? EFI_ABSP_TouchActive : 0;

  Context->QueueTail = (Context->QueueTail + 1) % B2L_POINTER_QUEUE_SIZE;
  Context->QueueCount++;
}

STATIC
VOID
ConsumeEvent (
  IN OUT B2L_TOUCH_CONTEXT  *Context,
  IN     CONST UINT8         Event[B2L_SEC_TS_EVENT_SIZE]
  )
{
  B2L_SEC_TS_PARSE_RESULT  Result;

  Result = B2LSecTsParseEvent (Event, &Context->Pointer);
  if (Result == B2LSecTsUpdated) {
    QueuePointerState (Context);
  } else if (Result == B2LSecTsInvalid) {
    DEBUG ((DEBUG_WARN, "B2LTouch: malformed sec_ts coordinate event\n"));
  }
}

STATIC
EFI_STATUS
PollEvents (
  IN OUT B2L_TOUCH_CONTEXT  *Context
  )
{
  UINT8       Events[SEC_TS_MAX_EVENT_COUNT][B2L_SEC_TS_EVENT_SIZE];
  UINT8       Remaining;
  UINTN       Index;
  EFI_STATUS  Status;

  if (Context->Polling) {
    return EFI_NOT_READY;
  }

  Context->Polling = TRUE;
  Status = ReadCommand (
             SEC_TS_READ_ONE_EVENT,
             Events[0],
             B2L_SEC_TS_EVENT_SIZE
             );
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  // Samsung sec_ts returns an all-zero first byte when its event FIFO is empty.
  if (Events[0][0] == 0) {
    Status = EFI_SUCCESS;
    goto Exit;
  }

  Remaining = B2LSecTsRemainingEvents (Events[0]);
  if (Remaining >= SEC_TS_MAX_EVENT_COUNT) {
    DEBUG ((
      DEBUG_ERROR,
      "B2LTouch: invalid event count %u; clearing controller queue\n",
      Remaining
      ));
    WriteCommand (SEC_TS_CMD_CLEAR_EVENT_STACK);
    Status = EFI_COMPROMISED_DATA;
    goto Exit;
  }

  if (Remaining != 0) {
    Status = ReadCommand (
               SEC_TS_READ_ALL_EVENT,
               &Events[1][0],
               (UINTN)Remaining * B2L_SEC_TS_EVENT_SIZE
               );
    if (EFI_ERROR (Status)) {
      goto Exit;
    }
  }

  for (Index = 0; Index <= Remaining; Index++) {
    ConsumeEvent (Context, Events[Index]);
  }

Exit:
  Context->Polling = FALSE;
  if (EFI_ERROR (Status)) {
    Context->ErrorCount++;
    if ((Context->ErrorCount == 1) || ((Context->ErrorCount & 0x3F) == 0)) {
      DEBUG ((
        DEBUG_ERROR,
        "B2LTouch: event poll failed (%u): %r\n",
        (UINT32)Context->ErrorCount,
        Status
        ));
    }
  } else {
    Context->ErrorCount = 0;
  }

  return Status;
}

STATIC
EFI_STATUS
ProbeController (
  VOID
  )
{
  UINT8       BootStatus;
  UINT8       DeviceId[5];
  UINT8       FirmwareIntegrity;
  UINTN       Index;
  BOOLEAN     AllZero;
  BOOLEAN     AllOnes;
  EFI_STATUS  Status;

  Status = ReadCommand (SEC_TS_READ_DEVICE_ID, DeviceId, sizeof (DeviceId));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AllZero = TRUE;
  AllOnes = TRUE;
  for (Index = 0; Index < sizeof (DeviceId); Index++) {
    AllZero = (BOOLEAN)(AllZero && (DeviceId[Index] == 0x00));
    AllOnes = (BOOLEAN)(AllOnes && (DeviceId[Index] == 0xFF));
  }

  if (AllZero || AllOnes) {
    return EFI_NOT_FOUND;
  }

  Status = ReadCommand (
             SEC_TS_READ_FIRMWARE_INTEGRITY,
             &FirmwareIntegrity,
             sizeof (FirmwareIntegrity)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ReadCommand (
             SEC_TS_READ_BOOT_STATUS,
             &BootStatus,
             sizeof (BootStatus)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "B2LTouch: sec_ts id=%02x%02x%02x%02x%02x integrity=%02x boot=%02x\n",
    DeviceId[0],
    DeviceId[1],
    DeviceId[2],
    DeviceId[3],
    DeviceId[4],
    FirmwareIntegrity,
    BootStatus
    ));

  if ((FirmwareIntegrity & 0x80) == 0) {
    return EFI_COMPROMISED_DATA;
  }

  return (BootStatus == SEC_TS_STATUS_APP_MODE) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  EFI_STATUS  Status;

  if (This != &mTouch.Protocol) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&mTouch.Pointer, sizeof (mTouch.Pointer));
  mTouch.QueueHead        = 0;
  mTouch.QueueTail        = 0;
  mTouch.QueueCount       = 0;
  mTouch.ErrorCount       = 0;
  mTouch.PollBackoffTicks = 0;

  Status = WriteCommand (SEC_TS_CMD_CLEAR_EVENT_STACK);
  if (!EFI_ERROR (Status)) {
    Status = WriteCommand (SEC_TS_CMD_SENSE_ON);
  }

  if (!EFI_ERROR (Status) && ExtendedVerification) {
    Status = ProbeController ();
  }

  return EFI_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
TouchGetState (
  IN  EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  OUT EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  if ((This != &mTouch.Protocol) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (QueueIsEmpty (&mTouch)) {
    return EFI_NOT_READY;
  }

  CopyMem (State, &mTouch.Queue[mTouch.QueueHead], sizeof (*State));
  mTouch.QueueHead = (mTouch.QueueHead + 1) % B2L_POINTER_QUEUE_SIZE;
  mTouch.QueueCount--;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
TouchPollTimer (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  B2L_TOUCH_CONTEXT  *Touch;
  EFI_STATUS          Status;

  (VOID)Event;
  Touch = Context;
  if (Touch->PollBackoffTicks != 0) {
    Touch->PollBackoffTicks--;
    return;
  }

  Status = PollEvents (Touch);
  if (EFI_ERROR (Status)) {
    //
    // A missing or temporarily wedged controller must not monopolize the
    // firmware event loop.  One failed transfer is already bounded by the
    // transport timeout; defer the next attempt for one second.
    //
    Touch->PollBackoffTicks = B2L_TOUCH_ERROR_BACKOFF_TICKS;
  }
}

STATIC
VOID
EFIAPI
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  B2L_TOUCH_CONTEXT  *Touch;

  Touch = Context;
  if (!QueueIsEmpty (Touch)) {
    gBS->SignalEvent (Event);
  }
}

EFI_STATUS
EFIAPI
B2LTouchEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = B2LHsI2cInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "B2LTouch: HSI2C10 init failed: %r\n", Status));
    return Status;
  }

  Status = ProbeController ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "B2LTouch: sec_ts probe failed: %r\n", Status));
    return Status;
  }

  ZeroMem (&mTouch, sizeof (mTouch));
  mTouch.Mode.AbsoluteMinX = 0;
  mTouch.Mode.AbsoluteMinY = 0;
  mTouch.Mode.AbsoluteMinZ = 0;
  mTouch.Mode.AbsoluteMaxX = B2L_SEC_TS_MAX_COORD;
  mTouch.Mode.AbsoluteMaxY = B2L_SEC_TS_MAX_COORD;
  mTouch.Mode.AbsoluteMaxZ = 0x3F;
  mTouch.Mode.Attributes   = EFI_ABSP_SupportsPressureAsZ;

  mTouch.Protocol.Reset    = TouchReset;
  mTouch.Protocol.GetState = TouchGetState;
  mTouch.Protocol.Mode     = &mTouch.Mode;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  TouchWaitForInput,
                  &mTouch,
                  &mTouch.Protocol.WaitForInput
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  TouchPollTimer,
                  &mTouch,
                  &mTouch.PollTimer
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mTouch.Protocol.WaitForInput);
    return Status;
  }

  Status = TouchReset (&mTouch.Protocol, FALSE);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mTouch.PollTimer);
    gBS->CloseEvent (mTouch.Protocol.WaitForInput);
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiAbsolutePointerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mTouch.Protocol
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mTouch.PollTimer);
    gBS->CloseEvent (mTouch.Protocol.WaitForInput);
    return Status;
  }

  Status = gBS->SetTimer (
                  mTouch.PollTimer,
                  TimerPeriodic,
                  B2L_TOUCH_POLL_INTERVAL
                  );
  if (EFI_ERROR (Status)) {
    gBS->UninstallProtocolInterface (
           ImageHandle,
           &gEfiAbsolutePointerProtocolGuid,
           &mTouch.Protocol
           );
    gBS->CloseEvent (mTouch.PollTimer);
    gBS->CloseEvent (mTouch.Protocol.WaitForInput);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "B2LTouch: EFI Absolute Pointer installed\n"));
  return EFI_SUCCESS;
}
