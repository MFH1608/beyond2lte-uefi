#ifndef B2L_HSI2C_H
#define B2L_HSI2C_H

#include <Uefi.h>

//
// HSI2C10'e baglanir ve denetleyiciyi (GPIO mux, USI modu, zamanlama)
// baslatir. Basarisizsa EFI_STATUS kodunu dondurur; ardindan
// B2LHsI2cWriteRead EFI_NOT_READY doner.
//
EFI_STATUS
B2LHsI2cInitialize (
  VOID
  );

//
// sec_ts komut + (istege bagli) veri transferi. WriteBuffer[0] komut
// baytidir; ReadLength != 0 ise komutu yazip ayni adresten okur.
//
EFI_STATUS
B2LHsI2cWriteRead (
  IN  CONST UINT8  *WriteBuffer OPTIONAL,
  IN  UINTN         WriteLength,
  OUT UINT8        *ReadBuffer OPTIONAL,
  IN  UINTN         ReadLength
  );

//
// Son basarisiz transferden sonra denetleyicin ham durum registerlarini
// okur (INT_STATUS, TRANS_STATUS). NACK / zaman asimi / master durumunu
// ayirt etmek icin.
//
VOID
B2LHsI2cSnapshot (
  OUT UINT32  *IntStatus,
  OUT UINT32  *TransStatus
  );

//
// 0x08..0x77 araligini tarar; ACK veren her adresin bitini ScanAck'te
// isaretler (bit j = adres j). 0x48'in yanit verip vermedigini gormek icin.
//
EFI_STATUS
B2LHsI2cScan (
  OUT UINT32  ScanAck[4]
  );

#endif
