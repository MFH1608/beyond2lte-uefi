/** @file
  Exynos 9820 (beyond0lte) DWC3 device-mode CDC-ACM sinif surucusu - M2b.

  Amac: PC'de /dev/ttyACM* (Linux cdc_acm) veya usbser (Windows) olarak
  gorunen bir seri aygit saglamak. Iki arayuzlu standart CDC-ACM yerlesimi:

    IF0 (comm, 0x02/0x02/0x01) : header + call mgmt + ACM + union functional
                                 descriptor'lari + bildirim interrupt IN (0x82)
    IF1 (data, 0x0A)           : Bulk OUT (0x02) + Bulk IN (0x81)

  Bildirim interrupt uc noktasi ZORUNLU: Linux cdc_acm acm_probe() kontrol
  arayuzunde uc nokta yoksa -EINVAL doner (bkz. PORT_S10E.md 6.6).

  Sozlesme (bkz. UsbfnDwc3Dxe.c):
    - StartController: PHY + DWC3 core.
    - ConfigureEnableEndpoints: EP0 + arayuz uc noktalarini kurar ve
      DCTL.RUN_STOP ile portu aktif eder. UsbfnDwc3Dxe YALNIZCA
      InterfaceInfoTable[0]'i gezer; bu yuzden comm (interrupt bildirim) ve
      data (bulk) uc noktalarinin HEPSI tek bir "donanim konfig" arayuzu
      altinda toplanir (bkz. mHwEndpointTable).
    - EventHandler tekrar tekrar cagrilir; Setup paketleri burada gelir.
    - Surucu SET_ADDRESS'i kendisi yapar; SET_CONFIGURATION/SET_INTERFACE'i
      ise once tum non-control uc noktalari sifirlayip bize yollar. Standart
      isteklerin geri kalani (GET_DESCRIPTOR, GET_STATUS, ...) ve sinif
      istekleri (SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE, SEND_BREAK)
      BIZE gelir.

  Bulk veri yolu: SET_CONFIGURATION sonrasi Bulk OUT'ta okuma kurulur; gelen
  veri aynen Bulk IN'den geri yazilir (echo). Teshis ekraninda rx/tx/lc
  sayaclari gorunur.

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
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/DevicePath.h>

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

//
// ================= CDC-ACM (M2b) =================
//
// PC tarafinda Linux cdc_acm / Windows usbser'in tanimasi icin iki arayuzlu
// standart CDC-ACM yerlesimi. Bildirim interrupt uc noktasi ZORUNLU: Linux
// acm_probe() kontrol arayuzunde uc nokta yoksa -EINVAL ile reddeder.
//
#define B2L_USB_CDC_IF_COMM      0     // iletisim (kontrol) arayuzu
#define B2L_USB_CDC_IF_DATA      1     // veri arayuzu
#define B2L_USB_CDC_SUBCLASS_ACM 0x02
#define B2L_USB_CDC_PROTO_AT     0x01

#define B2L_USB_CLASS_COMM       0x02  // bInterfaceClass: Communications
#define B2L_USB_CLASS_CDC_DATA   0x0A  // bInterfaceClass: CDC-Data

#define B2L_USB_CDC_HEADER       0x00  // functional: header
#define B2L_USB_CDC_CALL_MGMT    0x01  // functional: call management
#define B2L_USB_CDC_ACM          0x02  // functional: ACM
#define B2L_USB_CDC_UNION        0x06  // functional: union

//
// CDC sinif istekleri (bRequest). GET/SET_LINE_CODING 7 bayt; digerleri veri
// asamasi olmadan gelir.
//
#define B2L_USB_CDC_SET_LINE_CODING         0x20
#define B2L_USB_CDC_GET_LINE_CODING         0x21
#define B2L_USB_CDC_SET_CONTROL_LINE_STATE  0x22
#define B2L_USB_CDC_SEND_BREAK              0x23

//
// Mantiksal uc nokta numaralari (bEndpointAddress & 0x0F). EFI_USBFN_IO_PROTOCOL
// Transfer() EndpointIndex olarak bunlari alir; surucu Phys = (index<<1)|dir
// yapar. 0x82 (bildirim IN) ile 0x02 (bulk OUT) ayni numarayi paylasir ama
// yonleri farkli oldugu icin farkli fiziksel uc noktalara duser.
//
#define B2L_USB_EP_DATA_IN       1     // 0x81 bulk IN
#define B2L_USB_EP_DATA_OUT      2     // 0x02 bulk OUT

//
// Bulk (veri) tampon boyutu. Surucu RX'te TRB uzunlugunu MaxPacketSize'a
// yuvarlar (HS'te 512); tamponu ona gore buyuk tutuyoruz.
//
#define B2L_USB_BULK_BUF_SIZE    1024

//
// CDC satir kodlamasi (7 bayt). Varsayilan 115200 8N1.
//
#pragma pack(1)
typedef struct {
  UINT32  DteRate;
  UINT8   CharFormat;
  UINT8   ParityType;
  UINT8   DataBits;
} B2L_USB_LINE_CODING;
#pragma pack()

//
// ================= M3: UEFI konsol koprusu =================
//
// B2LUsbSerialDxe burada UC protokol daha yayinlar: Simple Text In, Simple
// Text Out ve bir DevicePath. B2LPlatformBootManager bu DevicePath'i
// ConIn/ConOut/ErrOut degiskenlerine ekler; ConPlatform + ConSplitter onu
// toplu (agregat) konsola katar. Boylece PC klavyesi gST->ConIn'e, gST->ConOut
// ise CDC-ACM Bulk IN'e gider. Rota B2LUiPrint gibi GOP tabanli degil, gercek
// UEFI konsolu oldugu icin self-test ekrani bundan etkilenmez.
//
#define B2L_CONSOLE_KEY_QUEUE   64     // kuyruktaki EFI_INPUT_KEY sayisi
#define B2L_CONSOLE_TX_SIZE     4096   // USB'ye gidecek bayt halkasi (ring)
#define B2L_CONSOLE_MODE_COLS   200    // agregati kucultmemek icin buyuk mod
#define B2L_CONSOLE_MODE_ROWS   60

//
// Konsol cihaz yolu. Ayni GUID B2LPlatformBootManager.c icinde de sabittir;
// iki taraf AYNI kalmak zorundadir (ConPlatform path'i boyle eslestirir).
//
#define B2L_USB_SERIAL_CONSOLE_GUID \
  { 0x6d2b8f41, 0x3c7a, 0x4e19, { 0x8a, 0xd5, 0x2b, 0x90, 0x4c, 0x6f, 0x13, 0xe7 } }

typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} B2L_USB_SERIAL_DEVICE_PATH;

STATIC EFI_STATUS EFIAPI B2LTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  );

STATIC EFI_STATUS EFIAPI B2LReadKeyStroke (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                  *Key
  );

STATIC EFI_STATUS EFIAPI B2LTextOutReset (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  );

STATIC EFI_STATUS EFIAPI B2LOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  );

STATIC EFI_STATUS EFIAPI B2LTestString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  );

STATIC EFI_STATUS EFIAPI B2LQueryMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber,
  OUT UINTN                           *Columns,
  OUT UINTN                           *Rows
  );

STATIC EFI_STATUS EFIAPI B2LSetMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber
  );

STATIC EFI_STATUS EFIAPI B2LSetAttribute (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Attribute
  );

STATIC EFI_STATUS EFIAPI B2LClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  );

STATIC EFI_STATUS EFIAPI B2LSetCursorPosition (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Column,
  IN UINTN                            Row
  );

STATIC EFI_STATUS EFIAPI B2LEnableCursor (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          Enable
  );

STATIC EFI_SIMPLE_TEXT_OUTPUT_MODE  mConsoleOutMode = {
  1, 0, 0, 0, 0, TRUE
};

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mConsoleIn = {
  B2LTextInReset,
  B2LReadKeyStroke,
  NULL
};

STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mConsoleOut = {
  B2LTextOutReset,
  B2LOutputString,
  B2LTestString,
  B2LQueryMode,
  B2LSetMode,
  B2LSetAttribute,
  B2LClearScreen,
  B2LSetCursorPosition,
  B2LEnableCursor,
  &mConsoleOutMode
};

//
// Konsol cihaz yolu. mConsoleHandle bu protokollerle yayinlanir; boot manager
// bu DevicePath'i ConIn/ConOut degiskenlerine ekler.
//
STATIC CONST B2L_USB_SERIAL_DEVICE_PATH  mConsoleDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) }
    },
    B2L_USB_SERIAL_CONSOLE_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL)), (UINT8)((sizeof (EFI_DEVICE_PATH_PROTOCOL)) >> 8) }
  }
};

STATIC EFI_HANDLE  mConsoleHandle = NULL;
STATIC EFI_EVENT   mKeyEvent      = NULL;

//
// Tus kuyrugu. Pump gelen baytlari buraya tus olarak yazar; ReadKeyStroke
// buradan okur.
//
STATIC EFI_INPUT_KEY  mKeyQueue[B2L_CONSOLE_KEY_QUEUE];
STATIC UINT32         mKeyHead  = 0;
STATIC UINT32         mKeyTail  = 0;
STATIC UINT32         mKeyCount = 0;

//
// Giden (TX) bayt halkasi. Konsol cikisi + giris yankisi buradan Bulk IN'e
// akitilir. Ayni anda yalnizca BIR Transfer ucabilir (mTxBusy).
//
STATIC UINT8    mTxRing[B2L_CONSOLE_TX_SIZE];
STATIC UINT32   mTxHead  = 0;
STATIC UINT32   mTxTail  = 0;
STATIC UINT32   mTxCount = 0;
STATIC BOOLEAN  mTxBusy  = FALSE;

//
// ESC dizisi cozumleme durumu: 0 = yok, 1 = ESC, 2 = ESC[, 3 = ESC O.
//
STATIC UINT8  mEscState = 0;

STATIC EFI_USBFN_IO_PROTOCOL         *mFn;
STATIC B2L_USB_SERIAL_DIAG_PROTOCOL   mDiagProtocol;
STATIC EFI_GUID                       mDiagGuid = B2L_USB_SERIAL_DIAG_PROTOCOL_GUID;
STATIC EFI_EVENT                      mTimer;
STATIC VOID                          *mEp0Buffer;
STATIC VOID                          *mBulkOutBuffer;
STATIC VOID                          *mBulkInBuffer;
STATIC BOOLEAN                        mConfigured;
STATIC B2L_USB_LINE_CODING            mLineCoding = { 115200, 0, 0, 8 };

STATIC CONST CHAR16  mManStr[]    = L"Mu-Silicium";
STATIC CONST CHAR16  mProdStr[]   = L"Samsung Exynos Gadget";
STATIC CONST CHAR16  mSerialStr[] = L"0123456789ABCDEF";

//
// Cihaz tanimlayici. Sinif bilgisi arayuzlerde verilir; Linux cdc_acm arayuz
// sinifina gore baglanir. DeviceClass = 0x02 (Communications).
//
STATIC CONST USB_DEVICE_DESCRIPTOR  mDeviceDescriptor = {
  18,                         // Length
  USB_DESC_TYPE_DEVICE,       // DescriptorType
  0x0200,                     // BcdUSB
  B2L_USB_CLASS_COMM,         // DeviceClass (Communications)
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
// CDC functional descriptor'lari (CS_INTERFACE, 0x24).
//
#pragma pack(1)
typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   DescriptorSubType;
  UINT16  BcdCDC;
} B2L_USB_CDC_HEADER_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   DescriptorSubType;
  UINT8   Capabilities;
  UINT8   DataInterface;
} B2L_USB_CDC_CALL_MGMT_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   DescriptorSubType;
  UINT8   Capabilities;
} B2L_USB_CDC_ACM_DESC;

typedef struct {
  UINT8   Length;
  UINT8   DescriptorType;
  UINT8   DescriptorSubType;
  UINT8   MasterInterface;
  UINT8   SlaveInterface;
} B2L_USB_CDC_UNION_DESC;

//
// Konfigurasyon blogu: 1 konfigurasyon + 2 arayuz. GET_DESCRIPTOR(CONFIG) bu
// blogu dondurur; ayni zamanda donanim kurulumu icin descriptor kaynagidir.
//
//   comm (IF0): header, call mgmt, ACM, union + bildirim interrupt IN (0x82)
//   data (IF1): bulk OUT (0x02) + bulk IN (0x81)
//
typedef struct {
  USB_CONFIG_DESCRIPTOR       Config;
  USB_INTERFACE_DESCRIPTOR    CommInterface;
  B2L_USB_CDC_HEADER_DESC     Header;
  B2L_USB_CDC_CALL_MGMT_DESC  CallMgmt;
  B2L_USB_CDC_ACM_DESC        Acm;
  B2L_USB_CDC_UNION_DESC      Union;
  USB_ENDPOINT_DESCRIPTOR     NotifyIn;
  USB_INTERFACE_DESCRIPTOR    DataInterface;
  USB_ENDPOINT_DESCRIPTOR     BulkOut;
  USB_ENDPOINT_DESCRIPTOR     BulkIn;
} B2L_USB_CONFIG_BLOB;
#pragma pack()

STATIC CONST B2L_USB_CONFIG_BLOB  mConfigBlob = {
  { 9, USB_DESC_TYPE_CONFIG, sizeof (B2L_USB_CONFIG_BLOB), 2, 1, 0, 0x80, 0x32 },
  { 9, USB_DESC_TYPE_INTERFACE, B2L_USB_CDC_IF_COMM, 0, 1,
    B2L_USB_CLASS_COMM, B2L_USB_CDC_SUBCLASS_ACM, B2L_USB_CDC_PROTO_AT, 0 },
  { 5, USB_DESC_TYPE_CS_INTERFACE, B2L_USB_CDC_HEADER, 0x0110 },
  { 5, USB_DESC_TYPE_CS_INTERFACE, B2L_USB_CDC_CALL_MGMT, 0x00, B2L_USB_CDC_IF_DATA },
  { 4, USB_DESC_TYPE_CS_INTERFACE, B2L_USB_CDC_ACM, 0x02 },
  { 5, USB_DESC_TYPE_CS_INTERFACE, B2L_USB_CDC_UNION, B2L_USB_CDC_IF_COMM, B2L_USB_CDC_IF_DATA },
  { 7, USB_DESC_TYPE_ENDPOINT, 0x82, USB_ENDPOINT_INTERRUPT, 64, 10 },
  { 9, USB_DESC_TYPE_INTERFACE, B2L_USB_CDC_IF_DATA, 0, 2,
    B2L_USB_CLASS_CDC_DATA, 0x00, 0x00, 0 },
  { 7, USB_DESC_TYPE_ENDPOINT, 0x02, USB_ENDPOINT_BULK, 64, 0 },
  { 7, USB_DESC_TYPE_ENDPOINT, 0x81, USB_ENDPOINT_BULK, 64, 0 }
};

//
// Donanim konfig yerlesimi. UsbfnDwc3Dxe YALNIZCA InterfaceInfoTable[0]'i
// gezdigi icin comm (bildirim interrupt) + data (bulk) uc noktalarinin HEPSI
// burada toplanir. mHwInterfaceDesc sentetiktir; surucu ondan yalnizca
// NumEndpoints alanini okur, host'a gonderilen descriptor'lar mConfigBlob'da.
//
STATIC USB_ENDPOINT_DESCRIPTOR  *mHwEndpointTable[3] = {
  (USB_ENDPOINT_DESCRIPTOR *)&mConfigBlob.NotifyIn,
  (USB_ENDPOINT_DESCRIPTOR *)&mConfigBlob.BulkOut,
  (USB_ENDPOINT_DESCRIPTOR *)&mConfigBlob.BulkIn
};
STATIC USB_INTERFACE_DESCRIPTOR  mHwInterfaceDesc = {
  9, USB_DESC_TYPE_INTERFACE, B2L_USB_CDC_IF_COMM, 0, 3,
  B2L_USB_CLASS_COMM, B2L_USB_CDC_SUBCLASS_ACM, B2L_USB_CDC_PROTO_AT, 0
};
STATIC EFI_USB_INTERFACE_INFO    mInterfaceInfo      = {
  (USB_INTERFACE_DESCRIPTOR *)&mHwInterfaceDesc,
  mHwEndpointTable
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
  EP0'dan veri al (data-out asamasi, ornegin SET_LINE_CODING). Surucu kendi
  bounce tamponundan Buffer'a kopyalar; cagiran dogrudan hedef tamponu verir.
**/
STATIC
VOID
B2LUsbReceiveEp0 (
  IN VOID   *Buffer,
  IN UINTN   Length
  )
{
  UINTN  Size = Length;

  if (mFn->Transfer (
            mFn,
            0,
            EfiUsbEndpointDirectionDeviceRx,
            &Size,
            Buffer
            ) != EFI_SUCCESS)
  {
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
  Bulk OUT'ta okuma kur. Gelen veri B2LUsbHandleBulkOut'ta yankilanir. Zaten
  bir okuma varsa surucu EFI_NOT_READY doner; bu normaldir, yok sayilir.
**/
STATIC
VOID
B2LUsbStartBulkRead (
  VOID
  )
{
  UINTN  Size = B2L_USB_BULK_BUF_SIZE;

  if ((mFn == NULL) || (mBulkOutBuffer == NULL)) {
    return;
  }

  mFn->Transfer (
         mFn,
         B2L_USB_EP_DATA_OUT,
         EfiUsbEndpointDirectionDeviceRx,
         &Size,
         mBulkOutBuffer
         );
}

//
// ================= Konsol yardimcilari (M3) =================
//

/**
  Bir tusu kuyruga yaz. Kuyruk doluysa en eski tus duser (klavye tamponu gibi);
  konsol girisinde bloklama yoktur.
**/
STATIC
VOID
B2LKeyPush (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  if (mKeyCount >= B2L_CONSOLE_KEY_QUEUE) {
    mKeyHead = (mKeyHead + 1) % B2L_CONSOLE_KEY_QUEUE;
    mKeyCount--;
  }

  mKeyQueue[mKeyTail].ScanCode    = ScanCode;
  mKeyQueue[mKeyTail].UnicodeChar = UnicodeChar;
  mKeyTail                        = (mKeyTail + 1) % B2L_CONSOLE_KEY_QUEUE;
  mKeyCount++;
}

/**
  WaitForKey bildirimi. SimpleTextInOutSerial ile ayni desen: EVT_NOTIFY_WAIT
  olayini yalnizca kuyrukta tus varsa isaretle. ConSplitter kendi toplu
  WaitForKey'inde her cihazin bu olayini CheckEvent ile yoklar.
**/
STATIC
VOID
EFIAPI
B2LKeyNotify (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  if (mKeyCount != 0) {
    gBS->SignalEvent (Event);
  }
}

/**
  Gelen konsol baytini tusa cevir. ESC tabanli oklari (VT100/PC ANSI) ve
  DEL'i destekler; yeterli kadari self-test ve shell icin.
**/
STATIC
VOID
B2LConsoleFeedByte (
  IN UINT8  Byte
  )
{
  switch (mEscState) {
    case 0:
      if (Byte == 0x1B) {
        mEscState = 1;
        return;
      }

      if ((Byte == CHAR_BACKSPACE) || (Byte == CHAR_TAB) ||
          (Byte == CHAR_LINEFEED)  || (Byte == CHAR_CARRIAGE_RETURN))
      {
        B2LKeyPush (SCAN_NULL, (CHAR16)Byte);
        return;
      }

      if (Byte == 0x7F) {
        B2LKeyPush (SCAN_DELETE, 0);
        return;
      }

      B2LKeyPush (SCAN_NULL, (CHAR16)Byte);
      return;

    case 1:
      if (Byte == '[') {
        mEscState = 2;
        return;
      }

      if (Byte == 'O') {
        mEscState = 3;
        return;
      }

      B2LKeyPush (SCAN_ESC, 0);
      mEscState = 0;
      return;

    case 2:
      mEscState = 0;
      switch (Byte) {
        case 'A': B2LKeyPush (SCAN_UP, 0);    break;
        case 'B': B2LKeyPush (SCAN_DOWN, 0);  break;
        case 'C': B2LKeyPush (SCAN_RIGHT, 0); break;
        case 'D': B2LKeyPush (SCAN_LEFT, 0);  break;
        case 'H': B2LKeyPush (SCAN_HOME, 0);  break;
        case 'F': B2LKeyPush (SCAN_END, 0);   break;
        default: break;
      }

      return;

    case 3:
      mEscState = 0;
      switch (Byte) {
        case 'P': B2LKeyPush (SCAN_F1, 0); break;
        case 'Q': B2LKeyPush (SCAN_F2, 0); break;
        case 'R': B2LKeyPush (SCAN_F3, 0); break;
        case 'S': B2LKeyPush (SCAN_F4, 0); break;
        default: break;
      }

      return;

    default:
      mEscState = 0;
      return;
  }
}

/**
  TX halkasindan bir parca gonder. Ayni anda yalnizca bir Transfer ucabilir;
  bitisinde EfiUsbMsgEndpointStatusChangedTx mTxBusy'yi indirip tekrar cagirir.
**/
STATIC
VOID
B2LUsbTxPump (
  VOID
  )
{
  UINTN  Index;
  UINTN  Count;
  UINTN  Size;
  UINT8  *Dst;

  if (mTxBusy || (mTxCount == 0) || !mConfigured) {
    return;
  }

  if ((mFn == NULL) || (mBulkInBuffer == NULL)) {
    return;
  }

  Count = mTxCount;
  if (Count > B2L_USB_BULK_BUF_SIZE) {
    Count = B2L_USB_BULK_BUF_SIZE;
  }

  Dst = (UINT8 *)mBulkInBuffer;
  for (Index = 0; Index < Count; Index++) {
    Dst[Index] = mTxRing[(mTxHead + Index) % B2L_CONSOLE_TX_SIZE];
  }

  Size = Count;
  if (mFn->Transfer (
            mFn,
            B2L_USB_EP_DATA_IN,
            EfiUsbEndpointDirectionDeviceTx,
            &Size,
            mBulkInBuffer
            ) == EFI_SUCCESS)
  {
    mTxHead                    = (mTxHead + (UINT32)Count) % B2L_CONSOLE_TX_SIZE;
    mTxCount                  -= (UINT32)Count;
    mTxBusy                    = TRUE;
    mDiagProtocol.Diag.TxBytes += (UINT32)Count;
  }
}

/**
  Baytlari TX halkasina ekle ve gonderimi tetikle. Halka dolarsa sessizce
  damla: konsol cikisi asla bloklanmaz.
**/
STATIC
VOID
B2LUsbTxPush (
  IN CONST UINT8  *Data,
  IN UINTN         Length
  )
{
  UINTN  Index;

  for (Index = 0; Index < Length; Index++) {
    if (mTxCount >= B2L_CONSOLE_TX_SIZE) {
      break;
    }

    mTxRing[mTxTail] = Data[Index];
    mTxTail          = (mTxTail + 1) % B2L_CONSOLE_TX_SIZE;
    mTxCount++;
  }

  B2LUsbTxPump ();
}

//
// ================= Simple Text In / Out (M3) =================
//

STATIC
EFI_STATUS
EFIAPI
B2LTextInReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mKeyHead  = 0;
  mKeyTail  = 0;
  mKeyCount = 0;
  mEscState = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LReadKeyStroke (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mKeyCount == 0) {
    return EFI_NOT_READY;
  }

  Key->ScanCode    = mKeyQueue[mKeyHead].ScanCode;
  Key->UnicodeChar = mKeyQueue[mKeyHead].UnicodeChar;
  mKeyHead         = (mKeyHead + 1) % B2L_CONSOLE_KEY_QUEUE;
  mKeyCount--;

  mDiagProtocol.Diag.KeyReads++;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LTextOutReset (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  )
{
  This->SetAttribute (
          This,
          EFI_TEXT_ATTR (This->Mode->Attribute & 0x0F, EFI_BACKGROUND_BLACK)
          );
  return B2LSetMode (This, 0);
}

STATIC
EFI_STATUS
EFIAPI
B2LOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  UINT8   Buffer[64];
  UINTN   Count;
  UINTN   Total;
  CHAR16  Ch;

  if (String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Count = 0;
  Total = 0;
  while ((Ch = *String++) != CHAR_NULL) {
    Buffer[Count++] = (Ch < 0x80) ? (UINT8)Ch : (UINT8)'?';
    Total++;

    if (Count == sizeof (Buffer)) {
      B2LUsbTxPush (Buffer, Count);
      Count = 0;
    }
  }

  if (Count != 0) {
    B2LUsbTxPush (Buffer, Count);
  }

  mDiagProtocol.Diag.ConsoleOut += (UINT32)Total;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LTestString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  //
  // ASCII disi karakterler '?' olarak gonderilir (bkz. B2LOutputString), yani
  // her dize desteklenir.
  //
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LQueryMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber,
  OUT UINTN                           *Columns,
  OUT UINTN                           *Rows
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  *Columns = B2L_CONSOLE_MODE_COLS;
  *Rows    = B2L_CONSOLE_MODE_ROWS;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LSetMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  This->Mode->Mode = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LSetAttribute (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Attribute
  )
{
  This->Mode->Attribute = (INT32)Attribute;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  //
  // ANSI: ekrani temizle + imleci sol ust koseye al. Coklu cihazin toplandigi
  // ConSplitter'da kabul edilen minimal davranis.
  //
  STATIC CONST UINT8  Clear[] = { 0x1B, '[', '2', 'J', 0x1B, '[', 'H' };

  B2LUsbTxPush (Clear, sizeof (Clear));
  This->Mode->CursorColumn = 0;
  This->Mode->CursorRow    = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LSetCursorPosition (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Column,
  IN UINTN                            Row
  )
{
  This->Mode->CursorColumn = (INT32)Column;
  This->Mode->CursorRow    = (INT32)Row;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
B2LEnableCursor (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          Enable
  )
{
  return EFI_SUCCESS;
}

/**
  Bulk OUT tamamlandi. Gelen baytlar (1) tus kuyruguna cozulur, (2) M2b
  uyumu icin aynen geri yankilanir. Yanki da UEFI konsol cikisi gibi TX
  halkasindan gider. Okuma hemen yeniden kurulur (TX ayri uc).
**/
STATIC
VOID
B2LUsbHandleBulkOut (
  IN EFI_USBFN_TRANSFER_RESULT  *Result
  )
{
  UINTN  Length;
  UINTN  Index;
  UINT8  *Bytes;

  Length = Result->BytesTransferred;
  if (Length > B2L_USB_BULK_BUF_SIZE) {
    Length = B2L_USB_BULK_BUF_SIZE;
  }

  if ((Result->TransferStatus != UsbTransferStatusComplete) || (Length == 0)) {
    //
    // Iptal veya sifir uzunluk (SET_CONFIG/SET_INTERFACE sifirlamasi olabilir):
    // yankilamadan yeniden oku.
    //
    if (mConfigured) {
      B2LUsbStartBulkRead ();
    }
    return;
  }

  mDiagProtocol.Diag.RxBytes += (UINT32)Length;

  Bytes = (UINT8 *)mBulkOutBuffer;
  for (Index = 0; Index < Length; Index++) {
    B2LConsoleFeedByte (Bytes[Index]);
  }

  B2LUsbTxPush (Bytes, Length);

  if (mConfigured) {
    B2LUsbStartBulkRead ();
  }
}

/**
  CDC sinif istekleri (RequestType alani CLASS). SET/GET_LINE_CODING 7 bayt;
  SET_CONTROL_LINE_STATE ve SEND_BREAK veri asamasi olmadan gelir.
**/
STATIC
VOID
B2LUsbHandleClassRequest (
  IN EFI_USB_DEVICE_REQUEST  *Request
  )
{
  switch (Request->Request) {
    case B2L_USB_CDC_SET_LINE_CODING:
      B2LUsbReceiveEp0 (&mLineCoding, sizeof (mLineCoding));
      mDiagProtocol.Diag.LineCodings++;
      return;

    case B2L_USB_CDC_GET_LINE_CODING:
      B2LUsbSendEp0 (&mLineCoding, sizeof (mLineCoding));
      return;

    case B2L_USB_CDC_SET_CONTROL_LINE_STATE:
    case B2L_USB_CDC_SEND_BREAK:
      //
      // Veri asamasi yok; durum asamasi donanim tarafindan tamamlanir.
      //
      return;

    default:
      B2LUsbStallEp0 (Request);
      return;
  }
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

  if ((Request->RequestType & (0x03U << 5)) == USB_REQ_TYPE_CLASS) {
    B2LUsbHandleClassRequest (Request);
    return;
  }

  if ((Request->RequestType & (0x03U << 5)) != USB_REQ_TYPE_STANDARD) {
    //
    // Vendor/rezerve istekleri desteklemiyoruz.
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
      // Konfigurasyon secildi: bulk OUT'ta okumayi baslat, bekleyen cikisi akit.
      //
      if (mConfigured) {
        B2LUsbStartBulkRead ();
        B2LUsbTxPump ();
      }
      return;

    case USB_REQ_SET_INTERFACE:
      //
      // Host data arayuzunu (alt ayar 0) secer. SET_CONFIGURATION/SET_INTERFACE
      // oncesi surucu non-control uc noktalari sifirlar; okumayi yeniden kur.
      //
      if (mConfigured) {
        B2LUsbStartBulkRead ();
        B2LUsbTxPump ();
      }
      return;

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
        if (Payload.utr.EndpointIndex == B2L_USB_EP_DATA_IN) {
          //
          // Gonderim bitti: TX halkasindan siradakini gonder.
          //
          mTxBusy = FALSE;
          B2LUsbTxPump ();
        }
        break;

      case EfiUsbMsgEndpointStatusChangedRx:
        mDiagProtocol.Diag.RxDone++;
        if (Payload.utr.EndpointIndex == B2L_USB_EP_DATA_OUT) {
          B2LUsbHandleBulkOut (&Payload.utr);
        }
        break;

      case EfiUsbMsgBusEventReset:
        mConfigured                    = FALSE;
        mDiagProtocol.Diag.Configured  = 0;
        mDiagProtocol.Diag.Addr        = 0;
        mDiagProtocol.Diag.BusEvents++;
        //
        // Reset: ucaktaki gonderim iptal olur, TX halkasini bosalt.
        //
        mTxBusy = FALSE;
        mTxHead = 0;
        mTxTail = 0;
        mTxCount = 0;
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
  // 2) Konsol protokollerini yayinla (M3). Bu, bring-up'tan ONCE olmali:
  //    PC kablosu takili olmasa da driver resident kalir ve boot manager
  //    DevicePath'i ConIn/ConOut'a ekleyebilir. ReadKeyStroke kuyruk bosken
  //    EFI_NOT_READY doner.
  //
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  B2LKeyNotify,
                  NULL,
                  &mKeyEvent
                  );
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mConsoleIn.WaitForKey = mKeyEvent;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mConsoleHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  &mConsoleIn,
                  &gEfiSimpleTextOutProtocolGuid,
                  &mConsoleOut,
                  &gEfiDevicePathProtocolGuid,
                  (VOID *)&mConsoleDevicePath,
                  NULL
                  );
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mDiagProtocol.Diag.UsbConIn  = (UINT64)(UINTN)&mConsoleIn;
  mDiagProtocol.Diag.UsbConOut = (UINT64)(UINTN)&mConsoleOut;

  //
  // 3) UsbfnDwc3Dxe'nin yayinladigi protokolu bul. Bundan sonraki adimlar
  //    basarisiz olsa bile EFI_SUCCESS doneriz: konsol protokolleri yayinda
  //    kalmali ve teshis ekrani hangi asamada durdugunu gostermeli.
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
    return EFI_SUCCESS;
  }

  //
  // 4) PHY + DWC3 core bring-up.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_START;
  Status = mFn->StartController (mFn);
  mDiagProtocol.Diag.StartStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  //
  // 5) EP0 + CDC-ACM bulk (veri) DMA tamponlari.
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_BUFFER;
  mEp0Buffer = NULL;
  Status = mFn->AllocateTransferBuffer (mFn, B2L_USB_EP0_BUF_SIZE, &mEp0Buffer);
  mDiagProtocol.Diag.BufferStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus  = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status) || (mEp0Buffer == NULL)) {
    return EFI_SUCCESS;
  }

  mBulkOutBuffer = NULL;
  mBulkInBuffer  = NULL;
  if (EFI_ERROR (mFn->AllocateTransferBuffer (mFn, B2L_USB_BULK_BUF_SIZE, &mBulkOutBuffer)) ||
      EFI_ERROR (mFn->AllocateTransferBuffer (mFn, B2L_USB_BULK_BUF_SIZE, &mBulkInBuffer)) ||
      (mBulkOutBuffer == NULL) || (mBulkInBuffer == NULL))
  {
    mDiagProtocol.Diag.BufferStatus = (UINT64)(UINTN)EFI_OUT_OF_RESOURCES;
    return EFI_SUCCESS;
  }

  //
  // 6) Uc noktalari kur ve portu aktif et (DCTL.RUN_STOP = 1).
  //
  mDiagProtocol.Diag.Stage = B2L_USB_STAGE_CONFIG;
  Status = mFn->ConfigureEnableEndpoints (mFn, &mDeviceInfo);
  mDiagProtocol.Diag.ConfigStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus  = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  //
  // 7) Olay pompalama zamanlayicisi.
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
    return EFI_SUCCESS;
  }

  Status = gBS->SetTimer (mTimer, TimerPeriodic, B2L_USB_PUMP_PERIOD);
  mDiagProtocol.Diag.TimerStatus = (UINT64)(UINTN)Status;
  mDiagProtocol.Diag.EntryStatus = (UINT64)(UINTN)Status;
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  mDiagProtocol.Diag.Stage       = B2L_USB_STAGE_DONE;
  mDiagProtocol.Diag.EntryStatus = 0;

  DEBUG ((DEBUG_INFO, "B2LUsbSerial: device-mode up, endpoints configured\n"));

  return EFI_SUCCESS;
}
