#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  //
  // NOT: beyond1lte'de "DXE Heap" (0xC9240000 + 60 MB = 0xCCE40000) "Display
  // Reserved" bolgesini (0xCA000000 - 0xCB400000) tamamen iceriyordu. DXE heap
  // framebuffer'in uzerine tahsis yapar. Heap'i Display'in ardina tasidik.
  // Sonraki DTB rezervi modem_spare 0xD0000000 -> 16 MB bosluk kaliyor.
  //
  //
  // Samsung U-H / EL2 - S-Boot'un imzali U-H calisma alani.
  //
  // Stok G975FXXSGHWC1 BL, imzali UH bolumunu burada yukleyip
  // x0=0xC2000400, x1=0x87001000, x2=0x2000, x3=1,
  // x4=0x87000000, x5=0x100000 ile EL2'ye giriyor. UH ELF'i 0x87000000'a
  // bagli; 0x8701b800'e VBAR_EL2 kuruyor, veri alanini ve 0x87046000'e
  // kadar olan EL2 yigini kullaniyor. BOOT bolumune UEFI yazmak UH
  // bolumunu degistirmez. U-H'nin custom BOOT yolunda etkin olup olmadigi
  // S-Boot cagrı kosulu ve cihazdaki UHCFG ile ayri olarak dogrulanacak.
  //
  // S10e'de bu adresler AYNI S-Boot ailesinden geliyor; UH bolumu ve
  // UHCFG PIT'te ayni adlarla duruyor. Yine de S10e'nin kendi PIT'inde
  // UH boyutunu dogrula (PARTITIONS.md beyond2lte icin 2 MiB diyor).
  //
  // BsCode yanlisti: ExitBootServices sonrasinda Windows bu sayfalari
  // normal tahsis havuzuna geri katabilir. U-H'nin kodunu/verisini ezmek
  // sessiz EL2 reseti icin sinanabilir bir adaydi. Koruma uygulaninca RAM
  // 0x1F29'dan 0x1F27'ye indi ama Windows reseti surdu; yani bu ezilme ana
  // neden degil. Buna ragmen etkinlesirse calisacak EL2 sayfalarini Windows'a
  // vermek yanlis olacagindan rezerv korunur. Stok yukleyici x5'te 1 MiB
  // ister; mevcut 2 MiB rezervi eksiksiz koruyoruz.
  //
  // KORUYUCU WinPE TESTI: Windows'a yalniz 0x900000000..0x980000000 banki
  // birakilir (2 GiB). Stok DTB'nin secure video penceresi
  // 0x40000000..0xF0000000, ABOX penceresi 0xC0000000..0xE0000000'dir; ikisi
  // de dusuk banktadir. Stok Linux dmesg'i genel CMA'nin son ust bankta
  // yerlesebildigini gosterir. Linux'un kendi ayirmasi S-Boot'tan sonra
  // calisir; bu, o CMA alaninin UEFI'de mutlaka etkin oldugunun kaniti
  // DEGILDIR. Yine de WinPE 2 GiB ile calisabildigi icin, belirsizligin
  // riskini almamak adina UEFI'nin zorunlu parcalari disinda dusuk, ilk ust
  // ve son ust banklar bu turda Reserved tutulur.
  {"Lower dynamic guard",0x80000000, 0x07000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"Samsung U-H EL2",    0x87000000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"Lower dynamic guard",0x87200000, 0x41E00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  // BootShim, Resources/Configs/beyond0lte.toml'daki FD_SIZE kadar veriyi
  // 0xC9000000'a kopyalar. FD_SIZE=4 MiB iken eski 2 MiB tanim ve
  // 0xC9200000 stack'i FD'nin ikinci yarisini Windows'a vermis ve stack'i
  // calisan firmware'in ustune koymustu. FD ve stack kesinlikle ayrik kalmali.
  {"UEFI FD",            0xC9000000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack",         0xC9400000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Lower dynamic guard",0xC9440000, 0x00BC0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"Display Reserved",   0xCA000000, 0x01400000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},
  {"DXE Heap",           0xCB400000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  //
  // B2L EL2 - EL2 yuku, yigin ve SMC kaydi. 256 KB.
  //
  // NEREDE: DXE Heap 0xCF000000'da bitiyor, DTB'nin bir sonraki rezervi
  // modem_spare 0xD0000000'da basliyor. Aradaki 16 MB'lik boslugun basi;
  // ne UEFI ne Android burayi kullaniyor.
  //
  // Reserv SART, RtData DEGIL: EL2'de MMU kapali, adresler fiziksel. Windows'un
  // bu bolgeyi haritalamasina gerek yok - sadece DOKUNMAMASI gerekiyor.
  //
  // UNCACHED_UNBUFFERED SART: EL2 kodu onbellege ugramadan calisiyor ve
  // kaydi sifirlamadan hemen once yaziyor; veri onbellekte kalirsa kaybolur.
  //
  // Ic yerlesim (B2LEl2Payload.S):
  //   0xCF000000  istisna vektoru (VBAR_EL2, 2 KB hizali)
  //   0xCF000800  init rutini
  //   0xCF010000  yigin tepesi; cekirdek N -> +4096*N
  //   0xCF020000  bu acilisin SMC kaydi
  //   0xCF030000  onceki acilisin kaydi  <- Shell burayi okur
  //
  {"B2L EL2",            0xCF000000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},

  //
  // B2L RT LOG - CALISMA ZAMANI KANCASININ KAYDI
  //
  // "Reserv" DEGIL "RtData" OLMAK ZORUNDA, VE BU BIR BUGCHECK'E MAL OLDU.
  //
  // Ilk surumde kayit 0xCF040000'daydi ama bolge Reserv (EfiReservedMemoryType)
  // isaretliydi. SetVirtualAddressMap'ten sonra Reserv bolgeler SANAL HARITAYA
  // HIC GIRMIYOR; dolayisiyla EfiConvertPointer isaretciyi ceviremedi ve kanca
  // ilk cagrida fiziksel adrese yazmaya calisti. Sonuc:
  //
  //     SYSTEM_THREAD_EXCEPTION_NOT_HANDLED (0x7E)
  //
  // RtData (EfiRuntimeServicesData) bolgeler sanal haritaya giriyor ve
  // EfiConvertPointer onlari cevirebiliyor. EFI Shell tarafindan FIZIKSEL
  // adresten okumak yine calisiyor - bellegin kendisi ayni yerde.
  //
  // WRITE_BACK yeterli: bu bolgeye EL2 kodu degil, normal cekirdek modu
  // kodu yaziyor ve sicak sifirlamada icerik zaten kaliyor.
  //
  {"B2L RT Log",         0xCF040000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK},
  {"Lower dynamic guard",0xCF050000, 0x30FB0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},

  // 0x880000000..0x900000000, DTB'deki sec_rdx_bootdev'i de icerir.
  {"Upper bank guard", 0x880000000ULL, 0x080000000ULL, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},

  // 0x900000000..0x980000000 Windows/WinPE icin birakilan tek DRAM bankidir.

  // ------------------------------------------------------------------
  // S10e 6 GiB: 0x980000000'DAN SONRASI YOK.
  //
  // beyond2lte'de (8 GiB) burada 0x980000000..0xA00000000 ayri bir banka
  // vardi ve Reserved tutuluyordu. S10e 6/128'de toplam DRAM 6 GiB:
  // RamManagerLib MemoryEnd'i 0x800000000 + 6 GiB = 0x980000000 yapar
  // (S-Boot'un NS_IRAM+0x848'e yazdigi boyut okunur). Bu adresin ustunu
  // tarif eden descriptor EKLENMEZ; var olmayan DRAM'i "Reserved" diye
  // haritalamak UEFI'ye hayalet bir bolge verir.
  //
  // Kontrol: 0x80000000..0x100000000 (2 GiB dusuk) +
  //          0x880000000..0x980000000 (4 GiB ust) = 6 GiB.
  // ------------------------------------------------------------------

  //
  // ==========================================================================
  // STOCK DTB'DEKI reserved-memory BOLGELERI
  // ==========================================================================
  //
  // NEDEN EKLENDI - AYLARDIR ARADIGIMIZ IMZAYA BIREBIR UYUYOR
  //
  // Windows cekirdegi ~10 saniye SAGLIKLI calisiyor (kalp atisi son ana kadar
  // duzenli), sonra TEMIZ bir yazilim sifirlamasi geliyor. Bugcheck YOK
  // (dogrulanmis geri cagri), kullanici moduna hic cikilmiyor, ve sure
  // depolama hizindan, menu beklemesinden, startnet icerignden, watchdog'dan,
  // LPS0'dan ve ErrorControl'den BAGIMSIZ.
  //
  // Cekirdek init'inde bir sey ters giderse Windows BUGCHECK verir. Sessiz,
  // temiz bir sifirlama icin isletim sisteminin DISINDA birinin karar vermesi
  // gerekiyor.
  //
  // Samsung SoC'lerinde tam olarak boyle bir mekanizma var: normal dunyadan
  // KORUMALI bir bellek bolgesine erisim, guvenli monitorun cihazi
  // sifirlamasiyla sonuclaniyor. Isletim sistemine haber verilmiyor.
  //
  // Zamanlama da uyuyor: Windows'un tahsis sirasi deterministik, yani o
  // bolgeye ne zaman dokunacagi her acilista ayni - hep 9-10 saniye.
  //
  // Ve Linux'un neden calistigini acikliyor: DTB'deki reserved-memory
  // dugumlerini okuyup bu bolgelerden uzak duruyor. Bizim haritamizda ise
  // hicbiri yoktu, dolayisiyla Windows hepsini bos RAM saniyordu.
  //
  // BAS SUPHELI: "exynos,seclog" 0xC0000000. Adindan belli - GUVENLI GUNLUK,
  // TrustZone'un. Ustelik dusuk bir adres, yani Windows erken tahsis ediyor.
  //
  // Kaynak: stock DTB, /reserved-memory dugumu
  // (Resources/DTBs/beyond0lte.dts). Sabit "reg" adresi olan HER girdi
  // burada EfiReservedMemoryType olarak bulunmali: Android/Linux bu dugumu
  // kendisi okur, Windows ise okumaz. "ion,recyclable" kamera alani bile
  // generic Windows ayiricisi icin kullanilabilir RAM degildir. Dusuk banki
  // kaplayan Lower dynamic guard, bu listenin o banktaki tum girdilerini de
  // kapsar; ayri descriptor koymak bindirmeye yol acar.
  //
  // "size"/"alloc-ranges" ile tanimlanan ion_video_stream ve abox-rmem
  // sabit fiziksel adres tasimaz; fakat olasi pencerelerinin tamami dusuk
  // guard tarafinda kalir. ion_crypto'nun alloc-ranges'i yoktur; bu nedenle
  // onun yeri statik olarak kanitlanamaz. Ortadaki bankin ayri tutulmasi,
  // kanitlanmis bir S-Boot rezervi degil, WinPE icin secilmis koruyucu
  // bellek butcesidir.
  // tools_reserved_memory_audit.py, sabit DTB listesi ile bu tablonun
  // eksiksizligini derleme oncesi zorunlu olarak denetler.
  //
  //
  // Ayrik DRAM banklari (S10e 6 GiB):
  //   0x80000000    .. 0x100000000    (dusuk, 2 GiB)
  //   0x880000000   .. 0x900000000    (ilk ust, 2 GiB - Reserved)
  //   0x900000000   .. 0x980000000    (Windows, 2 GiB)
  // 0x980000000 ve ustu YOK. beyond2lte'de dortuncu bank oradaydi.

  // iRAM Regions
  {"IRAM",               0x02020000, 0x00018000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   DEVICE},
  {"NS_IRAM",            0x02038000, 0x00004000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

  // Register Regions
  {"Chip Info",          0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

  //
  // DONANIM WATCHDOG'LARI - SADECE OKUMAK ICIN HARITALANIYOR
  //
  // Stock DTB'de iki tane var ve ikisini de hic incelemedik:
  //     watchdog_cl0@10050000  samsung,exynos9-v1-wdt  timeout-sec = 30
  //     watchdog_cl2@10060000  samsung,exynos9-v2-wdt  timeout-sec = 30
  //
  // Watchdog hipotezini daha once CIKARIMLA elemistik: EbsTest 60 saniye
  // yasadi, ve EDK2'nin YAZILIM watchdog'unu etkisizlestirmek bir sey
  // degistirmedi. Ama bunlar DONANIM watchdog'lari, kume basina birer tane,
  // ve haritada olmadiklari icin ne UEFI ne Windows onlara dokunuyor.
  //
  // Cikarim muhtemelen dogru - acik olsalar UEFI de sifirlanirdi. Ama bu
  // proje sunu ogretti: CIKARIMLA ELEME, OLCUMLE ELEME DEGILDIR. Ve okumak
  // tek flas.
  //
  // Exynos WDT yazmaclari:
  //     +0x00 WTCON   bit5 = watchdog enable, bit0 = reset enable
  //     +0x04 WTDAT   yeniden yukleme degeri
  //     +0x08 WTCNT   anlik sayac
  //
  // WTCON'da bit5 set ise watchdog CALISIYOR demektir ve eleme yanlisti.
  //
  {"WDT CL0",            0x10050000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WDT CL2",            0x10060000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICD",               0x10101000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GICC",               0x10102000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMU",                0x15860000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Decon",              0x19030000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Clock Controller",   0x1A240000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

  // UFS bloklari. beyond1lte'de yoktu; eksikligi 0x13D6xxxx erisiminde
  // "Data abort: Translation fault" veriyor.
  //
  // GPIO controller'lari BURAYA EKLENMEZ: GpioDxe onlari kendi
  // MapMemoryRegion() ile map ediyor. Onceden eklersek cakisir ve
  // "Failed to Map GPIO Controller / start failed: Access Denied" alirsin.
  {"UFS UniPro",         0x13D50000, 0x00008000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS HCI",            0x13D60000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS IO Coherency",   0x13C10000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
