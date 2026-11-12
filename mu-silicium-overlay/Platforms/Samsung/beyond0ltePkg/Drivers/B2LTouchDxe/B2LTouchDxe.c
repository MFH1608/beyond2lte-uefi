/** @file
  Samsung sec_ts touchscreen driver for Galaxy S10e (beyond0lte).

  The stock device tree places sec_ts at address 0x48 on HSI2C10 and reports
  12-bit coordinates.  This DXE driver exposes the first supported contact as
  EFI_ABSOLUTE_POINTER_PROTOCOL.  It powers the tsp_io/tsp_avdd rails through
  the PMIC regulator protocol but does not touch panel firmware.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/AbsolutePointer.h>
#include <Protocol/EFIPmicRegulator.h>

#include <Device/Pmic.h>

#include "B2LHsI2c.h"
#include "B2LSecTsParser.h"
#include "B2LTouchDiag.h"

#define SEC_TS_CMD_SENSE_ON          0x10
#define SEC_TS_READ_FIRMWARE_INTEGRITY 0x21
#define SEC_TS_READ_DEVICE_ID        0x22
#define SEC_TS_CMD_SET_TOUCHFUNCTION 0x30
#define SEC_TS_READ_BOOT_STATUS      0x55
#define SEC_TS_READ_ONE_EVENT        0x60
#define SEC_TS_READ_ALL_EVENT        0x61
#define SEC_TS_CMD_CLEAR_EVENT_STACK 0x62
#define SEC_TS_CMD_SET_POWER_MODE    0xE4

//
// sec_ts dokunma islevleri: TOUCH|PALM|WET. Linux sec_ts "touch_functions"
// registeri 16-bit little-endian'dir, bu yuzden iki bayt yazilir.
//
#define B2L_TOUCH_FUNCTION_TOUCH     (1 << 0)
#define B2L_TOUCH_FUNCTION_PALM      (1 << 5)
#define B2L_TOUCH_FUNCTION_WET       (1 << 6)
#define B2L_TOUCH_FUNCTION_DEFAULT   (B2L_TOUCH_FUNCTION_TOUCH | \
                                      B2L_TOUCH_FUNCTION_PALM  | \
                                      B2L_TOUCH_FUNCTION_WET)

#define SEC_TS_MODE_SCAN            0x00
#define SEC_TS_MODE_LOWPOWER        0x01

#define SEC_TS_STATUS_APP_MODE       0x20
#define SEC_TS_MAX_EVENT_COUNT       32
#define B2L_POINTER_QUEUE_SIZE       32
#define B2L_TOUCH_POLL_INTERVAL      200000ULL // 20 ms in 100 ns units
#define B2L_TOUCH_ERROR_BACKOFF_TICKS 50       // Retry failed I2C after 1 s
//
// GetState() uzerinden istege bagli yoklama araligi. BDS'in "no OS"
// ekrani gibi kendi dongusunde calisan tuketiciler gBS->Stall ile
// beklediginden TPL_CALLBACK zamanlayicisi hic calismayabilir; bu yuzden
// GetState kuyrugu bos gorurse kendisi de (en fazla bu aralikta bir)
// denetleyiciyi okur.
//
#define B2L_TOUCH_ONDEMAND_INTERVAL_NS  20000000ULL // 20 ms

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
  UINT64                         LastPollNs;
} B2L_TOUCH_CONTEXT;

STATIC B2L_TOUCH_CONTEXT  mTouch;

STATIC B2L_TOUCH_DIAG_PROTOCOL  mTouchDiag;

STATIC CONST EFI_GUID  mB2LTouchDiagProtocolGuid = B2L_TOUCH_DIAG_PROTOCOL_GUID;

//
// Dokunma guc raylari (dts fragment@32, s2mps19mfd):
//   tsp_io   = LDO19M -> S2MPS19 LDO19, 1.8 V
//   tsp_avdd = LDO27M -> S2MPS19 LDO27, 3.0 V
// Her ikisi de "regulator-boot-on"; bootloader acmadiysa I2C hatti panel
// uyanmadan NACK verir. APRIORI'de PmicDxe B2LTouchDxe'den ONCE gelir, ama
// yine de entry aninda protokol yoksa Reset(TRUE) bir kez daha dener.
//
#define B2L_TOUCH_PMIC_LDO_IO    19
#define B2L_TOUCH_PMIC_LDO_AVDD  27
#define B2L_TOUCH_IO_UV          1800000U
#define B2L_TOUCH_AVDD_UV        3000000U
#define B2L_TOUCH_POWER_ON_US    100000U   // panelin uyanmasi icin bekleme

/**
  Basarisiz komut/transfer sonrasi teshis alanlarini doldurur: gercek
  EFI_STATUS, HSI2C10 durum registerlari ve (ilk kez) adres taramasi.
**/
STATIC
VOID
B2LTouchRecordFailure (
  IN EFI_STATUS  Status
  )
{
  UINT32  IntStatus;
  UINT32  TransStatus;

  mTouchDiag.Diag.LastStatus = (UINT64)(UINTN)Status;

  B2LHsI2cSnapshot (&IntStatus, &TransStatus);
  mTouchDiag.Diag.IntStatus   = IntStatus;
  mTouchDiag.Diag.TransStatus = TransStatus;

  if (!mTouchDiag.Diag.ScanDone) {
    B2LHsI2cScan (mTouchDiag.Diag.ScanAck);
    mTouchDiag.Diag.ScanDone = 1;
  }
}

