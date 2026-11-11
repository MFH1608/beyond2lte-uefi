  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  #
  # B2LEl2Dxe varsayilan derlemeden CIKARILDI. Kaynagi duruyor, normal
  # Windows yoluna hic girmez.
  #
  # "bash yap.sh --psci-hvc" bu tek INF satirini Mu-Silicium kopyasinda
  # acar. Yeni yuk HCR_EL2.TSC ile her SMC'yi tuzaklamaz; FADT'nin HVC
  # konduitiyle gelen PSCI cagrilarini dar bir EL2 koprusunde isler.
  #
  # ESKI SMC TUZAGI NEDEN CIKTI: kendi "B2L2" imzasini hicbir zaman
  # uretmedi, biz de "calismiyor, zarari yok" diye birakmistik. BU VARSAYIM
  # YANLISTI. TrustZone'a EL2'de kod calistirmasini soylemek asla no-op degil:
  #
  #   - Yuk kabul edildiyse EL2'de YARIM CALISAN bir hipervizor oturuyordu.
  #   - HCR_EL2.TSC acik demek EL1'den gelen HER SMC'nin vektore dusmesi.
  #   - Vektorumuz beklenmedik bir durumda donerse sistem kilitleniyordu.
  #
  # OLCUM BUNU DESTEKLIYOR: eski denemede cihaz SIFIRLAMADI, DONDU. Bu yuzden
  # HVC kopyasi da varsayilan degildir ve ilk turda yalnizca kaydini kanitlar.
  #
  # Bellek haritasindaki "B2L EL2" bolgesi YERINDE BIRAKILDI: kod
  # yuklenmedigi icin calismiyor, ama ayrilmis RAM olarak duruyor ve
  # Windows surucusunun olum sonrasi halka tamponu icin zaten oraya
  # yazilacak (bkz. WINDOWS_SURUCU_PLANI.md, Faz 1b).
  #
  # INF beyond0ltePkg/Drivers/B2LEl2Dxe/B2LEl2Dxe.inf

  #
  # Calisma zamani servisi kancasi. GetTime ve GetVariable sarmalanip
  # giris/cikis sayaci 0xCF040000'a yaziliyor. Gerekce B2LRtHookDxe.c
  # basinda: memmap "MMIO: 0 Pages" gosteriyor, yani hicbir aygit bolgemiz
  # RUNTIME isaretli degil ve SetVirtualAddressMap sonrasi MMIO'ya dokunan
  # bir calisma zamani surucusu bosluga yaziyor.
  #
  #
  # ================= ALETSIZ TABAN OLCUMU =================
  #
  # Uc tani surucusu de GECICI OLARAK cikarildi. Sebebi bir deney:
  # aylardir her olcumu KENDI KODUMUZ ICERIDEYKEN aldik ve bunlarin
  # sifirlamaya katkisi hic sinanmadi.
  #
  #   B2LRtHookDxe  - Windows'un cagirdigi GetVariable/SetVariable/GetTime/
  #                   ResetSystem'i sarmaliyor, gRT tablosunu degistirip
  #                   CRC'sini yeniden hesapliyor. Windows bu kancalardan
  #                   365 kez geciyor.
  #   B2LSehDxe     - EL3'e istisna isleyicisi kaydediyor, PSCI SMC'leri
  #                   yapiyor, SetVirtualAddressMap'te yeniden kaydediyor.
  #   B2LGicSusDxe  - devir aninda butun SPI'lari kapatiyor.
  #
  # Sayaclar saglikli gorunuyor ama saglikli gormek masum olmak degil.
  # Ustelik ilk gozlem hicbir aracimiz yokken ~15 saniyeydi, simdi ~10 -
  # aradaki farki hic aciklamadik.
  #
  # GERI ALMAK: asagidaki uc satirin basindaki # isaretlerini kaldir.
  #
  # INF beyond0ltePkg/Drivers/B2LRtHookDxe/B2LRtHookDxe.inf

  #
  # Samsung EL3 monitorunun Smart Exception Handler kancasi.
  #
  # EL3 beklenmedik bir istisna alinca cekirdegin kaydettigi adrese atliyor
  # (SMC_CMD_SET_SEH_ADDRESS = -123). Kayitli degilse atlayacak yeri yok ve
  # sifirliyor - sessizce. Windows bu SMC'yi asla yapmaz; biz yapiyoruz.
  #
  # Gerekce ve delil zinciri: EL3_SEH_PLANI.md
  #
  # INF beyond0ltePkg/Drivers/B2LSehDxe/B2LSehDxe.inf

  #
  # ExitBootServices aninda GIC'teki butun SPI'lari susturuyor.
  #
  # Cihazda olculdu: UEFI kesmeleri hic acmadigi halde sekiz SPI bekliyordu
  # ve dordu ekran hattindan (decon/dsim/dpp) geliyordu. S-Boot o bloklari
  # acilis logosu icin calisir birakiyor, UEFI susturmuyor, Windows'un ise
  # surucusu yok. Gerekce B2LGicSusDxe.c basinda.
  #
  # INF beyond0ltePkg/Drivers/B2LGicSusDxe/B2LGicSusDxe.inf

  INF SamsungPkg/Drivers/ChipInfoDxe/ChipInfoDxe.inf
  INF SamsungPkg/Drivers/GpioDxe/GpioDxe.inf
  INF SamsungPkg/Drivers/UsiDxe/UsiDxe.inf
  INF SamsungPkg/Drivers/SpeedyDxe/SpeedyDxe.inf
  INF SamsungPkg/Drivers/HsI2cDxe/HsI2cDxe.inf
  INF beyond0ltePkg/Drivers/B2LTouchDxe/B2LTouchDxe.inf
  INF SamsungPkg/Drivers/PmicDxe/PmicDxe.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf

  INF SamsungPkg/Drivers/UfsDxe/UfsDxe.inf

  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  INF FatPkg/EnhancedFatDxe/Fat.inf

  INF ShellPkg/Application/Shell/Shell.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 0
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif
  INF SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  INF SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf

  INF SamsungPkg/Drivers/RngDxe/RngDxe.inf

  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
