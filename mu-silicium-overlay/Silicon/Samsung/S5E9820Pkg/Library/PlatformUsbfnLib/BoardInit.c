#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationHelperLib.h>
#include <Library/PlatformUsbfnLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Device/Dwc3.h>

#include "PlatformUsbRegisters.h"

//
// Stok DTB: snps,quirk-frame-length-adjustment = <0x20>
//
#define EXYNOS9820_FLADJ_30MHZ          0x20

//
// ================= MapMemoryRegion, neden burada =================
//
// S-Boot USB MMIO sayfalarini (USBCON 0x10B00000 ve DWC3 0x10C00000) UEFI
// memory map'ine KOYMAZ. Ilk USB probe denemesi bu yuzden ilk okumada Data
// abort ile coktu (ESR 0x96000006, FAR 0x10B00000). Bu yuzden herhangi bir
// register'a dokunmadan once iki bolge EfiMemoryMappedIO olarak haritalanir.
// Iki bolge de zaten haritaliysa MapMemoryRegion EFI_ALREADY_STARTED /
// EFI_ACCESS_DENIED doner; bu "kullanilabilir" demektir, hatayi yutmuyoruz.
//
STATIC
VOID
UsbMapRegions (
  VOID
  )
{
  EFI_STATUS Status;

  Status = MapMemoryRegion (
             EXYNOS9820_USBDRD_CTRL_BASE,
             EXYNOS9820_USBDRD_CTRL_SIZE,
             EfiMemoryMappedIO
             );
  if (EFI_ERROR (Status) &&
      (Status != EFI_ALREADY_STARTED) &&
      (Status != EFI_ACCESS_DENIED))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Usbfn9820: USBCON MMIO map failed: %r\n",
      Status
      ));
  }

  Status = MapMemoryRegion (
             EXYNOS9820_USBDRD_LINK_BASE,
             EXYNOS9820_USBDRD_LINK_SIZE,
             EfiMemoryMappedIO
             );
  if (EFI_ERROR (Status) &&
      (Status != EFI_ALREADY_STARTED) &&
      (Status != EFI_ACCESS_DENIED))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Usbfn9820: DWC3 MMIO map failed: %r\n",
      Status
      ));
  }
}

STATIC
VOID
UsbPhyIsolation (
  IN BOOLEAN  Isolate
  )
{
  UINT32 Register;

  Register = MmioRead32 (EXYNOS9820_PMU_USBDRD_PHY_CTRL);

  if (Isolate) {
    Register &= ~EXYNOS9820_PMU_PHY_ENABLE;
  } else {
    Register |= EXYNOS9820_PMU_PHY_ENABLE;
  }

  MmioWrite32 (EXYNOS9820_PMU_USBDRD_PHY_CTRL, Register);
}

STATIC
VOID
UsbPhyPipe3Override (
  VOID
  )
{
  UINT32 Register;

  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_LINK_CTRL);
  Register &= ~LINKCTRL_PIPE3_FORCE_PHY_STATUS;
  Register |= LINKCTRL_PIPE3_FORCE_EN | LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE;
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_LINK_CTRL, Register);

  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_COMBO_PMA_CTRL);
  Register |= SECPMACTL_PMA_LOW_PWR;
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_COMBO_PMA_CTRL, Register);
}

