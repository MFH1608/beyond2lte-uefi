#ifndef B2L_HSI2C_H
#define B2L_HSI2C_H

#include <Uefi.h>

EFI_STATUS
B2LHsI2cInitialize (
  VOID
  );

EFI_STATUS
B2LHsI2cWriteRead (
  IN  CONST UINT8  *WriteBuffer OPTIONAL,
  IN  UINTN         WriteLength,
  OUT UINT8        *ReadBuffer OPTIONAL,
  IN  UINTN         ReadLength
  );

#endif
