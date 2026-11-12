##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = beyond0lte
  # beyond2lte'nin GUID'inden AYRI olmali: ayni anda derlenirse Build/
  # altindaki ayni hedefi iki platform paylasir ve ikinci derleme birincinin
  # ciktisini ezer.
  PLATFORM_GUID                  = A7C4B0D2-9E31-4F58-B6A0-0D4E7C1B0F13
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/beyond0ltePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = beyond0ltePkg/beyond0lte.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include S5E9820Pkg/S5E9820Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # PcdSystemMemorySize BURADA EZILMIYOR - DENENDI, ETKISI OLMADI.
  #
  # 0xC0000000 (3 GB) yazildi ve cihazda olculdu: harita hala 7977 MB
  # gosteriyordu. Demek ki DRAM boyutu bu PCD'den degil platformun kendi
  # tespitinden geliyor. Sinirlama MemoryMapLib'e tasindi; gerekce ve
  # olcum orada.
  #
  # S10e (beyond0lte) 6/128: tespit 6 GiB okumali (S-Boot'un NS_IRAM+0x848
  # yazdigi deger; RamManagerLib okur). Ust bank sinirlari MemoryMapLib'de
  # buna gore: son bank 0x980000000'da BITIYOR, orada descriptor YOK.
  #

  #
  # UEFI Stack
  #
  # BootShim'in 4 MiB'lik FD kopyasi 0xC9000000..0xC9400000'i kaplar.
  # Stack bu araligin icinde olamaz; MemoryMapLib ile birlikte tasinir.
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xC9400000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Samsung"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Galaxy S10e"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"beyond0lte"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Galaxy_S10e_beyond0lte"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Galaxy S10e"

  #
  # Simple Frame Buffer
  #
  # S10e paneli: 5.8", 1080x2280 (19:9). Deger stock DTB'den:
  # /s6e3fa7_beyond0 { resolution = <0x438 0x8e8>; }  (0x438=1080,
  # 0x8e8=2280). 1440x3040 yazmak ekrani bozar; framebuffer
  # zaten sabit adreste ve DECON satir adimi genislige gore kurulu.
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2280
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Ekrandaki debug log'unun akis hizi (satir basina gecikme, mikrosaniye).
  # SiliciumPkg varsayilani 10000 (10 ms). Fotograflayabilmek icin yavaslatildi.
  # Fotograf cekmen bitince 10000'e geri al - yoksa boot cok uzun surer.
  #
  #
  # 0 = her DEBUG satiri icin bekleme yok. Varsayilan 100000 (100 ms/satir)
  # acilisi dakikalara cikariyor. Bu, geri alinan tek sey degil - bilerek
  # korunuyor cunku Windows'un gordugu hicbir seyi degistirmiyor.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|0
  #
  # SiliciumPkg.dsc.inc DEBUG build'de 0x8007EE0F veriyor; bu maskede
  # DEBUG_INFO (0x40) YOK. Bizim tani satirlarimiz ve UfsDxe'nin
  # "UFS Well known lun[%d] %llu MB" kapasite satiri EFI_D_INFO seviyesinde,
  # yani ekrana hic dusmuyordu. INFO bitini ekliyoruz.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F


  #
  # BDS'in otomatik olusturdugu "Internal UEFI Shell" boot secenegi bu GUID'i
  # ariyor (MsBootOptionsLib.c -> RegisterFvBootOption(PcdShellFile, ...)).
  # Varsayilani C57AD6B7-... ; biz FV'ye ShellPkg'in kendi GUID'iyle koydugumuz
  # icin PCD'yi ona esitliyoruz.
  #
  # 7C04A583-9E3E-4F1C-AD65-E05268D0B4D1 (gUefiShellFileGuid), little-endian bayt dizisi.
  # {GUID("...")} sozdizimi bu EDK2 surumunde sessizce yanlis sonuc verebiliyor;
  # PcBdsPkg.dec varsayilaniyla ayni formati kullaniyoruz.
  gPcBdsPkgTokenSpaceGuid.PcdShellFile|{ 0x83, 0xA5, 0x04, 0x7C, 0x3E, 0x9E, 0x1C, 0x4F, 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }


