/** @file
  beyond0lte'ye ozgu USB device-mode teshis protokolu.

  B2LUsbSerialDxe bu protokolu yayinlar; BDS'in self-test ekrani okur. Amac
  dokunma teshisindeki (B2LTouchDiag) ile ayni: cihazda harf ureten konsol
  olmadigi icin enumerasyonun nerede durdugunu TEK flash'ta ekrandan ayirt
  etmek. Mumkun oldugu kadar erken yayinlanir; sonraki adim coksede bile bu
  bilgi ekrana ulasir.

  GUID iki modulde de yerel sabit olarak tanimlanir (INF'e protocol eklemeye
  gerek yok, bkz. B2LTouchDiag.h). Yerlesim `B2LPlatformBootManager.c` icinde
  AYNEN tekrarlanir; Signature/Size cifti iki tarafin ayni oldugunu dogrular.
**/

#ifndef B2L_USB_SERIAL_DIAG_H_
#define B2L_USB_SERIAL_DIAG_H_

#include <Uefi.h>

#define B2L_USB_SERIAL_DIAG_PROTOCOL_GUID \
  { 0x2f7c9a14, 0x5b6d, 0x4a83, { 0x91, 0x2e, 0x7c, 0x4f, 0xb1, 0x0a, 0x63, 0xd8 } }

//
// Yerlesim iki modulde elle tekrarlandigi icin surum isaretcisi. Ekran
// Signature/Size tutmazsa "layout mismatch" basar.
//
#define B2L_USB_SERIAL_DIAG_SIGNATURE  0x31445542U   // "BUD1" (kucuk endian baytlar)

//
// Surucu hangi asamada durdu? Entry protokolu ilk is yayinladigi icin sonraki
// adimlar coksede bu numara ekrana ulasir.
//
#define B2L_USB_STAGE_ENTRY      0   // entry basladi
#define B2L_USB_STAGE_INSTALL    1   // diag protokolu yayinlandi
#define B2L_USB_STAGE_LOCATE     2   // EFI_USBFN_IO_PROTOCOL arandi
#define B2L_USB_STAGE_START      3   // StartController (PHY + core)
#define B2L_USB_STAGE_BUFFER     4   // EP0 DMA tamponu ayrildi
#define B2L_USB_STAGE_CONFIG     5   // ConfigureEnableEndpoints (RUN_STOP)
#define B2L_USB_STAGE_TIMER      6   // olay pompalama zamanlayicisi
#define B2L_USB_STAGE_DONE       7   // hepsi tamam

typedef struct {
  //
  // Sozlesme: Signature/Size. Size = sizeof (B2L_USB_SERIAL_DIAG).
  //
  UINT32  Signature;
  UINT32  Size;

  //
  // Asama takibi. Stage = en son denenen adim, EntryStatus = o adimin
  // EFI_STATUS degeri (EFI_STATUS 64-bit oldugu icin UINT64).
  //
  UINT32  Stage;
  UINT32  Reserved0;
  UINT64  EntryStatus;

  //
  // Adim adim sonuclar. Enumerasyon nerede takildiysa buradan okunur.
  //
  UINT64  LocateStatus;
  UINT64  StartStatus;
  UINT64  BufferStatus;
  UINT64  ConfigStatus;
  UINT64  TimerStatus;

  //
  // Kontrol aktarim sayaclari.
  //
  UINT32  Pumps;        // olay dongusu kac kez calisti
  UINT32  Setups;       // EfiUsbMsgSetupPacket sayisi
  UINT32  Configured;   // SET_CONFIGURATION alindi mi (0/1)
  UINT32  Addr;         // SET_ADDRESS ile atanan adres (surucuya bildirilir)
  UINT32  Stalls;       // desteklemedigimiz icin STALL ettigimiz setup sayisi
  UINT32  TxDone;       // Tx tamamlanma olayi
  UINT32  RxDone;       // Rx tamamlanma olayi
  UINT32  BusEvents;    // reset/attach/detach olayi

  //
  // Son setup paketi (hangi istegin geldigini gormek icin).
  //
  UINT32  LastBRequest;
  UINT32  LastBMRequestType;
  UINT32  LastValue;

  //
  // ConfigureEnableEndpoints sonrasi okunan DWC3 durumu (RUN_STOP gorunur).
  //
  UINT32  Gctl;
  UINT32  Dctl;
  UINT32  Dsts;
} B2L_USB_SERIAL_DIAG;

typedef struct {
  B2L_USB_SERIAL_DIAG  Diag;
} B2L_USB_SERIAL_DIAG_PROTOCOL;

#endif
