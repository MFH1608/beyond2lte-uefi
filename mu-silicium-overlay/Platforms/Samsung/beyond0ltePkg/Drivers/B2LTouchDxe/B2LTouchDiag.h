/** @file
  beyond0lte'ye ozgu dokunma teshis protokolu.

  B2LTouchDxe bu protokolu yayinlar; BDS'in self-test ekrani okur. Amac:
  cihazda harf ureten klavye/seri konsol olmadigi icin dokunma sorununu
  TEK flash'ta ayirt edebilmek. Sayaclar, son durum kodlari ve son olay
  baytlari dogrudan ekrana basilir.

  GUID iki modulde de yerel sabit olarak tanimlanir (INF'e protocol
  eklemeye gerek kalmaz). Yerlesim `B2LPlatformBootManager.c` icinde AYNEN
  tekrarlanir; Signature/Size cifti iki tarafin ayni oldugunu dogrular.
**/

#ifndef B2L_TOUCH_DIAG_H_
#define B2L_TOUCH_DIAG_H_

#include <Uefi.h>

#define B2L_TOUCH_DIAG_PROTOCOL_GUID \
  { 0x9c1f4b52, 0x8d36, 0x4c01, { 0xa7, 0x62, 0x51, 0x0e, 0x9b, 0x3d, 0x77, 0xc4 } }

//
// Yerlesim iki modulde elle tekrarlandigi icin surum isaretcisi. Ekran
// Signature ya da Size tutmazsa "diag layout mismatch" basar: bu, iki dosyayi
// ayni anda guncellemeyi unuttugumuz anlamina gelir.
//
#define B2L_TOUCH_DIAG_SIGNATURE  0x31474442U   // "BDG1" (kucuk endian baytlar)

#define B2L_TOUCH_DIAG_EVENT_SIZE  8

//
// B2LTouchEntry hangi asamada durdu? Self-test ekrani bunu "Touch.entry"
// satirinda gosterir. Surucu protokolu ILK is olarak yayinladigi icin,
// sonraki bir adim basarisiz olsa bile bu bilgi ekrana ulasir.
//
#define B2L_TOUCH_STAGE_ENTRY     0
#define B2L_TOUCH_STAGE_I2C       1
#define B2L_TOUCH_STAGE_EVENTS    2
#define B2L_TOUCH_STAGE_INSTALL   3
#define B2L_TOUCH_STAGE_PROBE     4
#define B2L_TOUCH_STAGE_RESET     5
#define B2L_TOUCH_STAGE_TIMER     6
#define B2L_TOUCH_STAGE_DONE      7

//
// PmicLocate: dokunma guc raylarini beslemek icin PMIC regulator protokolu
// arandi mi?
//
#define B2L_TOUCH_PMIC_NOT_TRIED  0
#define B2L_TOUCH_PMIC_FOUND      1

typedef struct {
  //
  // Sozlesme: Signature/Size. Size = sizeof (B2L_TOUCH_DIAG).
  //
  UINT32  Signature;
  UINT32  Size;

  //
  // B2LTouchEntry asama takibi. Stage = o an denenen adim, EntryStatus = o
  // adimin EFI_STATUS degeri (EFI_STATUS 64-bit oldugu icin UINT64).
  //
  UINT32  Stage;
  UINT32  PmicLocate;     // B2L_TOUCH_PMIC_* ya da kirpilmis hata kodu

  UINT64  EntryStatus;
  UINT64  AbsPointer;     // basariliysa kurulan AbsolutePointer protokolu
  UINT64  LastStatus;     // son komut/transfer denemesinin gercek EFI_STATUS'i

  //
  // PollEvents cagri sayaci ve sonuclari.
  //
  UINT32  Polls;
  UINT32  Empty;          // byte0 == 0: FIFO bos
  UINT32  Errors;         // I2C okuma hatasi
  UINT32  StatusEvents;   // eid != 0 (status/gesture) olaylar
  UINT32  CoordEvents;    // eid == 0 (koordinat) olaylar
  UINT32  Ignored;        // parse edildi ama kuyruga girmedi

  //
  // Dokunma guc raylari. tsp_io = S2MPS19 LDO19, tsp_avdd = S2MPS19 LDO27.
  // LdoUv: guc verildikten sonra okunan gerilim (uV); 0 = bilinmiyor.
  //
  UINT32  LdoUv[2];

  //
  // Son basarisiz transferden sonra HSI2C10'un ham registerlari. NACK mi
  // (INT_STATUS NO_DEV), zaman asimi mi, yoksa master durumu ne: hepsini
  // buradan ayirt ederiz.
  //
  UINT32  IntStatus;
  UINT32  TransStatus;

  //
  // HSI2C10 adres taramasi. Bit j (j = 0..127) -> adres j ACK verdi.
  //
  UINT32  ScanAck[4];

  //
  // Probe (0x22/0x21/0x55) sirasinda okunan kimlik degerleri.
  //
  UINT8   DeviceId[5];
  UINT8   Integrity;
  UINT8   BootStatus;

  //
  // TouchReset/ProbeController cagri sayaclari ve guc durumu.
  //
  UINT8   ResetCalls;
  UINT8   ProbeCalls;
  UINT8   PowerOk;        // iki ray da acildiysa 1
  UINT8   ScanDone;       // adres taramasi yapildi mi
  UINT8   LdoSetOk[2];    // SetLdo sonucu: {LDO19, LDO27}, 0/1

  //
  // Son islenen ham olay (8 bayt). Hic olay yoksa sifir kalir.
  //
  UINT8   LastEvent[B2L_TOUCH_DIAG_EVENT_SIZE];

  //
  // Bring-up sirasinda okunan/yazilan register degerleri. Bu alanlar
  // komut haritasinin (0x30, 0xE4) dogru oldugunu da dogrular.
  //
  UINT8   FnBefore[2];    // 0x30 yazmadan onceki deger
  UINT8   FnAfter[2];     // 0x30 yazdiktan sonraki deger
  UINT8   PowerMode;      // 0xE4 okunan deger (0xFF: bilinmiyor)
  UINT8   BringUpOk;      // bring-up yazilari basarili mi (0/1)
  UINT8   Reserved[5];
} B2L_TOUCH_DIAG;

typedef struct {
  B2L_TOUCH_DIAG  Diag;
} B2L_TOUCH_DIAG_PROTOCOL;

#endif
