/** @file
  Exynos 9820 (beyond0lte) DWC3 device-mode sinif surucusu - M2a.

  Amac: EFI_USBFN_IO_PROTOCOL (UsbfnDwc3Dxe) uzerinde DURAN en kucuk USB
  aygit yiginini kurup PC'de ENUMERASYONU kanitlamak. M2a'da sinif henuz
  CDC-ACM degil: tek arayuz, uc nokta yok. Boylece kontrol-aktarim motoru ve
  enumerasyon ayri ayri dogrulanir; CDC-ACM (M2b) sonra descriptor + sinif
  istekleri eklenerek yapilir.

  Sozlesme (bkz. UsbfnDwc3Dxe.c):
    - StartController: PHY + DWC3 core.
    - ConfigureEnableEndpoints: EP0 + interface[0] Bulk uc noktalarini kurar
      ve DCTL.RUN_STOP ile portu aktif eder. Bu surumde interface[0]'in
      hicbir uc noktasi yok, yani yalnizca EP0.
    - EventHandler tekrar tekrar cagrilir; Setup paketleri burada gelir.
    - Surucu SET_ADDRESS'i kendisi yapar; SET_CONFIGURATION/SET_INTERFACE'i
      ise once tum non-control uc noktalari sifirlayip bize yollar. Standart
      isteklerin geri kalani (GET_DESCRIPTOR, GET_STATUS, ...) BIZE gelir.

  Olaylar bir zamanlayici ile pompa edilir (TPL_CALLBACK). Self-test ana
  dongusu her karede Raise/RestoreTPL yapip bekleyen notify'lari isledigi
  icin bu, shell/konsol olmadan da calisir.
**/

#include <Uefi.h>

#include <IndustryStandard/Usb.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/UsbFunctionIo.h>

#include "B2LUsbSerialDiag.h"

//
// Pompalama periyodu (100 ns birim): 1 ms.
//
#define B2L_USB_PUMP_PERIOD     10000

//
// EP0 tamponu (DMA uyumlu, AllocateTransferBuffer ile alinir).
//
#define B2L_USB_EP0_BUF_SIZE    512

#define B2L_USB_VENDOR_ID       0x04E8
#define B2L_USB_PRODUCT_ID      0x2143

//
// DWC3 durum registerlari (bkz. S5E9820Pkg/Library/PlatformUsbfnLib).
//
#define B2L_DWC3_BASE           0x10C00000
#define B2L_DWC3_GCTL           0xC110
#define B2L_DWC3_DCFG           0xC700
#define B2L_DWC3_DCTL           0xC704
#define B2L_DWC3_DSTS           0xC70C

//
// DCFG.DEVADDR: bit 3..9. SET_ADDRESS'i UsbfnDwc3Dxe kendisi yazar, bize
// yollamaz; bu yuzden adresi donanimdan okuyup ekrana oyle basariz.
//
#define B2L_DWC3_DEVADDR(Dcfg)  (((Dcfg) >> 3) & 0x7F)

STATIC EFI_USBFN_IO_PROTOCOL         *mFn;
STATIC B2L_USB_SERIAL_DIAG_PROTOCOL   mDiagProtocol;
STATIC EFI_GUID                       mDiagGuid = B2L_USB_SERIAL_DIAG_PROTOCOL_GUID;
STATIC EFI_EVENT                      mTimer;
STATIC VOID                          *mEp0Buffer;
STATIC BOOLEAN                        mConfigured;

STATIC CONST CHAR16  mManStr[]    = L"Mu-Silicium";
STATIC CONST CHAR16  mProdStr[]   = L"Samsung Exynos Gadget";
STATIC CONST CHAR16  mSerialStr[] = L"0123456789ABCDEF";

//
// Cihaz tanimlayici.
//
STATIC CONST USB_DEVICE_DESCRIPTOR  mDeviceDescriptor = {
  18,                         // Length
  USB_DESC_TYPE_DEVICE,       // DescriptorType
  0x0200,                     // BcdUSB
  0xFF,                       // DeviceClass (vendor-specific)
  0x00,                       // DeviceSubClass
  0x00,                       // DeviceProtocol
  64,                         // MaxPacketSize0
  B2L_USB_VENDOR_ID,          // IdVendor
  B2L_USB_PRODUCT_ID,         // IdProduct
  0x0100,                     // BcdDevice
  1,                          // StrManufacturer
  2,                          // StrProduct
  3,                          // StrSerialNumber
  1                           // NumConfigurations
};

