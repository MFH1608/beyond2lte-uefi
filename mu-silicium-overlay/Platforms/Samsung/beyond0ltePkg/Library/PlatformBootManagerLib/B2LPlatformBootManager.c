/** @file
  B2L platform boot manager.

  Normalde "isletim sistemi bulunamadi" durumunda PlatformBootManagerUnableToBoot
  ekrana logo basip 10 sn sonra cihazi kapatir. Bu dosya o davranisi, S10e
  donanim self-test ekraniyla degistirir.

  Neden: cihazda harf ureten bir klavye yok (keypad yalnizca Up/Down/Enter/Esc
  veriyor) ve FAT/ESP bolumu bulunmadigi icin UEFI Shell'e komut yazilamiyor.
  Bu ekran dogrulama bilgilerini dogrudan framebuffer'a basar; kullanici
  yalnizca ekrani fotograflar.

  Cikis: Bixby'yi basili tutup Volume Up/Down'a basmak (Left/Right ScanCode'u
  uretir) -> ekrandan cikilir, ardindan normal "no OS" yolu cihazi kapatir.
**/

#include <Uefi.h>

#include <Library/DeviceBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationHelperLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiImage.h>
#include <Protocol/HiiFont.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/DevicePath.h>
#include <Protocol/UsbFunctionIo.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>

//
// S10e'de son DRAM banki 0x980000000'da bitiyor (0x80000000..0x100000000 +
// 0x880000000..0x980000000 = 6 GiB). Bu adresin ustunde, var olmayan DRAM'i
// tarif eden BIR descriptor OLMAMALI; bu satir MemoryMapLib'in dogru
// calistigini gosterir. Yuksek adresli MMIO (varsa) zararsiz sayilir.
//
#define B2L_MMIO_LIMIT  0x980000000ULL

//
// B2LTouchDxe'nin yayinladigi teshis protokolu (bkz.
// Drivers/B2LTouchDxe/B2LTouchDiag.h). Bu paketin .dec dosyasi build'e
// bagli olmadigi icin Include yolu eklenemiyor; GUID ve yerlesim burada
// yerel olarak tekrarlanir. Iki taraf AYNI kalmak zorundadir.
//
#define B2L_TOUCH_DIAG_PROTOCOL_GUID \
  { 0x9c1f4b52, 0x8d36, 0x4c01, { 0xa7, 0x62, 0x51, 0x0e, 0x9b, 0x3d, 0x77, 0xc4 } }

#define B2L_TOUCH_DIAG_EVENT_SIZE  8

#define B2L_TOUCH_DIAG_SIGNATURE  0x31474442U   // "BDG1"

//
// Surucu ile ayni asama numaralari (bkz. B2LTouchDiag.h). "done" asamasina
// ulastiysa entry/probe/bring-up zinciri tamamlanmis demektir.
//
#define B2L_TOUCH_STAGE_DONE      7

typedef struct {
  UINT32  Signature;
  UINT32  Size;
  UINT32  Stage;
  UINT32  PmicLocate;
  UINT64  EntryStatus;
  UINT64  AbsPointer;
  UINT64  LastStatus;
  UINT32  Polls;
  UINT32  Empty;
  UINT32  Errors;
  UINT32  StatusEvents;
  UINT32  CoordEvents;
  UINT32  Ignored;
  UINT32  LdoUv[2];
  UINT32  IntStatus;
  UINT32  TransStatus;
  UINT32  ScanAck[4];
  UINT8   DeviceId[5];
  UINT8   Integrity;
  UINT8   BootStatus;
  UINT8   ResetCalls;
  UINT8   ProbeCalls;
  UINT8   PowerOk;
  UINT8   ScanDone;
  UINT8   LdoSetOk[2];
  UINT8   LastEvent[B2L_TOUCH_DIAG_EVENT_SIZE];
  UINT8   FnBefore[2];
  UINT8   FnAfter[2];
  UINT8   PowerMode;
  UINT8   BringUpOk;
  UINT8   Reserved[5];
} B2L_TOUCH_DIAG;

typedef struct {
  B2L_TOUCH_DIAG  Diag;
} B2L_TOUCH_DIAG_PROTOCOL;

STATIC CONST EFI_GUID  mB2LTouchDiagGuid = B2L_TOUCH_DIAG_PROTOCOL_GUID;

//
// B2LUsbSerialDxe'nin yayinladigi teshis protokolu (bkz.
// Drivers/B2LUsbSerialDxe/B2LUsbSerialDiag.h). Ayni sekilde GUID ve yerlesim
// burada yerel olarak tekrarlanir; iki taraf AYNI kalmak zorundadir.
//
#define B2L_USB_SERIAL_DIAG_PROTOCOL_GUID \
  { 0x2f7c9a14, 0x5b6d, 0x4a83, { 0x91, 0x2e, 0x7c, 0x4f, 0xb1, 0x0a, 0x63, 0xd8 } }

#define B2L_USB_SERIAL_DIAG_SIGNATURE  0x31445542U   // "BUD1"

#define B2L_USB_STAGE_DONE             7

typedef struct {
  UINT32  Signature;
  UINT32  Size;
  UINT32  Stage;
  UINT32  Reserved0;
  UINT64  EntryStatus;
  UINT64  LocateStatus;
  UINT64  StartStatus;
  UINT64  BufferStatus;
  UINT64  ConfigStatus;
  UINT64  TimerStatus;
  UINT32  Pumps;
  UINT32  Setups;
  UINT32  Configured;
  UINT32  Addr;
  UINT32  Stalls;
  UINT32  TxDone;
  UINT32  RxDone;
  UINT32  BusEvents;
  UINT32  RxBytes;
  UINT32  TxBytes;
  UINT32  LineCodings;
  UINT32  LastBRequest;
  UINT32  LastBMRequestType;
  UINT32  LastValue;
  UINT32  Gctl;
  UINT32  Dctl;
  UINT32  Dsts;
  UINT64  UsbConIn;
  UINT64  UsbConOut;
  UINT32  KeyReads;
  UINT32  ConsoleOut;
} B2L_USB_SERIAL_DIAG;

typedef struct {
  B2L_USB_SERIAL_DIAG  Diag;
} B2L_USB_SERIAL_DIAG_PROTOCOL;

STATIC CONST EFI_GUID  mB2LUsbDiagGuid = B2L_USB_SERIAL_DIAG_PROTOCOL_GUID;

//
// M3: USB seri konsolunun cihaz yolu. B2LUsbSerialDxe AYNI GUID ile bir
// DevicePath yayinlar; DeviceBootManagerBeforeConsole onu ConIn/ConOut/ErrOut
// degiskenlerine ekler, ConPlatform + ConSplitter toplu konsola katar. GUID
// iki modulde AYNI kalmak zorundadir.
//
#define B2L_USB_SERIAL_CONSOLE_GUID \
  { 0x6d2b8f41, 0x3c7a, 0x4e19, { 0x8a, 0xd5, 0x2b, 0x90, 0x4c, 0x6f, 0x13, 0xe7 } }