/**
  tsp_io/tsp_avdd raylarini ayarlayip acar. PMIC regulator protokolu
  yoksa (ornegin APRIORI'de PmicDxe'den once) sessizce basarisiz olur;
  ekran bunu Touch.pm satirinda gosterir. Idempotenttir.
**/
STATIC
EFI_STATUS
B2LTouchPowerOn (
  VOID
  )
{
  EFI_PMIC_REGULATOR_PROTOCOL  *Regulator;
  EFI_STATUS                    Status;
  UINT32                        Microvolts;

  if (mTouchDiag.Diag.PmicLocate == B2L_TOUCH_PMIC_FOUND) {
    // Daha once bulundu; sonucu tekrar kullan.
    return (mTouchDiag.Diag.PowerOk != 0) ? EFI_SUCCESS : EFI_NOT_READY;
  }

  Status = gBS->LocateProtocol (
                  &gEfiPmicRegulatorProtocolGuid,
                  NULL,
                  (VOID **)&Regulator
                  );
  if (EFI_ERROR (Status) || (Regulator == NULL)) {
    mTouchDiag.Diag.PmicLocate = (UINT32)(UINTN)Status;
    return Status;
  }

  mTouchDiag.Diag.PmicLocate = B2L_TOUCH_PMIC_FOUND;

  // Once gerilimi, sonra enable'i ayarla (SetLdoVoltage enable bitine
  // dokunmaz, SetLdo yalnizca mod bitlerini yazar).
  Regulator->SetLdoVoltage (ID_S2MPS19, B2L_TOUCH_PMIC_LDO_IO, B2L_TOUCH_IO_UV);
  Regulator->SetLdoVoltage (ID_S2MPS19, B2L_TOUCH_PMIC_LDO_AVDD, B2L_TOUCH_AVDD_UV);

  Status = Regulator->SetLdo (
                        ID_S2MPS19,
                        B2L_TOUCH_PMIC_LDO_IO,
                        S2MPS_MODE_NORMAL,
                        TRUE
                        );
  mTouchDiag.Diag.LdoSetOk[0] = EFI_ERROR (Status) ? 0 : 1;

  Status = Regulator->SetLdo (
                        ID_S2MPS19,
                        B2L_TOUCH_PMIC_LDO_AVDD,
                        S2MPS_MODE_NORMAL,
                        TRUE
                        );
  mTouchDiag.Diag.LdoSetOk[1] = EFI_ERROR (Status) ? 0 : 1;

  if (!EFI_ERROR (Regulator->GetLdoVoltage (ID_S2MPS19, B2L_TOUCH_PMIC_LDO_IO, &Microvolts))) {
    mTouchDiag.Diag.LdoUv[0] = Microvolts;
  }

  if (!EFI_ERROR (Regulator->GetLdoVoltage (ID_S2MPS19, B2L_TOUCH_PMIC_LDO_AVDD, &Microvolts))) {
    mTouchDiag.Diag.LdoUv[1] = Microvolts;
  }

  mTouchDiag.Diag.PowerOk = (mTouchDiag.Diag.LdoSetOk[0] &&
                             mTouchDiag.Diag.LdoSetOk[1]) ? 1 : 0;

  // Raylar yeni acildiysa panelin firmware'i ayaga kalksin.
  gBS->Stall (B2L_TOUCH_POWER_ON_US);

  return mTouchDiag.Diag.PowerOk ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
ReadCommand (
  IN  UINT8  Command,
  OUT UINT8 *Data,
  IN  UINTN  DataLength
  )
{
  EFI_STATUS  Status;

  Status = B2LHsI2cWriteRead (&Command, 1, Data, DataLength);
  if (EFI_ERROR (Status)) {
    B2LTouchRecordFailure (Status);
  }

  return Status;
}

STATIC
EFI_STATUS
WriteCommand (
  IN UINT8  Command
  )
{
  EFI_STATUS  Status;

  Status = B2LHsI2cWriteRead (&Command, 1, NULL, 0);
  if (EFI_ERROR (Status)) {
    B2LTouchRecordFailure (Status);
  }

  return Status;
}

STATIC
EFI_STATUS
WriteCommandData (
  IN UINT8        Command,
  IN CONST UINT8  *Data,
  IN UINTN        DataLength
  )
{
  UINT8       Buffer[4];
  EFI_STATUS  Status;

  if (DataLength > (sizeof (Buffer) - 1)) {
    return EFI_INVALID_PARAMETER;
  }

  Buffer[0] = Command;
  if (DataLength != 0) {
    CopyMem (&Buffer[1], Data, DataLength);
  }

  Status = B2LHsI2cWriteRead (Buffer, DataLength + 1, NULL, 0);
  if (EFI_ERROR (Status)) {
    B2LTouchRecordFailure (Status);
  }

  return Status;
}

STATIC
UINT64
TouchNowNs (
  VOID
  )
{
  return GetTimeInNanoSecond (GetPerformanceCounter ());
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

  CopyMem (mTouchDiag.Diag.LastEvent, Event, B2L_TOUCH_DIAG_EVENT_SIZE);

  if ((Event[0] & 0x03) == 0) {
    mTouchDiag.Diag.CoordEvents++;
  } else {
    mTouchDiag.Diag.StatusEvents++;
  }

  Result = B2LSecTsParseEvent (Event, &Context->Pointer);
  if (Result == B2LSecTsUpdated) {
    QueuePointerState (Context);
  } else {
    mTouchDiag.Diag.Ignored++;
    if (Result == B2LSecTsInvalid) {
      DEBUG ((DEBUG_WARN, "B2LTouch: malformed sec_ts coordinate event\n"));
    }
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

  mTouchDiag.Diag.Polls++;

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
    mTouchDiag.Diag.Empty++;
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
  Context->Polling     = FALSE;
  Context->LastPollNs  = TouchNowNs ();
  if (EFI_ERROR (Status)) {
    mTouchDiag.Diag.Errors++;
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

  mTouchDiag.Diag.ProbeCalls++;

  Status = ReadCommand (SEC_TS_READ_DEVICE_ID, DeviceId, sizeof (DeviceId));
  if (!EFI_ERROR (Status)) {
    CopyMem (mTouchDiag.Diag.DeviceId, DeviceId, sizeof (DeviceId));
  }

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

  mTouchDiag.Diag.Integrity  = FirmwareIntegrity;
  mTouchDiag.Diag.BootStatus = BootStatus;

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
  UINT8       TouchFunction[2];
  UINT8       PowerMode;
  UINT8       ScanMode;
  UINT8       ReadStatus;

  if (This != &mTouch.Protocol) {
    return EFI_INVALID_PARAMETER;
  }

  mTouchDiag.Diag.ResetCalls++;

  //
  // Dokunma guc raylarini her Reset'te yeniden uygula. Entry sirasinda
  // (APRIORI) PmicDxe henuz yuklenmemis olabilir; self-test ekrani
  // Reset(TRUE)'yu tum suruculer yuklendikten sonra cagirir.
  //
  B2LTouchPowerOn ();

  ZeroMem (&mTouch.Pointer, sizeof (mTouch.Pointer));
  mTouch.QueueHead        = 0;
  mTouch.QueueTail        = 0;
  mTouch.QueueCount       = 0;
  mTouch.ErrorCount       = 0;
  mTouch.PollBackoffTicks = 0;

  mTouchDiag.Diag.FnBefore[0] = 0xFF;
  mTouchDiag.Diag.FnBefore[1] = 0xFF;
  mTouchDiag.Diag.FnAfter[0]  = 0xFF;
  mTouchDiag.Diag.FnAfter[1]  = 0xFF;
  mTouchDiag.Diag.PowerMode   = 0xFF;
  mTouchDiag.Diag.BringUpOk   = 0;

  //
  // sec_ts'i olay uretir hale getir. Probe basarili oldugu icin komut
  // haritasi (0x21/0x22/0x55/0x60) ZATEN dogru; ayni haritadaki 0x30 ve
  // 0xE4 de guvenle kullanilabilir. Linux sec_ts_reinit/start_device'in
  // ozeti:
  //   1) dokunma islev bitleri (0x30, 16-bit LE: TOUCH|PALM|WET)
  //   2) LPM'deyse tarama moduna don (0xE4 = 0)
  //   3) olay yigini temizle (0x62)
  //   4) sensing ac (0x10)
  //
  Status = ReadCommand (SEC_TS_CMD_SET_TOUCHFUNCTION, mTouchDiag.Diag.FnBefore, 2);
  if (EFI_ERROR (Status)) {
    mTouchDiag.Diag.FnBefore[0] = 0xFF;
    mTouchDiag.Diag.FnBefore[1] = 0xFF;
  }

  TouchFunction[0] = (UINT8)(B2L_TOUCH_FUNCTION_DEFAULT & 0xFF);
  TouchFunction[1] = (UINT8)((B2L_TOUCH_FUNCTION_DEFAULT >> 8) & 0xFF);
  Status = WriteCommandData (SEC_TS_CMD_SET_TOUCHFUNCTION, TouchFunction, 2);

  if (!EFI_ERROR (Status)) {
    ReadStatus = ReadCommand (SEC_TS_CMD_SET_POWER_MODE, &PowerMode, 1);
    if (!EFI_ERROR (ReadStatus)) {
      mTouchDiag.Diag.PowerMode = PowerMode;
      if (PowerMode == SEC_TS_MODE_LOWPOWER) {
        ScanMode = SEC_TS_MODE_SCAN;
        WriteCommandData (SEC_TS_CMD_SET_POWER_MODE, &ScanMode, 1);
      }
    }
  }

  if (!EFI_ERROR (Status)) {
    ReadCommand (SEC_TS_CMD_SET_TOUCHFUNCTION, mTouchDiag.Diag.FnAfter, 2);
    Status = WriteCommand (SEC_TS_CMD_CLEAR_EVENT_STACK);
  }

  if (!EFI_ERROR (Status)) {
    Status = WriteCommand (SEC_TS_CMD_SENSE_ON);
  }

  mTouchDiag.Diag.BringUpOk = EFI_ERROR (Status) ? 0 : 1;

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
  EFI_STATUS  Status;

  if ((This != &mTouch.Protocol) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (QueueIsEmpty (&mTouch)) {
    //
    // Kendi dongusunde calisan tuketiciler (ornegin BDS'in no-OS self-test
    // ekrani) gBS->Stall ile bekledigi icin periyodik TPL_CALLBACK
    // zamanlayicisi hic islemeyebilir. Kuyruk bossa ve son yoklamadan bu
    // yana yeterince sure gectiyse burada da denetleyiciyi oku.
    //
    Status = EFI_NOT_READY;
    if (!mTouch.Polling &&
        ((TouchNowNs () - mTouch.LastPollNs) >= B2L_TOUCH_ONDEMAND_INTERVAL_NS))
    {
      Status = PollEvents (&mTouch);
    }

    if (QueueIsEmpty (&mTouch)) {
      //
      // EFI_NOT_READY = okuma basarili ama FIFO bos (dokunma yok / panel
      // taramiyor); EFI_DEVICE_ERROR = gercek I2C hatasi. EFI_NOT_READY de
      // makro acisindan "error" sayildigi icin ayri ayirmak zorunlu; aksi
      // halde dokunulmadigi her karede yanlislikla Device Error donuyordu.
      //
      if (EFI_ERROR (Status) && (Status != EFI_NOT_READY)) {
        return EFI_DEVICE_ERROR;
      }

      return EFI_NOT_READY;
    }
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

  //
  // Teshis protokolu ILK is olarak yayinlanir. Surucu sonraki adimlardan
  // birinde basarisiz olursa eskiden entry hata donduruyordu; DXE core da
  // hata donen bir driver'in imajini bellekten kaldiriyor
  // (CoreUnloadAndCloseImage). O zaman `.bss` icindeki mTouchDiag gecersiz
  // olur ve self-test ekrani "no diag protocol" gorur.
  //
  // Bu yuzden protokol kurulduktan sonra entry HER ZAMAN EFI_SUCCESS doner;
  // gercek durum Stage/EntryStatus alanlarindan okunur.
  //
  ZeroMem (&mTouchDiag, sizeof (mTouchDiag));
  mTouchDiag.Diag.Signature = B2L_TOUCH_DIAG_SIGNATURE;
  mTouchDiag.Diag.Size      = (UINT32)sizeof (B2L_TOUCH_DIAG);
  mTouchDiag.Diag.Stage     = B2L_TOUCH_STAGE_ENTRY;
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  (EFI_GUID *)(VOID *)&mB2LTouchDiagProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mTouchDiag
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_I2C;
  Status = B2LHsI2cInitialize ();
  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "B2LTouch: HSI2C10 init failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  //
  // Baglam ve AbsolutePointer protokolu, probe'dan ONCE kurulur. Boylece
  // probe ilk denemede basarisiz olsa bile self-test ekrani Reset(TRUE)
  // cagirarak bring-up'i (PmicDxe vb. yuklendikten sonra) tekrar deneyebilir.
  //
  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_EVENTS;
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
  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_INSTALL;
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiAbsolutePointerProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mTouch.Protocol
                  );
  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mTouch.Protocol.WaitForInput);
    return EFI_SUCCESS;
  }

  //
  // Self-test ekrani ConSplitter'in toplu AbsolutePointer'i (mode 0x10000)
  // yerine dogrudan bizim protokolu kullanabilsin.
  //
  mTouchDiag.Diag.AbsPointer = (UINT64)(UINTN)&mTouch.Protocol;

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_PROBE;
  B2LTouchPowerOn ();
  Status = ProbeController ();
  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "B2LTouch: sec_ts probe failed: %r\n", Status));
    return EFI_SUCCESS;
  }

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_RESET;
  Status = TouchReset (&mTouch.Protocol, FALSE);
  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_TIMER;
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  TouchPollTimer,
                  &mTouch,
                  &mTouch.PollTimer
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (
                    mTouch.PollTimer,
                    TimerPeriodic,
                    B2L_TOUCH_POLL_INTERVAL
                    );
  }

  mTouchDiag.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    if (mTouch.PollTimer != NULL) {
      gBS->CloseEvent (mTouch.PollTimer);
      mTouch.PollTimer = NULL;
    }

    return EFI_SUCCESS;
  }

  mTouchDiag.Diag.Stage = B2L_TOUCH_STAGE_DONE;
  DEBUG ((DEBUG_INFO, "B2LTouch: EFI Absolute Pointer installed\n"));

  return EFI_SUCCESS;
}