//
// Konfigurasyon blogu: 1 konfigurasyon + 1 arayuz (uc nokta yok). USB 2.0
// FS/HS enumerasyonu icin yeterli; GET_DESCRIPTOR(CONFIG) bu blogu dondurur.
//
#pragma pack(1)
typedef struct {
  USB_CONFIG_DESCRIPTOR     Config;
  USB_INTERFACE_DESCRIPTOR  Interface;
} B2L_USB_CONFIG_BLOB;
#pragma pack()

STATIC CONST B2L_USB_CONFIG_BLOB  mConfigBlob = {
  { 9, USB_DESC_TYPE_CONFIG, sizeof (B2L_USB_CONFIG_BLOB), 1, 1, 0, 0x80, 0x32 },
  { 9, USB_DESC_TYPE_INTERFACE, 0, 0, 0, 0xFF, 0x00, 0x00, 0 }
};

//
// ConfigureEnableEndpoints'in bekledigi yerlesim. Surucu yalnizca
// InterfaceInfoTable[0]'i kullanir; uc nokta listesi bu yuzden bos.
//
STATIC USB_ENDPOINT_DESCRIPTOR  *mEndpointTable[1]   = { NULL };
STATIC EFI_USB_INTERFACE_INFO    mInterfaceInfo      = {
  (USB_INTERFACE_DESCRIPTOR *)&mConfigBlob.Interface,
  mEndpointTable
};
STATIC EFI_USB_INTERFACE_INFO   *mInterfaceInfoTable[1] = { &mInterfaceInfo };
STATIC EFI_USB_CONFIG_INFO       mConfigInfo         = {
  (USB_CONFIG_DESCRIPTOR *)&mConfigBlob.Config,
  mInterfaceInfoTable
};
STATIC EFI_USB_CONFIG_INFO      *mConfigInfoTable[1] = { &mConfigInfo };
STATIC EFI_USB_DEVICE_INFO       mDeviceInfo         = {
  (USB_DEVICE_DESCRIPTOR *)&mDeviceDescriptor,
  mConfigInfoTable
};

/**
  EP0'i STALL et. Surucu (UsbfnDwc3Dxe) EP0 icin yonu yok sayar; yine de
  istegin yonunde gondeririz.
**/
STATIC
VOID
B2LUsbStallEp0 (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  BOOLEAN                       State = TRUE;
  EFI_USBFN_ENDPOINT_DIRECTION  Direction;

  Direction = ((Request->RequestType & USB_ENDPOINT_DIR_IN) != 0)
                ? EfiUsbEndpointDirectionDeviceTx
                : EfiUsbEndpointDirectionDeviceRx;

  mDiagProtocol.Diag.Stalls++;

  if (mFn->SetEndpointStallState != NULL) {
    mFn->SetEndpointStallState (mFn, 0, Direction, &State);
  }
}

/**
  EP0'a veri yaz. Veriyi DMA uyumlu mEp0Buffer'a kopyalar; surucu uzunlugu
  istegin wLength'ine gore kirpar.
**/
STATIC
VOID
B2LUsbSendEp0 (
  IN VOID   *Data,
  IN UINTN   Length
  )
{
  UINTN  Size = Length;

  if ((Length != 0) && (Data != NULL) && (Data != mEp0Buffer)) {
    CopyMem (mEp0Buffer, Data, Length);
  }

  if (mFn->Transfer (mFn, 0, EfiUsbEndpointDirectionDeviceTx, &Size, mEp0Buffer) != EFI_SUCCESS) {
    mDiagProtocol.Diag.Stalls++;
  }
}

/**
  GET_DESCRIPTOR(STRING) yaniti. Dizeler UTF-16LE olarak elle yazilir; boylece
  CHAR16 literal uzunluk/terminator sorunlari olmaz.
**/
STATIC
VOID
B2LUsbSendString (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  CONST CHAR16  *Str;
  UINT8         *Dst = (UINT8 *)mEp0Buffer;
  UINTN          Len;
  UINTN          Index;
  UINT8          StrIndex = (UINT8)(Request->Value & 0xFF);

  switch (StrIndex) {
    case 0:
      //
      // LANGID: 0x0409 (Ingilizce, ABD).
      //
      Dst[0] = 4;
      Dst[1] = USB_DESC_TYPE_STRING;
      Dst[2] = 0x09;
      Dst[3] = 0x04;
      B2LUsbSendEp0 (mEp0Buffer, 4);
      return;

    case 1: Str = mManStr;    break;
    case 2: Str = mProdStr;   break;
    case 3: Str = mSerialStr; break;

    default:
      B2LUsbStallEp0 (Request);
      return;
  }

  Len    = StrLen (Str);
  Dst[0] = (UINT8)(2 + (Len * 2));
  Dst[1] = USB_DESC_TYPE_STRING;

  for (Index = 0; Index < Len; Index++) {
    Dst[2 + (Index * 2)]     = (UINT8)(Str[Index] & 0xFF);
    Dst[2 + (Index * 2) + 1] = (UINT8)((Str[Index] >> 8) & 0xFF);
  }

  B2LUsbSendEp0 (mEp0Buffer, 2 + (Len * 2));
}