//
// B2LTool'un FV dosya GUID'i. B2LTool.h icindeki B2L_TOOL_FILE_GUID ve
// B2LTool.inf icindeki FILE_GUID ile AYNI kalmak zorundadir (bkz.
// Applications/B2LTool).
//
#define B2L_TOOL_FILE_GUID \
  { 0xC7E1A2B3, 0x4D5F, 0x4061, { 0x8A, 0x92, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F, 0x80 } }

STATIC CONST EFI_GUID  mB2LToolGuid = B2L_TOOL_FILE_GUID;

STATIC CONST struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mB2LUsbConsolePath = {
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

//
// Self-test yazisini PC'ye (yalniz USB) basmak icin B2LUsbSerialDxe'nin
// Simple Text Out protokolu. UsbDiag->Diag.UsbConOut'tan alinir; 0 ise
// USB konsolu yok demektir.
//
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *mB2LUsbConOut = NULL;

//
// Konsolun yazi tipi 8x19 pikselde sabittir (EFI_GLYPH_WIDTH/HEIGHT
// degistirilemez), bu yuzden 1080x2280 ekranda fotograftan okumak zor.
// B2LGoster'daki gibi glifleri HII font protokolunden alip OLCEK kat
// buyuterek dogrudan framebuffer'a ciziyoruz. Boylece self-test ekrani
// buyuk ve net okunur.
//
#define B2L_UI_OLCEK    3                 // glif basina 3x3 piksel blogu
#define B2L_UI_GLIF_W   8                 // HII varsayilan glif genisligi
#define B2L_UI_GLIF_H   19                // HII varsayilan glif yuksekligi
#define B2L_UI_SATIR    (B2L_UI_GLIF_H * B2L_UI_OLCEK + 4)
#define B2L_UI_KENAR    8                 // sol kenar boslugu

#define B2L_UI_BEYAZ    0x00FFFFFF
#define B2L_UI_GRI      0x00A0A0A0
#define B2L_UI_YESIL    0x0040FF40
#define B2L_UI_KIRMIZI  0x00FF4040
#define B2L_UI_SARI     0x00FFFF40
#define B2L_UI_MAVI     0x0060C0FF

//
// ==================== USB getirme denemesi (S10e) ====================
//
// Amac: USB yigininin (DWC3/xHCI) bu cihazda ACILIP acilmadigini TEK
// flash'ta olcmek. Cihazda harf ureten klavye/seri konsol olmadigi icin
// degerler dogrudan self-test ekranina basilir.
//
// Bu blok SALT OKUMADIR - hicbir register YAZILMAZ. Sonuc, ileriki adimin
// (PHY/saat getirme) gerekli olup olmadigini gosterir: GSNPSID ve xHCI
// CAPLENGTH makul cikarsa blok S-Boot'tan canli devralinmis demektir;
// 0 ya da 0xffffffff cikarsa once saat/guc getirmek gerekir.
//
// ONEMLI: S-Boot, USB MMIO sayfalarini (USBCON/DWC3) UEFI memory map'ine
// KOYMAZ. Ilk deneme (v1) bu yuzden ilk okumada "Data abort / Translation
// fault, second level" ile coktu. Bu yuzden okumadan once her bolge
// MapMemoryRegion() ile EfiMemoryMappedIO olarak haritalanir; haritalama
// basarisizsa o bolge HIC OKUNMAZ (fault imkansiz).
//
// Adresler stok S10e DTB'sinden (Resources/DTBs/beyond0lte.dts) ve
// S5E7885Pkg / S5E9830Pkg PlatformUsbfnLib yerlesiminden alindi; ikisi de
// ayni USBCON register haritasini kullanir. DWC3 global offsetleri
// SamsungPkg/Include/Device/Dwc3.h ile aynidir.
//
#define B2L_USB_PMU_BASE        0x15860000ULL
#define B2L_USB_PMU_PHY         0x072C    // usbdrd phy control (DTB pmu_offset)
#define B2L_USB_PMU_PHY_DP      0x0704    // usbdp phy control (DTB pmu_offset_dp)
#define B2L_USB_USBCON_BASE     0x10B00000ULL   // phy@10B00000, reg[0] 0x200
#define B2L_USB_USBCON_SIZE     0x1000    // phy node reg uzunlugu (4 KiB)
#define B2L_USB_USBCON_VER      0x00      // CTRL_VER
#define B2L_USB_USBCON_CLKRST   0x20      // CLKRST
#define B2L_USB_DWC3_BASE       0x10C00000ULL   // usb@10C00000 (dwc3)
#define B2L_USB_DWC3_SIZE       0x10000   // usb node reg uzunlugu (64 KiB)
#define B2L_USB_DWC3_GSNPSID    0xC120    // Synopsys kimlik (0x5533xxxx / 0x3331xxxx)
#define B2L_USB_DWC3_GCTL       0xC110    // PRTCAPDIR burada
#define B2L_USB_DWC3_GUSB2PHYCFG 0xC200    // UTMI+ arayuz / turnaround
#define B2L_USB_DWC3_DCTL       0xC704    // Device Control (RUN_STOP vb.)
#define B2L_USB_DWC3_DSTS       0xC70C    // Device Status (hiz, halt)
#define B2L_USB_XHCI_CAPLEN     0x00      // xHCI CAPLENGTH(8) | HCIVERSION(16)
#define B2L_USB_XHCI_HCSPARAMS1 0x04      // xHCI HCSPARAMS1

typedef struct {
  UINT32  PmuPhy;         // PMU + 0x72C
  UINT32  PmuPhyDp;       // PMU + 0x704
  UINT32  UsbconVer;      // USBCON CTRL_VER
  UINT32  UsbconClkrst;   // USBCON CLKRST
  UINT32  SnpsId;         // DWC3 GSNPSID
  UINT32  Gctl;           // DWC3 GCTL
  UINT32  XhciCapLen;     // xHCI CAPLENGTH/HCIVERSION
  UINT32  XhciParams1;    // xHCI HCSPARAMS1
} B2L_USB_PROBE;

STATIC B2L_USB_PROBE  mUsb;
STATIC EFI_STATUS     mUsbConMap;
STATIC EFI_STATUS     mUsbDwc3Map;
STATIC BOOLEAN        mUsbValid;

//
// ================= Device-mode bring-up denemesi (M1) =================
//
// UsbfnDwc3Dxe (SamsungPkg) yalnizca EFI_USBFN_IO_PROTOCOL yayinlar; PHY ve
// controller ancak bir sinif surucusu StartController'i cagirinca acilir.
// Sinif surucusu (gadget) henuz yok (M2 = CDC-ACM). Bu blok, S5E9820Pkg
// PlatformUsbfnLib'in (USBCON 0x10B00000 + DWC3 0x10C00000) bring-up'ini
// dogrulamak icin StartController'i DOGRUDAN cagirir ve sonucu ekrana basar.
//
// Not: StartController sonrasi cihaz PC'ye baglanmaz; baglanma icin
// ConfigureEnableEndpoints (RUN_STOP) gerekir ki o M2'dedir. Burada amac
// PHY/core zincirinin calistigini gormek: st=Success ve GCTL/DCTL/DSTS
// makul olmali.
//
typedef struct {
  EFI_STATUS  LocateStatus;
  EFI_STATUS  StartStatus;
  UINT32      Gctl;
  UINT32      Dctl;
  UINT32      Dsts;
  BOOLEAN     Present;
} B2L_USB_DEV;

STATIC B2L_USB_DEV  mUsbDev;

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mUiGop;
STATIC EFI_HII_FONT_PROTOCOL         *mUiFont;
STATIC UINT32                        *mUiFb;      // framebuffer (32-bit piksel)
STATIC UINT32                         mUiStride;  // piksel/satir
STATIC UINT32                         mUiY;       // o anki cizim satiri
STATIC BOOLEAN                        mUiReady;

/**
  Tek glifi OLCEK kat buyutup framebuffer'a bas.
**/
STATIC
VOID
B2LUiGlyph (
  IN CHAR16  Ch,
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Color
  )
{
  EFI_IMAGE_OUTPUT  *Blt;
  UINT32             HRes;
  UINT32             VRes;
  UINT32             Px;
  UINT32             Py;
  UINT32             I;
  UINT32             J;

  Blt = NULL;
  if (EFI_ERROR (mUiFont->GetGlyph (mUiFont, Ch, NULL, &Blt, NULL)) || (Blt == NULL)) {
    return;
  }

  HRes = mUiGop->Mode->Info->HorizontalResolution;
  VRes = mUiGop->Mode->Info->VerticalResolution;

  for (Py = 0; Py < Blt->Height; Py++) {
    for (Px = 0; Px < Blt->Width; Px++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *P;

      P = &Blt->Image.Bitmap[Py * Blt->Width + Px];

      //
      // HII glifi on plani acik renk veriyor; sifirdan farkli her piksel
      // "harfin parcasi" sayilir.
      //
      if ((P->Red | P->Green | P->Blue) == 0) {
        continue;
      }

      for (J = 0; J < B2L_UI_OLCEK; J++) {
        for (I = 0; I < B2L_UI_OLCEK; I++) {
          UINT32  Fx;
          UINT32  Fy;

          Fx = X + Px * B2L_UI_OLCEK + I;
          Fy = Y + Py * B2L_UI_OLCEK + J;
          if ((Fx < HRes) && (Fy < VRes)) {
            mUiFb[Fy * mUiStride + Fx] = Color;
          }
        }
      }
    }
  }

  if (Blt->Image.Bitmap != NULL) {
    gBS->FreePool (Blt->Image.Bitmap);
  }
  gBS->FreePool (Blt);
}

/**
  Tek satir yaz ve asagi in.
**/
STATIC
VOID
B2LUiLine (
  IN CONST CHAR16  *Text,
  IN UINT32         Color
  )
{
  UINT32  X;

  X = B2L_UI_KENAR;
  while (*Text != L'\0') {
    B2LUiGlyph (*Text, X, mUiY, Color);
    X += B2L_UI_GLIF_W * B2L_UI_OLCEK;
    Text++;
  }

  mUiY += B2L_UI_SATIR;
}

/**
  Printf benzeri satir: metni bicimlendirip buyuk yaziyla cizer.
**/
STATIC
VOID
B2LUiPrint (
  IN UINT32         Color,
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR16   Buffer[160];

  VA_START (Marker, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  B2LUiLine (Buffer, Color);
}

/**
  M3: ayni satiri USB seri konsoluna da yaz. Ekrani etkilemez; mB2LUsbConOut
  yoksa (USB konsolu yayinda degilse) sessizce gecer.
**/
STATIC
VOID
B2LUsbPrint (
  IN CONST CHAR16  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR16   Buffer[200];

  if (mB2LUsbConOut == NULL) {
    return;
  }

  VA_START (Marker, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Marker);
  VA_END (Marker);

  mB2LUsbConOut->OutputString (mB2LUsbConOut, Buffer);
}

/**
  Y0 satirindan ekranin altina kadar siyahla temizle.
**/
STATIC
VOID
B2LUiClearFrom (
  IN UINT32  Y0
  )
{
  UINT32  VRes;

  VRes = mUiGop->Mode->Info->VerticalResolution;
  if (Y0 >= VRes) {
    return;
  }

  SetMem32 (
    &mUiFb[Y0 * mUiStride],
    (UINTN)(VRes - Y0) * mUiStride * sizeof (UINT32),
    0x00000000
    );
}

/**
  MapMemoryRegion zaten haritali bir bolge icin EFI_ALREADY_STARTED /
  EFI_ACCESS_DENIED doner. M2a'dan sonra bunu artik bekle: sinif
  surucusu (B2LUsbSerialDxe) DXE dispatch'te StartController'i cagirip
  bolgeleri bizden ONCE haritalar. Bu yuzden "hata" degil, "kullanilabilir"
  sayilir (bkz. PlatformUsbfnLib/BoardInit.c UsbMapRegions).
**/
STATIC
BOOLEAN
B2LMapOk (
  IN EFI_STATUS  Status
  )
{
  return (BOOLEAN)(!EFI_ERROR (Status) ||
                   (Status == EFI_ALREADY_STARTED) ||
                   (Status == EFI_ACCESS_DENIED));
}

/**
  USB bloklarini SALT OKUYARAK bir kez ornekle. Yazma yok; ekranin
  canli dongusunde degil, statik bolumde cagrilir.

  S-Boot USB MMIO sayfalarini memory map'e koymadigi icin once
  MapMemoryRegion() ile EfiMemoryMappedIO olarak haritalanir. Yalnizca
  haritalama basarili olan bolge okunur; aksi halde hic dokunulmaz.
  PMU bolgesi (0x15860000) S-Boot tarafindan zaten haritalanmistir
  (v1'de okunabildi), o yuzden dogrudan okunur.
**/
STATIC
VOID
B2LUsbRead (
  VOID
  )
{
  mUsbConMap  = MapMemoryRegion (
                  B2L_USB_USBCON_BASE,
                  B2L_USB_USBCON_SIZE,
                  EfiMemoryMappedIO
                  );
  mUsbDwc3Map = MapMemoryRegion (
                  B2L_USB_DWC3_BASE,
                  B2L_USB_DWC3_SIZE,
                  EfiMemoryMappedIO
                  );

  mUsb.PmuPhy   = MmioRead32 ((UINTN)(B2L_USB_PMU_BASE + B2L_USB_PMU_PHY));
  mUsb.PmuPhyDp = MmioRead32 ((UINTN)(B2L_USB_PMU_BASE + B2L_USB_PMU_PHY_DP));

  if (B2LMapOk (mUsbConMap)) {
    mUsb.UsbconVer    = MmioRead32 ((UINTN)(B2L_USB_USBCON_BASE + B2L_USB_USBCON_VER));
    mUsb.UsbconClkrst = MmioRead32 ((UINTN)(B2L_USB_USBCON_BASE + B2L_USB_USBCON_CLKRST));
  }

  if (B2LMapOk (mUsbDwc3Map)) {
    mUsb.SnpsId      = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_DWC3_GSNPSID));
    mUsb.Gctl        = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_DWC3_GCTL));
    mUsb.XhciCapLen  = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_XHCI_CAPLEN));
    mUsb.XhciParams1 = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_XHCI_HCSPARAMS1));
  }

  mUsbValid = TRUE;
}

