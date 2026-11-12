##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # Timer
  #
  USE_PHYSICAL_TIMER                         = 1

  #
  # KEP
  #
  HAS_CNTFRQ_EL0_EMPTY_ERRATA                = 0

!include SamsungPkg/SamsungPkg.dsc.inc

[LibraryClasses]
  PlatformUfsLib|S5E9820Pkg/Library/PlatformUfsLib/PlatformUfsLib.inf

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x2030323839453553
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x9820

  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x10101000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x10102000

  #
  # GPIO
  # List Contents: 0x10430000, 0x10830000, 0x13030000, 0x13C40000, 0x15580000, 0x15850000, 0x15C30000, 0x18C60000
  #
  gSamsungPkgTokenSpaceGuid.PcdGpioCtrlAddr|{ 0x00, 0x00, 0x43, 0x10, 0x00, 0x00, 0x83, 0x10, 0x00, 0x00, 0x03, 0x13, 0x00, 0x00, 0xC4, 0x13, 0x00, 0x00, 0x58, 0x15, 0x00, 0x00, 0x85, 0x15, 0x00, 0x00, 0xC3, 0x15, 0x00, 0x00, 0xC6, 0x18 }

  #
  # SPEEDY
  # List Contents: 0x15970000, 0x15980000
  #
  gSamsungPkgTokenSpaceGuid.PcdSpeedyBusAddr|{ 0x00, 0x00, 0x97, 0x15, 0x00, 0x00, 0x98, 0x15 }

  #
  # RAM Manager
  #
  gSamsungPkgTokenSpaceGuid.PcdHasSecurePgtbl|TRUE

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Exynos (TM) 9820 @ 2.73 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"S5E9820"

[LibraryClasses]
  #
  # SEC Libraries
  #
  PlatformSecLib|S5E9820Pkg/Library/PlatformSecLib/PlatformSecLib.inf

  #
  # UEFI Libraries
  #
  HwResetSystemLib|S5E9820Pkg/Library/ResetSystemLib/ResetSystemLib.inf
  ResetSystemLib|S5E9820Pkg/Library/ResetSystemLib/ResetSystemLib.inf

  #
  # IO Libraries
  #
  GpioBankLib|S5E9820Pkg/Library/GpioBankLib/GpioBankLib.inf
  HsI2cLib|S5E9820Pkg/Library/HsI2cLib/HsI2cLib.inf

  #
  # Power Management Libraries
  #
  PmicLib|S5E9820Pkg/Library/PmicLib/PmicLib.inf

  #
  # Samsung Libraries
  #
  UsiLib|S5E9820Pkg/Library/UsiLib/UsiLib.inf

  #
  # USB (device/gadget). SamsungPkg.dsc.inc varsayilan olarak Null verir;
  # burada Exynos 9820'e ozel DWC3 + combo PHY bring-up'i ile degistirilir.
  # Ayrinti ve adresler: Library/PlatformUsbfnLib/BoardInit.c
  #
  PlatformUsbfnLib|S5E9820Pkg/Library/PlatformUsbfnLib/PlatformUsbfnLib.inf