EFI_STATUS
UsbfnPlatformGetData (
  OUT USBFN_PLATFORM_DATA  *PlatformData
  )
{
  if (PlatformData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // DWC3 controller (Global Register Block Offset 0xC100).
  //
  PlatformData->ControllerBase = EXYNOS9820_USBDRD_LINK_BASE;

  //
  // Ilk tur USB 2.0 High Speed: SuperSpeed PHY'yi devre disi birak ki
  // combo PHY'nin USB3 tarafi (SS PLL, PIPE3) getirilmeden de link
  // kurabilelim. SuperSpeed ayri bir adimda acilir.
  //
  PlatformData->MaximumSpeed          = UsbBusSpeedHigh;
  PlatformData->SuperSpeedPhyDisabled = TRUE;

  PlatformData->UtmiWidth = UsbfnUtmiWidth16Bit;

  //
  // DTB: snps,dis-u2-freeclk-exists-quirk -> UTMI+ PHY'de serbest
  // kosan saat YOK.
  //
  PlatformData->Usb2FreeClockExists = FALSE;

  PlatformData->FrameLengthAdjustMask  = DWC3_GFLADJ_30MHZ_MASK;
  PlatformData->FrameLengthAdjustValue = EXYNOS9820_FLADJ_30MHZ;

  PlatformData->NumEndpoints = 0;

  PlatformData->VendorId  = 0x04E8;
  PlatformData->ProductId = 0x2143;

  return EFI_SUCCESS;
}

EFI_STATUS
UsbfnPlatformPhyInit (
  VOID
  )
{
  UINT32 Register;
  UINT32 SsPorts;

  //
  // Bu noktaya kadar HICBIR register'a dokunulmadi; once MMIO'yu haritala
  // (yukaridaki aciklamaya bakin). Boylece Data abort imkansiz.
  //
  UsbMapRegions ();

  //
  // USBCON blok ayakta mi (saat/power domain)? S-Boot bunu acik
  // biraktigi icin okunabilir olmali.
  //
  Register = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CTRL_VER);
  if ((Register == 0xFFFFFFFF) || (Register == 0)) {
    DEBUG ((
      DEBUG_ERROR,
      "Usbfn9820: USBCON not reachable (CTRL_VER = 0x%08x)\n",
      Register
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // HWACG'yi kapat.
  //
  MmioOr32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_LINK_CTRL, LINKCTRL_FORCE_QACT);

  Register = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_LINK_PORT);
  SsPorts  = (Register & LINKPORT_HOST_NUM_U3_MASK) >> LINKPORT_HOST_NUM_U3_SHIFT;

  //
  // PHY resetini baslat.
  //
  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST);
  Register |= CLKRST_PHY_SW_RST;
  if (SsPorts != 0) {
    Register |= CLKRST_PHY20_SW_POR | CLKRST_PHY20_SW_POR_SEL | CLKRST_PHY_RESET_SEL;
  }

  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST, Register);

  //
  // UTMI+ ac: suspend/sleep ve D+/D- pull-down kapali.
  //
  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_UTMI);
  Register &= ~(UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_UTMI, Register);

  MmioOr32 (
    EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP,
    HSP_EN_UTMISUSPEND | HSP_COMMONONN
    );

  //
  // Filtreleri bypass et; VBUS/BVALID'i S-Boot yokmus gibi zorla
  // (cihazda harici VBUS pad'i yok, bkz. DTB is_not_vbus_pad = <0x01>).
  //
  MmioOr32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_LINK_CTRL, LINKCTRL_BUS_FILTER_BYPASS (0xF));
  MmioOr32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_UTMI,      UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
  MmioOr32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP,       HSP_VBUSVLDEXT | HSP_VBUSVLDEXTSEL);

  //
  // PHY analog bloklarini besle (SIDDQ'yu temizle).
  //
  MmioAnd32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP_TEST, ~HSP_TEST_SIDDQ);

  //
  // PHY resetinin oturmasini bekle.
  //
  gBS->Stall (10);

  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST);
  Register &= ~(CLKRST_PHY_SW_RST | CLKRST_PORT_RST);
  if (SsPorts != 0) {
    Register |= CLKRST_PHY20_SW_POR_SEL;
    Register &= ~CLKRST_PHY20_SW_POR;
  }

  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST, Register);

  gBS->Stall (75);

  if (SsPorts != 0) {
    UsbPhyPipe3Override ();
  }

  //
  // PHY izolasyonunu kaldir (en son).
  //
  UsbPhyIsolation (FALSE);

  return EFI_SUCCESS;
}

EFI_STATUS
UsbfnPlatformPhyExit (
  VOID
  )
{
  UINT32 Register;

  //
  // VBUS/BVALID zorlamasini birak, PHY'yi uykuya al.
  //
  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_UTMI);
  Register &= ~(UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
  Register |= UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP;
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_UTMI, Register);

  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP);
  Register &= ~(HSP_VBUSVLDEXT | HSP_VBUSVLDEXTSEL);
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP, Register);

  //
  // Analog bloklari kapat ve link resetle.
  //
  MmioOr32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP_TEST, HSP_TEST_SIDDQ);

  Register  = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST);
  Register |= CLKRST_LINK_SW_RST;
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST, Register);

  gBS->Stall (10);

  Register &= ~CLKRST_LINK_SW_RST;
  MmioWrite32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_CLKRST, Register);

  UsbPhyIsolation (TRUE);

  return EFI_SUCCESS;
}

BOOLEAN
UsbfnPlatformIsVbusPresent (
  VOID
  )
{
  UINT32 Register;

  //
  // Gercek VBUS pad'i yok; LINESTATE'e bak (7885/9830 ile ayni yaklasim).
  //
  Register = MmioRead32 (EXYNOS9820_USBDRD_CTRL_BASE + EXYNOS_USBCON_HSP_TEST);

  return (BOOLEAN)(HSP_TEST_LINESTATE (Register) != 0);
}