/**
  USB ornegini ekrana bas. Renkler kaba yorumdur:
    yesil  -> deger makul (GSNPSID 0x5533.., xHCI CAPLENGTH != 0)
    gri    -> bilinmiyor / notr
  Haritalama basarisizsa yalnizca bu durumda kirmizi "USB.map" satiri
  basilir; normal durumda 4 satir kalir (yerlesim degismez).
**/
STATIC
VOID
B2LDrawUsb (
  VOID
  )
{
  if (!mUsbValid) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"USB     : probe skipped");
    return;
  }

  if (!B2LMapOk (mUsbConMap) || !B2LMapOk (mUsbDwc3Map)) {
    B2LUiPrint (
      B2L_UI_KIRMIZI,
      L"USB.map : con=%r dw3=%r",
      mUsbConMap,
      mUsbDwc3Map
      );
  }

  B2LUiPrint (B2L_UI_GRI, L"USB.pmu : %08x %08x", mUsb.PmuPhy, mUsb.PmuPhyDp);
  B2LUiPrint (B2L_UI_GRI, L"USB.con : %08x %08x", mUsb.UsbconVer, mUsb.UsbconClkrst);
  B2LUiPrint (
    (((mUsb.SnpsId & 0xFFFF0000U) == 0x55330000U) ||
     ((mUsb.SnpsId & 0xFFFF0000U) == 0x33310000U)) ? B2L_UI_YESIL : B2L_UI_GRI,
    L"USB.dw3 : %08x %08x",
    mUsb.SnpsId,
    mUsb.Gctl
    );
  B2LUiPrint (
    ((mUsb.XhciCapLen & 0xFFU) != 0) ? B2L_UI_YESIL : B2L_UI_GRI,
    L"USB.xhc : %08x %08x",
    mUsb.XhciCapLen,
    mUsb.XhciParams1
    );
}