/**
  Setup paketi isle. Surucunun kendisi SET_ADDRESS'i yaptigi icin burada
  yalnizca gercek standart istekler + (M2b'de) sinif istekleri gelir.
**/
STATIC
VOID
B2LUsbHandleSetup (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  UINT16  StatusWord;
  UINT8   Byte;

  mDiagProtocol.Diag.Setups++;
  mDiagProtocol.Diag.LastBRequest      = Request->Request;
  mDiagProtocol.Diag.LastBMRequestType = Request->RequestType;
  mDiagProtocol.Diag.LastValue         = Request->Value;

  if ((Request->RequestType & (0x03U << 5)) != USB_REQ_TYPE_STANDARD) {
    //
    // M2a'da sinif istegi yok; bilinmeyeni STALL et.
    //
    B2LUsbStallEp0 (Request);
    return;
  }

  switch (Request->Request) {
    case USB_REQ_GET_DESCRIPTOR:
      switch (Request->Value >> 8) {
        case USB_DESC_TYPE_DEVICE:
          if ((Request->Value & 0xFF) != 0) {
            B2LUsbStallEp0 (Request);
            return;
          }

          B2LUsbSendEp0 ((VOID *)&mDeviceDescriptor, sizeof (mDeviceDescriptor));
          return;

        case USB_DESC_TYPE_CONFIG:
          if ((Request->Value & 0xFF) != 0) {
            B2LUsbStallEp0 (Request);
            return;
          }

          B2LUsbSendEp0 ((VOID *)&mConfigBlob, sizeof (mConfigBlob));
          return;

        case USB_DESC_TYPE_STRING:
          B2LUsbSendString (Request);
          return;

        default:
          //
          // DEVICE_QUALIFIER vb. FS/HS aygit icin STALL dogru yanittir.
          //
          B2LUsbStallEp0 (Request);
          return;
      }

    case USB_REQ_GET_STATUS:
      StatusWord = 0;
      B2LUsbSendEp0 (&StatusWord, sizeof (StatusWord));
      return;

    case USB_REQ_GET_CONFIG:
      Byte = mConfigured ? 1 : 0;
      B2LUsbSendEp0 (&Byte, 1);
      return;

    case USB_REQ_GET_INTERFACE:
      Byte = 0;
      B2LUsbSendEp0 (&Byte, 1);
      return;

    case USB_REQ_SET_CONFIG:
      mConfigured = (BOOLEAN)(Request->Value != 0);
      mDiagProtocol.Diag.Configured = mConfigured ? 1 : 0;
      //
      // Veri asamasi yok; durum asamasi donanim tarafindan tamamlanir.
      //
      return;

    case USB_REQ_SET_INTERFACE:
    case USB_REQ_SET_FEATURE:
    case USB_REQ_CLEAR_FEATURE:
      return;

    default:
      B2LUsbStallEp0 (Request);
      return;
  }
}