[LibraryClasses]
  #
  # UEFI Shell
  #
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
  AcpiViewCommandLib|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf

  #
  # Memory Libraries
  #
  MemoryMapLib|beyond0ltePkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|beyond0ltePkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # Boot manager. Varsayilan (SiliciumNullPkg) "no OS" aninda cihazi kapatir.
  # Bizimki yerine S10e donanim self-test ekranini cizer: klavye/ESP olmadan
  # dogrulama icin. Yalnizca beyond0lte'ye bagli; beyond2lte etkilenmez.
  #
  DeviceBootManagerLib|beyond0ltePkg/Library/PlatformBootManagerLib/B2LPlatformBootManagerLib.inf

[Components]
  #
  # UEFI Shell (firmware volume'a gomulu - USB ya da ESP gerektirmez)
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      # acpiview: ACPI tablolarini cihazda listeler ve spec'e karsi dogrular.
      # Windows denemeden once tablolarimizi test etmenin en dogrudan yolu.
      NULL|ShellPkg/Library/UefiShellAcpiViewCommandLib/UefiShellAcpiViewCommandLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
      gEfiMdePkgTokenSpaceGuid.PcdUefiLibMaxPrintBufferSize|8000
  }

  #
  # EbsTest - ExitBootServices sonrasi kalp atisi.
  #
  # SADECE DERLENIYOR, FDF'ye EKLENMIYOR: firmware'e gomulmedigi icin
  # normal acilis yolunu hic etkilemiyor. Uretilen .efi out/ altina
  # kopyalaniyor; test etmek isteyince esp/ icine elle konuyor,
  # startup.nsh varsa calistiriyor, yoksa Windows'a geciyor.
  #
  beyond0ltePkg/Applications/EbsTest/EbsTest.inf

  #
  # B2LGoster - tani kayitlarini buyuk harflerle cizer. EbsTest gibi FDF'ye
  # girmiyor; out/ altina cikip esp/ icine elle konuyor.
  #
  beyond0ltePkg/Applications/B2LGoster/B2LGoster.inf
  # B2LBootTani - bootmgfw'yi ayni FAT biriminden baslatip yalnizca GOP ve
  # UEFI olaylariyla canlilik/ExitBootServices siniri gosteren uygulama.
  # Fiziksel tani kayitlarina veya ayrilmis RAM'e erismez.
  beyond0ltePkg/Applications/B2LBootTani/B2LBootTani.inf
  #
  # B2LTool - GPT dokumu, MISC BCB ile TWRP'ye giris, imaj yazma ve menu.
  # FDF'ye de eklenir (DXE.inc): BDS onu boot option olarak ve
  # UnableToBoot icinden baslatir.
  #
  beyond0ltePkg/Applications/B2LTool/B2LTool.inf
  beyond0ltePkg/Drivers/B2LRtHookDxe/B2LRtHookDxe.inf
  beyond0ltePkg/Drivers/B2LSehDxe/B2LSehDxe.inf
  beyond0ltePkg/Drivers/B2LGicSusDxe/B2LGicSusDxe.inf

  #
  # Deneysel PSCI HVC koprusu (bkz. Drivers/B2LEl2Dxe/B2LEl2Payload.S)
  #
  # Varsayilan derlemede kapali. "bash yap.sh --psci-hvc" hem bu satiri hem
  # DXE.inc'teki INF'i gecici Mu-Silicium kopyasinda acar; sonraki normal
  # derleme overlay'i yeniden kopyalayip ikisini de kapatir.
  #
  # beyond0ltePkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf

  #
  # Input
  #
  beyond0ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # USB device-mode sinif surucusu (M2a: enumerasyon). UsbfnDwc3Dxe'nin
  # yayinladigi EFI_USBFN_IO_PROTOCOL uzerinde durur; ekrandaki "USB.ser"
  # satirindan izlenir (bkz. Drivers/B2LUsbSerialDxe).
  #
  beyond0ltePkg/Drivers/B2LUsbSerialDxe/B2LUsbSerialDxe.inf