/**
  UsbfnDwc3Dxe protokolunu bul, StartController'i cagir (PHY + core
  bring-up), sonra DWC3 durum registerlarini oku. Statik bolumde bir kez.
**/
STATIC
VOID
B2LUsbDevBringUp (
  VOID
  )
{
  EFI_USBFN_IO_PROTOCOL  *Fn;

  mUsbDev.Present = FALSE;
  mUsbDev.Gctl    = 0;
  mUsbDev.Dctl    = 0;
  mUsbDev.Dsts    = 0;
  Fn              = NULL;

  mUsbDev.LocateStatus = gBS->LocateProtocol (
                                (EFI_GUID *)(VOID *)&gEfiUsbFunctionIoProtocolGuid,
                                NULL,
                                (VOID **)&Fn
                                );
  if (EFI_ERROR (mUsbDev.LocateStatus) || (Fn == NULL) ||
      (Fn->StartController == NULL))
  {
    return;
  }

  mUsbDev.Present     = TRUE;
  mUsbDev.StartStatus = Fn->StartController (Fn);

  //
  // MMIO bu noktada B2LUsbRead tarafindan da haritalanmistir; okuma guvenli.
  //
  mUsbDev.Gctl = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_DWC3_GCTL));
  mUsbDev.Dctl = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_DWC3_DCTL));
  mUsbDev.Dsts = MmioRead32 ((UINTN)(B2L_USB_DWC3_BASE + B2L_USB_DWC3_DSTS));
}

/**
  Device-mode bring-up sonucunu bas.
**/
STATIC
VOID
B2LDrawUsbDev (
  VOID
  )
{
  if (!mUsbDev.Present) {
    B2LUiPrint (
      B2L_UI_GRI,
      L"USB.dev : no UsbFnIo (%r)",
      mUsbDev.LocateStatus
      );
    return;
  }

  B2LUiPrint (
    EFI_ERROR (mUsbDev.StartStatus) ? B2L_UI_KIRMIZI : B2L_UI_YESIL,
    L"USB.dev : st=%r",
    mUsbDev.StartStatus
    );
  B2LUiPrint (
    B2L_UI_GRI,
    L"USB.ctl : %08x %08x %08x",
    mUsbDev.Gctl,
    mUsbDev.Dctl,
    mUsbDev.Dsts
    );
}

STATIC
BOOLEAN
B2LIsMmio (
  IN EFI_MEMORY_TYPE  Type
  )
{
  return (Type == EfiMemoryMappedIO) || (Type == EfiMemoryMappedIOPortSpace);
}

STATIC
VOID
B2LDrawMemory (
  VOID
  )
{
  EFI_STATUS             Status;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVer;
  UINTN                  Count;
  UINTN                  Index;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT64                 DramTotal;
  UINT64                 FreeRam;
  UINT64                 Highest;
  UINT64                 End;
  BOOLEAN                AboveLimit;

  MapSize = 0;
  Status  = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : GetMemoryMap probe error (%r)", Status);
    return;
  }

  MapSize += 8 * DescSize;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : allocation failed");
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"RAM     : GetMemoryMap error (%r)", Status);
    FreePool (Map);
    return;
  }

  DramTotal  = 0;
  FreeRam    = 0;
  Highest    = 0;
  AboveLimit = FALSE;
  Count      = MapSize / DescSize;

  for (Index = 0; Index < Count; Index++) {
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescSize);

    End = D->PhysicalStart + MultU64x64 (D->NumberOfPages, 4096);

    //
    // MMIO haritada yuksek adreslerde gorunebilir; onu DRAM saymadan
    // yalnizca RAM'i topla. S10e'de 6 GiB'in TAMAMI (rezerv dahil) tarif
    // edilmis olmali -> toplam 6144 MB cikar.
    //
    if (!B2LIsMmio (D->Type)) {
      if (D->PhysicalStart < B2L_MMIO_LIMIT) {
        DramTotal += MultU64x64 (D->NumberOfPages, 4096);
      }

      if (D->PhysicalStart >= B2L_MMIO_LIMIT) {
        AboveLimit = TRUE;
      }
    }

    if (D->Type == EfiConventionalMemory) {
      FreeRam += MultU64x64 (D->NumberOfPages, 4096);
    }

    if (End > Highest) {
      Highest = End;
    }
  }

  B2LUiPrint (B2L_UI_BEYAZ, L"RAM     : %u MB DRAM, %u MB free",
         (UINT32)DivU64x32 (DramTotal, 1024 * 1024),
         (UINT32)DivU64x32 (FreeRam, 1024 * 1024));
  B2LUiPrint (B2L_UI_BEYAZ, L"Memmap  : %u desc, max end 0x%lx", (UINT32)Count, Highest);
  B2LUiPrint (
    AboveLimit ? B2L_UI_KIRMIZI : B2L_UI_YESIL,
    L"RAM >=0x980000000  : %a",
    AboveLimit ? "PRESENT (WRONG)" : "none (ok)"
    );

  FreePool (Map);
}

