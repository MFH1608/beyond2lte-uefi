/** @file
  beyond0lte'ye ozgu dokunma teshis protokolu.

  B2LTouchDxe bu protokolu yayinlar; BDS'in self-test ekrani okur. Amac:
  cihazda harf ureten klavye/seri konsol olmadigi icin dokunma sorununu
  TEK flash'ta ayirt edebilmek. Sayaclar ve son olay baytlari dogrudan
  ekrana basilir.

  GUID iki modulde de yerel sabit olarak tanimlanir (INF'e protocol
  eklemeye gerek kalmaz).
**/

#ifndef B2L_TOUCH_DIAG_H_
#define B2L_TOUCH_DIAG_H_

#include <Uefi.h>

#define B2L_TOUCH_DIAG_PROTOCOL_GUID \
  { 0x9c1f4b52, 0x8d36, 0x4c01, { 0xa7, 0x62, 0x51, 0x0e, 0x9b, 0x3d, 0x77, 0xc4 } }

#define B2L_TOUCH_DIAG_EVENT_SIZE  8

typedef struct {
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
  UINT8   Reserved[3];
} B2L_TOUCH_DIAG;

typedef struct {
  B2L_TOUCH_DIAG  Diag;
} B2L_TOUCH_DIAG_PROTOCOL;

#endif