/**
  Zamanlayici notify'i: bekleyen tum USB olaylarini bosalt.
**/
STATIC
VOID
EFIAPI
B2LUsbPump (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  EFI_USBFN_MESSAGE          Message;
  EFI_USBFN_MESSAGE_PAYLOAD  Payload;
  UINTN                      PayloadSize;
  EFI_STATUS                 Status;
  UINT32                     Guard;

  if ((mFn == NULL) || (mFn->EventHandler == NULL)) {
    return;
  }

  mDiagProtocol.Diag.Pumps++;

  for (Guard = 0; Guard < 64; Guard++) {
    Message     = EfiUsbMsgNone;
    PayloadSize = sizeof (Payload);
    ZeroMem (&Payload, sizeof (Payload));

    Status = mFn->EventHandler (mFn, &Message, &PayloadSize, &Payload);
    if (EFI_ERROR (Status)) {
      break;
    }

    if (Message == EfiUsbMsgNone) {
      break;
    }

    switch (Message) {
      case EfiUsbMsgSetupPacket:
        B2LUsbHandleSetup (&Payload.udr);
        break;

      case EfiUsbMsgEndpointStatusChangedTx:
        mDiagProtocol.Diag.TxDone++;
        break;

      case EfiUsbMsgEndpointStatusChangedRx:
        mDiagProtocol.Diag.RxDone++;
        break;

      case EfiUsbMsgBusEventReset:
        mConfigured                    = FALSE;
        mDiagProtocol.Diag.Configured  = 0;
        mDiagProtocol.Diag.Addr        = 0;
        mDiagProtocol.Diag.BusEvents++;
        break;

      case EfiUsbMsgBusEventAttach:
      case EfiUsbMsgBusEventDetach:
      case EfiUsbMsgBusEventSuspend:
      case EfiUsbMsgBusEventResume:
      case EfiUsbMsgBusEventSpeed:
        mDiagProtocol.Diag.BusEvents++;
        break;

      default:
        break;
    }
  }

  //
  // RUN_STOP gorunur olsun: her pompalama sonrasi DWC3 durumunu tazele.
  // Adres dogrudan DCFG'den okunur, cunku SET_ADDRESS'i controller surucusu
  // kendisi yazar (bize ulasmaz).
  //
  mDiagProtocol.Diag.Gctl = MmioRead32 (B2L_DWC3_BASE + B2L_DWC3_GCTL);
  mDiagProtocol.Diag.Addr = B2L_DWC3_DEVADDR (MmioRead32 (B2L_DWC3_BASE + B2L_DWC3_DCFG));
  mDiagProtocol.Diag.Dctl = MmioRead32 (B2L_DWC3_BASE + B2L_DWC3_DCTL);
  mDiagProtocol.Diag.Dsts = MmioRead32 (B2L_DWC3_BASE + B2L_DWC3_DSTS);
}

EFI_STATUS
EFIAPI
B2LUsbSerialEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  mDiagProtocol.Diag.Signature   = B2L_USB_SERIAL_DIAG_SIGNATURE;
  mDiagProtocol.Diag.Size        = sizeof (B2L_USB_SERIAL_DIAG);
  mDiagProtocol.Diag.Stage       = B2L_USB_STAGE_ENTRY;
  mDiagProtocol.Diag.EntryStatus = 0;

  //
  // 1) Teshis protokolunu ILK is yayinla: sonraki bir adim coksede ekranda
  //    hangi asamanin basarisiz oldugu gorunur.
  //
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &mDiagGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDiagProtocol
                  );
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_INSTALL;

  //
  // 2) UsbfnDwc3Dxe'nin yayinladigi protokolu bul.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_LOCATE;
  Status = gBS->LocateProtocol (
                  (EFI_GUID *)(VOID *)&gEfiUsbFunctionIoProtocolGuid,
                  NULL,
                  (VOID **)&mFn
                  );
  mDiagProtocol.Diag.LocateStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus  = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status) || (mFn == NULL)) {
    return Status;
  }

  //
  // 3) PHY + DWC3 core bring-up.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_START;
  Status = mFn->StartController (mFn);
  mDiagProtocol.Diag.StartStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 4) EP0 DMA tamponu.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_BUFFER;
  mEp0Buffer = NULL;
  Status = mFn->AllocateTransferBuffer (mFn, B2L_USB_EP0_BUF_SIZE, &mEp0Buffer);
  mDiagProtocol.Diag.BufferStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus  = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status) || (mEp0Buffer == NULL)) {
    return Status;
  }

  //
  // 5) Uc noktalari kur ve portu aktif et (DCTL.RUN_STOP = 1).
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_CONFIG;
  Status = mFn->ConfigureEnableEndpoints (mFn, &mDeviceInfo);
  mDiagProtocol.Diag.ConfigStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus  = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 6) Olay pompalama zamanlayicisi.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_TIMER;
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  B2LUsbPump,
                  NULL,
                  &mTimer
                  );
  mDiagProtocol.Diag.TimerStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mTimer, TimerPeriodic, B2L_USB_PUMP_PERIOD);
  mDiagProtocol.Diag.TimerStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDiagProtocol.Diag.Stage       = B2L_USB_STAGE_DONE;
  mDiagProtocol.Diag.EntryStatus = 0;

  DEBUG ((DEBUG_INFO, "B2LUsbSerial: device-mode up, endpoints configured\n"));

  return EFI_SUCCESS;
}