STATIC
VOID
B2LDrawDevices (
  VOID
  )
{
  EFI_STATUS                    Status;
  UINTN                         HandleCount;
  UINTN                         Index;
  UINTN                         Shown;
  EFI_HANDLE                    *Handles;
  EFI_BLOCK_IO_PROTOCOL         *Bio;
  UINT64                        Bytes;

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiBlockIoProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_KIRMIZI, L"Block IO: none (%r)", Status);
  } else {
    B2LUiPrint (B2L_UI_BEYAZ, L"Block IO: %u device", (UINT32)HandleCount);
    //
    // Yalnizca ilk iki cihaz yazilir: ekranin altinda canli dokunma
    // satirlarina yer kalmasi gerekiyor (bkz. B2L_UI_OLCEK).
    //
    Shown = 0;
    for (Index = 0; (Index < HandleCount) && (Shown < 2); Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
      if (EFI_ERROR (Status) || (Bio->Media == NULL)) {
        continue;
      }

      Bytes = MultU64x64 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize);
      B2LUiPrint (B2L_UI_GRI, L"  #%u %u MB blk=%u %a%a",
             (UINT32)(Shown + 1),
             (UINT32)DivU64x32 (Bytes, 1024 * 1024),
             (UINT32)Bio->Media->BlockSize,
             Bio->Media->RemovableMedia ? "removable" : "fixed",
             Bio->Media->MediaPresent ? "" : " (no media)");
      Shown++;
    }
    FreePool (Handles);
  }

  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol,
                   &gEfiSimpleFileSystemProtocolGuid,
                   NULL,
                   &HandleCount,
                   &Handles
                   );
  if (EFI_ERROR (Status)) {
    B2LUiPrint (B2L_UI_GRI, L"FAT/FS  : 0 volume");
  } else {
    B2LUiPrint (B2L_UI_BEYAZ, L"FAT/FS  : %u volume", (UINT32)HandleCount);
    FreePool (Handles);
  }
}

STATIC
VOID
B2LDrawDisplay (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL)) {
    B2LUiPrint (B2L_UI_BEYAZ, L"Display : %ux%u (GOP mode %u)",
           Gop->Mode->Info->HorizontalResolution,
           Gop->Mode->Info->VerticalResolution,
           Gop->Mode->Mode);
  } else {
    B2LUiPrint (B2L_UI_KIRMIZI, L"Display : no GOP (%r)", Status);
  }
}

STATIC
VOID
B2LSelfTest (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_ABSOLUTE_POINTER_PROTOCOL  *Abs;
  EFI_INPUT_KEY                  Key;
  B2L_TOUCH_DIAG_PROTOCOL        *TouchDiag;
  BOOLEAN                        HasTouch;
  BOOLEAN                        Exit;
  UINT32                         DynTop;
  B2L_USB_SERIAL_DIAG_PROTOCOL   *UsbDiag;
  BOOLEAN                        UsbDiagOk;
  CHAR16                         LastKey[56];

  HasTouch    = FALSE;
  Exit        = FALSE;
  Abs         = NULL;
  TouchDiag   = NULL;
  DynTop      = 0;
  UsbDiag     = NULL;
  UsbDiagOk   = FALSE;

  //
  // Buyuk yazi icin GOP + HII font gerekli. Ikisi de FV'de
  // (GraphicsConsoleDxe ve HiiDatabaseDxe); bulunamazsa konsol yazisina
  // dusmek yerine acikca bildir.
  //
  mUiReady = FALSE;
  Status   = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mUiGop);
  if (!EFI_ERROR (Status) && (mUiGop->Mode != NULL) && (mUiGop->Mode->Info != NULL)) {
    Status = gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mUiFont);
    if (!EFI_ERROR (Status) && (mUiFont != NULL)) {
      mUiFb     = (UINT32 *)(UINTN)mUiGop->Mode->FrameBufferBase;
      mUiStride = mUiGop->Mode->Info->PixelsPerScanLine;
      mUiReady  = TRUE;
    }
  }

  if (!mUiReady) {
    Print (L"B2L self-test: GOP/HII font unavailable (big text disabled).\r\n");
    return;
  }

  //
  // ConIn "lazy" moddadir: ILK ReadKeyStroke cagrisi ConnectConIn olayini
  // tetikler (bkz. ConSplitter.c) ve konsol kendini yeniden cizerken
  // framebuffer'i SILEBILIR. Bu, canli dongudeki ilk turda - yani statik blok
  // (baslik + bilgi) cizildikten SONRA - oldugu icin ekranin USTU bos
  // kaliyordu (bkz. PORT_S10E.md 6.5: "ust bos"). Tetiklemeyi CIZIMDEN ONCE
  // yapiyoruz: sahte ReadKeyStroke + TPL pompası konsolun yeniden cizimini
  // bitirsin, ardindan temizleyip uzerine yazariz. Bayrak tek seferlik
  // oldugu icin canli dongude bir daha tetiklenmez.
  //
  if ((gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
    gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));
  }

  B2LUiClearFrom (0);
  mUiY = B2L_UI_KENAR;

  UnicodeSPrint (LastKey, sizeof (LastKey), L"(not yet)");

  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");
  B2LUiPrint (B2L_UI_BEYAZ, L" S10e (beyond0lte) - UEFI SELF TEST");
  B2LUiPrint (B2L_UI_GRI,   L" No ESP; verification screen. Photograph.");
  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");

  B2LDrawDisplay ();
  B2LDrawMemory ();
  B2LDrawDevices ();
  B2LUsbRead ();
  B2LDrawUsb ();
  B2LUsbDevBringUp ();
  B2LDrawUsbDev ();

  //
  // Dokunma teshis protokolu. Ekranda artik DOKUNMA SATIRI GOSTERILMIYOR
  // (yer kapliyordu, bkz. PORT_S10E.md); bu blok yalnizca gercek
  // AbsolutePointer protokolunu elde etmek icin duruyor. UI ConSplitter'in
  // toplu (agregat) protokolu yerine DOGRUDAN bizim protokolla konusmak
  // zorunda: ConSplitter mode'u 0x10000 (65536) ve bizim surucumuz oraya
  // bagli degil.
  //
  // Signature/Size, iki modulde elle tutulan yerlesimin ayni oldugunu
  // dogrular; tutmazsa teshis alani okunmaz.
  //
  Status = gBS->LocateProtocol (
                  (EFI_GUID *)(VOID *)&mB2LTouchDiagGuid,
                  NULL,
                  (VOID **)&TouchDiag
                  );
  if (!EFI_ERROR (Status) &&
      (TouchDiag != NULL) &&
      (TouchDiag->Diag.Signature == B2L_TOUCH_DIAG_SIGNATURE) &&
      (TouchDiag->Diag.Size == (UINT32)sizeof (B2L_TOUCH_DIAG)) &&
      (TouchDiag->Diag.AbsPointer != 0))
  {
    Abs      = (EFI_ABSOLUTE_POINTER_PROTOCOL *)(UINTN)TouchDiag->Diag.AbsPointer;
    HasTouch = TRUE;
  } else {
    TouchDiag = NULL;
  }

  //
  // USB device-mode sinif surucusunun teshis protokolu. M2a'da asil soru
  // "PC enumerasyonu yapti mi": su (setup) ve cf (SET_CONFIGURATION) bu
  // yuzden ekranda. Yerlesim tutmazsa satir hic cizilmez.
  //
  Status = gBS->LocateProtocol (
                  (EFI_GUID *)(VOID *)&mB2LUsbDiagGuid,
                  NULL,
                  (VOID **)&UsbDiag
                  );
  if (!EFI_ERROR (Status) &&
      (UsbDiag != NULL) &&
      (UsbDiag->Diag.Signature == B2L_USB_SERIAL_DIAG_SIGNATURE) &&
      (UsbDiag->Diag.Size == (UINT32)sizeof (B2L_USB_SERIAL_DIAG)))
  {
    UsbDiagOk = TRUE;

    //
    // M3: USB konsolunun STO'sunu al. Yukaridaki self-test yazisini PC'ye
    // (yalniz USB'ye) basmak icin kullanilir; ekran GOP/HII ile cizilmeye
    // devam eder. Pointer 0 ise (ornegin surucu konsolu yayinlayamadiysa)
    // B2LUsbPrint sessizce gecer.
    //
    if (UsbDiag->Diag.UsbConOut != 0) {
      mB2LUsbConOut = (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *)(UINTN)UsbDiag->Diag.UsbConOut;
    }
  } else {
    UsbDiag = NULL;
  }

  if (!HasTouch) {
    Status   = gBS->LocateProtocol (&gEfiAbsolutePointerProtocolGuid, NULL, (VOID **)&Abs);
    HasTouch = !EFI_ERROR (Status);
  }

  //
  // Surucu entry'de bring-up yapmisti; ama entry APRIORI sirasinda calisir ve
  // o an tsp_io/tsp_avdd raylarini acan PmicDxe yuklu olmayabilir, yani
  // panel I2C'ye hic yanit vermemis olabilir. Self-test baslarken tum DXE
  // suruculeri yuklendigi icin Reset(TRUE) bir kez daha guc + bring-up
  // dener. Basarisiz olsa da ekran calismaya devam eder.
  //
  if (HasTouch && (Abs->Reset != NULL)) {
    Abs->Reset (Abs, TRUE);
  }

  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");
  B2LUiPrint (B2L_UI_GRI,   L"VolUp/Down=Up/Down Bixby=Enter Bixby+Vol=Exit");
  B2LUiPrint (B2L_UI_BEYAZ, L"--------------------------------------------------------------");

  //
  // M3: ayni teshis bilgisini USB seri konsoluna da bas. Ekran etkilenmez;
  // PC'de "screen /dev/ttyACM0 115200" bu satirlari gosterir. Baslik bir kez,
  // canli satir asagidaki dongude \r ile ayni yere yazilir.
  //
  B2LUsbPrint (L"\r\n=== beyond0lte UEFI self-test (USB seri konsol) ===\r\n");
  B2LUsbPrint (L"Dokunma/klavye/keypad self-test calisiyor. PC'de tuslayin.\r\n");
  B2LUsbPrint (L"VolUp/Down=Up/Down Bixby=Enter Bixby+Vol=Exit\r\n");

  //
  // Statik bolge buraya kadar cizildi. Asagisi her karede temizlenip
  // yeniden cizilecek canli bolge.
  //
  DynTop = mUiY;

  for (;;) {
    //
    // Bu dongu kendi govdesinde gBS->Stall ile bekledigi icin TPL_CALLBACK
    // zamanlayicilari (ornegin B2LUsbSerialDxe'nin USB olay pompasi)
    // kendiliginden islemez. Raise/RestoreTPL cifti bekleyen tum notify
    // fonksiyonlarini pompalar.
    //
    gBS->RestoreTPL (gBS->RaiseTPL (TPL_HIGH_LEVEL));

    B2LUiClearFrom (DynTop);
    mUiY = DynTop;

    if ((gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        if (Key.ScanCode == SCAN_UP) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Up   -> SCAN_UP (0x01)");
        } else if (Key.ScanCode == SCAN_DOWN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Volume Down -> SCAN_DOWN (0x02)");
        } else if (Key.ScanCode == SCAN_ESC) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby hold -> SCAN_ESC (0x17)");
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
          UnicodeSPrint (LastKey, sizeof (LastKey), L"Bixby short -> ENTER (0x0D)");
        } else if ((Key.ScanCode == SCAN_LEFT) || (Key.ScanCode == SCAN_RIGHT)) {
          UnicodeSPrint (
            LastKey,
            sizeof (LastKey),
            L"Bixby+Vol -> %a (exit)",
            (Key.ScanCode == SCAN_LEFT) ? "LEFT" : "RIGHT"
            );
          Exit = TRUE;
        } else {
          UnicodeSPrint (
            LastKey,
            sizeof (LastKey),
            L"Scan=0x%04x Uni=0x%04x",
            Key.ScanCode,
            Key.UnicodeChar
            );
        }
      }
    }

    //
    // Tus tanisi (dokunma satirlari kaldirildi: ekrani tasirip ust bilgi
    // blogunu eziyordu, bkz. PORT_S10E.md 6.5).
    //
    B2LUiPrint (B2L_UI_BEYAZ, L"Key     : %s", LastKey);

    //
    // USB device-mode: basarisizsa HANGI asama + o asamanin status'u; basarili
    // ise enumerasyon kaniti. su = gelen setup sayisi, cf = SET_CONFIGURATION,
    // ad = atanan adres, pm = olay pompasi kac kez cagrildi (0 ise pompa hic
    // donmedi), ev = reset/attach/detach gibi bus olayi sayisi (0 ise host
    // hic konusmadi). Tek satir, cunku ekran dolu.
    //
    if (UsbDiagOk) {
      if (UsbDiag->Diag.Stage != B2L_USB_STAGE_DONE) {
        B2LUiPrint (
          B2L_UI_KIRMIZI,
          L"USB.ser: s=%u st=%r",
          UsbDiag->Diag.Stage,
          (EFI_STATUS)(UINTN)UsbDiag->Diag.EntryStatus
          );
      } else {
        B2LUiPrint (
          (UsbDiag->Diag.Configured != 0) ? B2L_UI_YESIL : B2L_UI_SARI,
          L"USB.ser: su=%u cf=%u ad=%u pm=%u ev=%u",
          UsbDiag->Diag.Setups,
          UsbDiag->Diag.Configured,
          UsbDiag->Diag.Addr,
          UsbDiag->Diag.Pumps,
          UsbDiag->Diag.BusEvents
          );
        //
        // CDC-ACM veri yolu: rx = host'tan okunan bayt, tx = geri yazilan
        // (echo) bayt, lc = SET_LINE_CODING sayisi. PC'de /dev/ttyACM* acilip
        // veri gonderilince rx/tx artar.
        //
        B2LUiPrint (
          ((UsbDiag->Diag.RxBytes != 0) || (UsbDiag->Diag.TxBytes != 0))
            ? B2L_UI_YESIL : B2L_UI_GRI,
          L"USB.ser: rx=%u tx=%u lc=%u",
          UsbDiag->Diag.RxBytes,
          UsbDiag->Diag.TxBytes,
          UsbDiag->Diag.LineCodings
          );
      }
    } else {
      B2LUiPrint (B2L_UI_GRI, L"USB.ser: no diag protocol");
    }

    if (Exit) {
      break;
    }

    //
    // M3: canli satiri USB konsoluna da yaz. \r ile AYNI satira yazip saga
    // bosluk birakiriz; boylece PC terminalinde titremesiz tek satir olur.
    // co = konsola yazilan karakter (STO), kr = okunan tus (STI).
    //
    B2LUsbPrint (L"\rKey: %s", LastKey);
    if (UsbDiagOk && (UsbDiag->Diag.Stage == B2L_USB_STAGE_DONE)) {
      B2LUsbPrint (
        L"   rx=%u tx=%u co=%u kr=%u     ",
        UsbDiag->Diag.RxBytes,
        UsbDiag->Diag.TxBytes,
        UsbDiag->Diag.ConsoleOut,
        UsbDiag->Diag.KeyReads
        );
    }

    gBS->Stall (20000);
  }

  B2LUiClearFrom (DynTop);
  mUiY = DynTop;
  B2LUiPrint (B2L_UI_BEYAZ, L"Exiting; device will power off.");
  gBS->Stall (2000000);
}

//
// ====================== M3c: FV'den UEFI Shell boot option =================
//
// Shell derlemede FV'ye gomulu (Shell.inf, FILE_GUID = PcdShellFile). Ancak bu
// platformda kullanilan MsBootOptionsLib (SiliciumPkg) yalnizca SSD/USB
// politika seceneklerini kaydediyor; "Internal UEFI Shell" secenegini
// olusturmuyor. Cihazda FAT/ESP bolumu olmadigindan SSD/USB secenekleri
// basarisiz olur ve BDS UnableToBoot'a duser (self-test ekrani). Bu fonksiyon
// FV'deki Shell'i isaret eden bir boot option ekler; konsol M3'te USB seri
// hattina baglandigi icin dogrudan interaktif UEFI Shell acilir.
//
// MsBootOptionsLib.c'deki RegisterFvBootOption mantiginin dar bir kopyasi:
// ayni (FilePath, Description, Attributes) zaten varsa tekrar eklenmez, yoksa
// BootOrder'in SONUNA eklenir (SSD/USB'den sonra denenir).
//
STATIC
EFI_STATUS
B2LRegisterShellBootOption (
  VOID
  )
{
  EFI_STATUS                         Status;
  EFI_HANDLE                         *FvHandles;
  UINTN                              FvCount;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  UINT32                             AuthStatus;
  VOID                               *Buffer;
  UINTN                              Size;
  EFI_DEVICE_PATH_PROTOCOL           *FvPath;
  EFI_DEVICE_PATH_PROTOCOL           *ShellPath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  EFI_BOOT_MANAGER_LOAD_OPTION       *Existing;
  UINTN                              ExistingCount;

  FvHandles = NULL;
  FvCount   = 0;
  FvPath    = NULL;

  //
  // PcdShellFile GUID'ini tasiyan FV'yi bul. ReadSection ile dosyanin
  // gercekten var oldugunu dogrulariz; yoksa bu FV atlanir.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &FvCount,
                  &FvHandles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < FvCount; Index++) {
    Status = gBS->HandleProtocol (
                    FvHandles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Buffer = NULL;
    Size   = 0;
    Status = Fv->ReadSection (
                  Fv,
                  PcdGetPtr (PcdShellFile),
                  EFI_SECTION_PE32,
                  0,
                  &Buffer,
                  &Size,
                  &AuthStatus
                  );
    if (Buffer != NULL) {
      FreePool (Buffer);
    }

    if (!EFI_ERROR (Status)) {
      FvPath = DevicePathFromHandle (FvHandles[Index]);
      break;
    }
  }

  FreePool (FvHandles);

  if (FvPath == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Fv()/FvFile(shell) cihaz yolunu kur.
  //
  EfiInitializeFwVolDevicepathNode (&FileNode, PcdGetPtr (PcdShellFile));
  ShellPath = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (ShellPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // InitializeLoadOption FilePath'i KOPYALAR; ShellPath bu yuzden serbest
  // birakilabilir.
  //
  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
             L"UEFI Shell",
             ShellPath,
             NULL,
             0
             );
  FreePool (ShellPath);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Idempotent: ayni secenek zaten varsa ekleme (aksi halde her acilista
  // BootOrder'a yeni bir kopya duserdi).
  //
  Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
  if ((Existing != NULL) && (EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount) != -1)) {
    EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
    EfiBootManagerFreeLoadOption (&Option);
    return EFI_SUCCESS;
  }

  if (Existing != NULL) {
    EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
  }

  Status = EfiBootManagerAddLoadOptionVariable (&Option, (UINTN)-1);
  EfiBootManagerFreeLoadOption (&Option);

  return Status;
}

/**
  Verilen FV dosya GUID'i icin Fv(...)/FvFile(guid) cihaz yolunu kurar.
  B2LRegisterShellBootOption ile ayni mantik; GUID tasiyan FV'yi ReadSection
  ile dogrular.
**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
B2LFvFilePath (
  IN CONST EFI_GUID  *FileGuid
  )
{
  EFI_STATUS                         Status;
  EFI_HANDLE                         *FvHandles;
  UINTN                              FvCount;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  UINT32                             AuthStatus;
  VOID                               *Buffer;
  UINTN                              Size;
  EFI_DEVICE_PATH_PROTOCOL           *FvPath;
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;

  FvHandles = NULL;
  FvCount   = 0;
  FvPath    = NULL;
  Path      = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &FvCount,
                  &FvHandles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (Index = 0; Index < FvCount; Index++) {
    Status = gBS->HandleProtocol (
                    FvHandles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Buffer = NULL;
    Size   = 0;
    Status = Fv->ReadSection (
                  Fv,
                  (EFI_GUID *)(VOID *)FileGuid,
                  EFI_SECTION_PE32,
                  0,
                  &Buffer,
                  &Size,
                  &AuthStatus
                  );
    if (Buffer != NULL) {
      FreePool (Buffer);
    }

    if (!EFI_ERROR (Status)) {
      FvPath = DevicePathFromHandle (FvHandles[Index]);
      break;
    }
  }

  FreePool (FvHandles);

  if (FvPath == NULL) {
    return NULL;
  }

  EfiInitializeFwVolDevicepathNode (&FileNode, (EFI_GUID *)(VOID *)FileGuid);
  Path = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  return Path;
}

/**
  Bir FV uygulamasini boot option olarak kaydet (idempotent). Ayni
  (FilePath, Description) zaten varsa eklenmez; yoksa BootOrder'in SONUNA
  eklenir (normal BOOT seceneklerinden sonra).
**/
STATIC
EFI_STATUS
B2LRegisterFvApp (
  IN CONST EFI_GUID  *FileGuid,
  IN CONST CHAR16    *Description,
  IN VOID            *OptionalData,
  IN UINTN            OptionalDataSize
  )
{
  EFI_STATUS                    Status;
  EFI_DEVICE_PATH_PROTOCOL      *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Existing;
  UINTN                         ExistingCount;

  Path = B2LFvFilePath (FileGuid);
  if (Path == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
             (CHAR16 *)(VOID *)Description,
             Path,
             OptionalData,
             OptionalDataSize
             );
  FreePool (Path);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Existing = EfiBootManagerGetLoadOptions (&ExistingCount, LoadOptionTypeBoot);
  if ((Existing != NULL) &&
      (EfiBootManagerFindLoadOption (&Option, Existing, ExistingCount) != -1))
  {
    EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
    EfiBootManagerFreeLoadOption (&Option);
    return EFI_SUCCESS;
  }

  if (Existing != NULL) {
    EfiBootManagerFreeLoadOptions (Existing, ExistingCount);
  }

  Status = EfiBootManagerAddLoadOptionVariable (&Option, (UINTN)-1);
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

/**
  Bir FV uygulamasini simdi yukle ve baslat (BootOrder'a dokunmaz).
**/
STATIC
EFI_STATUS
B2LStartFvApp (
  IN CONST EFI_GUID  *FileGuid
  )
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_HANDLE                 Image;
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;

  Path = B2LFvFilePath (FileGuid);
  if (Path == NULL) {
    return EFI_NOT_FOUND;
  }

  Image  = NULL;
  Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // LoadOptions bos birakilir: B2LTool menusu acilir.
  //
  Status = gBS->HandleProtocol (
                  Image,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Loaded
                  );
  if (!EFI_ERROR (Status) && (Loaded != NULL)) {
    Loaded->LoadOptions     = NULL;
    Loaded->LoadOptionsSize = 0;
  }

  Status = gBS->StartImage (Image, NULL, NULL);
  gBS->UnloadImage (Image);
  return Status;
}

//
// DeviceBootManagerLib implements. The rest is the same as the Silicon Null
// implementation; only UnableToBoot does real work on this device.
//

EFI_HANDLE
EFIAPI
DeviceBootManagerBeforeConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath,
  OUT BDS_CONSOLE_CONNECT_ENTRY **PlatformConsoles
  )
{
  //
  // M3: USB seri konsolunu ConIn/ConOut/ErrOut listelerine ekle. Bu fonksiyon
  // SiliciumPkg/BootManagerLib'deki Keypad/Display kayitlarindan hemen sonra
  // cagrilir, yani BdsEntry konsollari baglamadan once. ConPlatform +
  // ConSplitter bunu toplu (agregat) konsola katar: PC klavyesi gST->ConIn'e,
  // gST->ConOut ise CDC-ACM bulk IN'e gider.
  //
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)(VOID *)&mB2LUsbConsolePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)(VOID *)&mB2LUsbConsolePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)(VOID *)&mB2LUsbConsolePath, NULL);

  //
  // M3c: FV'deki UEFI Shell'i boot option olarak kaydet. Bu fonksiyon
  // PlatformBootManagerBeforeConsole icinde MsBootOptionsLib'in varsayilan
  // secenekleri (SSD/USB) kaydettigi noktadan SONRA calisir; Shell onlarin
  // ardina eklenir. Konsol zaten USB seri hattina bagli oldugu icin Shell
  // dogrudan interaktif olur.
  //
  B2LRegisterShellBootOption ();

  //
  // B2LTool'u da boot option olarak kaydet. Kategori APP oldugu icin BDS
  // normal boot dongusunde ATLAR (bkz. BdsDxe/BdsEntry.c); yani cihaz
  // kendiliginden araca girmez. Kullanici yalnizca boot menusunden secer
  // ya da asagidaki UnableToBoot yolu cagirir.
  //
  B2LRegisterFvApp (&mB2LToolGuid, L"beyond0lte Tools", NULL, 0);

  //
  // Dogrudan TWRP'ye gecen ikinci boot option: ayni B2LTool, fakat
  // OptionalData = "recov". Secilince B2LTool PMU INFORM3'e recovery
  // reboot-reason'i yazip SWRESET atar; S-Boot TWRP'yi acar (bkz.
  // Applications/B2LTool). boylece menusuz tek secimle recovery.
  //
  B2LRegisterFvApp (&mB2LToolGuid, L"TWRP (recovery)", L"recov", sizeof (L"recov"));

  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerAfterConsole (
  VOID
  )
{
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerOnDemandConInConnect (
  VOID
  )
{
  return NULL;
}

VOID
EFIAPI
DeviceBootManagerProcessBootCompletion (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return;
}

EFI_STATUS
EFIAPI
DeviceBootManagerPriorityBoot (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return EFI_SUCCESS;
}

VOID
EFIAPI
DeviceBootManagerUnableToBoot (
  VOID
  )
{
  //
  // Once B2LTool menusunu ac. Kullanici TWRP'ye gecmek ya da imaj yazmak
  // icin buradan secebilir. Arac donerse (kullanici cikti) eski teshis
  // ekranina dusuyoruz; davranis boylece geriye uyumlu kaliyor.
  //
  B2LStartFvApp (&mB2LToolGuid);

  B2LSelfTest ();
}

VOID
EFIAPI
DeviceBootManagerBdsEntry (
  VOID
  )
{
  return;
}