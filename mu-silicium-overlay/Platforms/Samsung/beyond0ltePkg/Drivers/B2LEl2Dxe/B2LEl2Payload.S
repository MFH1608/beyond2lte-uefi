//
// B2LEl2Payload.S - EL2'de calisan PSCI HVC koprusu.
//
// ===========================================================================
// NEDEN
// ===========================================================================
// S-Boot yalnizca PSCI 0.1 sunuyor; Windows'a verilen FADT ise PSCI
// kullanimini beyan ediyor. PSCI 0.2'nin zorunlu VERSION, AFFINITY_INFO,
// SYSTEM_OFF ve SYSTEM_RESET cagrilari stok EL3'te yok. Bu yuk, FADT'nin
// "Must use HVC for PSCI" bitiyle birlikte Windows'un PSCI cagrilarini
// EL2'ye yonlendirir. Yalnizca "bash yap.sh --psci-hvc" bunu firmware'e
// koyar; varsayilan derleme EL2'ye hic dokunmaz.
//
// Eski deney HCR_EL2.TSC ile EL1'den gelen HER SMC'yi yakaliyordu. Bu,
// tanisiz bir denemede sistemi dondurdu ve normal SMC'lere araci olmamasi
// gereken bir yol acti. Burada TSC BILEREK ayarlanmiyor: sadece Windows'un
// FADT nedeniyle yaptigi HVC'ler gelir. Legacy CPU_SUSPEND/CPU_OFF/CPU_ON
// SMC'ye iletilir; v0.2 sorgulari EL2'de cevaplanir.
//
// ===========================================================================
// EL2'YE NASIL CIKIYORUZ
// ===========================================================================
// S-Boot cekirdegi EL1'de birakiyor, ama TrustZone'da EL2'ye kod yukleyen bir
// arka kapi var. Samsung kendi cekirdeginde RKP hipervizorunu bununla
// yukluyor (init/vmm.c). Arayuz sleirsgoevy/exynos-kvm-patch'te belgelenmis:
//
//   x0 = 0xC2000400   VMM_64BIT_SMC_CALL_MAGIC
//   x1 = entry_point  EL2'de calisacak rutinin FIZIKSEL adresi
//   x2 = 4096         VMM_STACK_OFFSET
//   x3 = 1            VMM_MODE_AARCH64
//   x4, x5            rutine x0/x1 olarak gecer
//   smc #0
//
// Rutin EL2'de calisir ve "smc #0" ile x0=0xC2000401, x1=status diyerek
// doner. status SIFIR OLMAK ZORUNDA - degilse firmware HYP durumunu
// varsayilana sifirliyor ve sonraki HVC'ler istisna verir.
//
// UC KISIT (hepsi belgelenmis davranis):
//   1) Init yalnizca acilis cekirdeginde calisir. Digerleri onun EL2
//      durumunu KOPYALAR; tek fark sp = bootcore.sp + 4096 * cekirdek_no.
//      Bu yuzden yigin bolgemiz sekiz cekirdeklik ayrilmis durumda.
//   2) Firmware vbar_el2'yi cikista kaydedip rastgele noktalarda GERI
//      YUKLUYOR. Yani vektor adresi sonradan degistirilemez - ama bizim
//      koydugumuz yerde kalir, ki bize gereken tam olarak bu.
//   3) EL2'de MMU kapali: butun adresler fiziksel. Kod ve tampon sabit
//      fiziksel adreslerde, hicbir haritalama gerekmiyor.
//
// ===========================================================================
// YERLESIM  (MemoryMapLib'de "B2L EL2", 0xCF000000, 256 KB, Reserved)
// ===========================================================================
//   0xCF000000  istisna vektoru (2 KB, VBAR_EL2 2 KB hizali olmak zorunda)
//   0xCF000800  init rutini      <- SMC'ye entry_point olarak bu verilir
//   0xCF010000  acilis cekirdegi yigin tepesi; cekirdek N -> +4096*N
//   0xCF020000  BU acilisin HVC kaydi
//   0xCF030000  ONCEKI acilisin kaydi (DXE surucusu kopyalar, Shell okur)
//
// Vektor tablosu dosyanin BASINDA: 0xCF000000 zaten 2 KB hizali oldugu icin
// hizalama numarasina gerek kalmiyor ve yuk tek parca kopyalanabiliyor.
//
// ===========================================================================
// KAYIT BICIMI  (0xCF020000)
// ===========================================================================
//   +0x00  u32 magic 'B2HV' -> dmem'de "42 32 48 56"
//   +0x04  u32 HVC sayisi
//   +0x08  u32 son PSCI fonksiyon numarasi
//   +0x0C  u32 son istisna sinifi (normal HVC icin 0x16)
//   +0x10  u64 son x1
//   +0x18  u64 son x2
//
// Magic, VMM girisi yukumuzu gercekten EL2'de calistirdiginda yazilir.
// Bu sayede bos bir kayit "HVC hic kullanilmadi" ile "arka kapi calismadi"
// durumlarini ayirir.
//

#define EL2_VECT        0xCF000000
#define EL2_STACK_TOP   0xCF010000
#define EL2_LOG         0xCF020000
#define EL2_LOG_MAGIC   0x56483242      // "B2HV" little-endian

#define PSCI_VERSION        0x84000000
#define PSCI_CPU_SUSPEND    0xC4000001
#define PSCI_CPU_OFF        0x84000002
#define PSCI_CPU_ON         0xC4000003
#define PSCI_AFFINITY_INFO  0xC4000004
#define PSCI_MIGRATE_INFO   0x84000006
#define PSCI_SYSTEM_OFF     0x84000008
#define PSCI_SYSTEM_RESET   0x84000009

#define VMM_RETURN      0xC2000401

//
// 64 bitlik sabiti movz/movk ile kur. Literal havuzu KULLANMIYORUZ: yuk
// baska bir adrese kopyalaniyor ve konumdan bagimsiz olmasi gerekiyor.
//
.macro MOV64 reg, val
  movz \reg, #(((\val) >> 48) & 0xFFFF), lsl #48
  movk \reg, #(((\val) >> 32) & 0xFFFF), lsl #32
  movk \reg, #(((\val) >> 16) & 0xFFFF), lsl #16
  movk \reg, #((\val) & 0xFFFF)
.endm

  .section .text.b2lel2, "ax"
  .align 11                      // 2 KB: VBAR_EL2 hizalama sarti

  .global ASM_PFX(B2LEl2PayloadStart)
  .global ASM_PFX(B2LEl2PayloadEnd)
  .global ASM_PFX(B2LEl2InitOffset)

ASM_PFX(B2LEl2PayloadStart):

// ---------------------------------------------------------------------------
// EL2 istisna vektoru - 16 giris, her biri 128 bayt
// ---------------------------------------------------------------------------
// Yalnizca "Lower EL, AArch64, Synchronous" (0x400) bizi ilgilendiriyor;
// FADT HVC biti nedeniyle EL1'den gelen HVC oraya duser. HCR_EL2.TSC kapali
// oldugu icin normal SMC'ler EL3'e dogrudan gider ve buraya dusmez.
// ---------------------------------------------------------------------------
  .align 7
cur_el_sp0_sync:      eret
  .align 7
cur_el_sp0_irq:       eret
  .align 7
cur_el_sp0_fiq:       eret
  .align 7
cur_el_sp0_serr:      eret

  .align 7
cur_el_spx_sync:      eret
  .align 7
cur_el_spx_irq:       eret
  .align 7
cur_el_spx_fiq:       eret
  .align 7
cur_el_spx_serr:      eret

  .align 7
low_el_a64_sync:      b   b2l_sync_handler
  .align 7
low_el_a64_irq:       eret
  .align 7
low_el_a64_fiq:       eret
  .align 7
low_el_a64_serr:      eret

  .align 7
low_el_a32_sync:      eret
  .align 7
low_el_a32_irq:       eret
  .align 7
low_el_a32_fiq:       eret
  .align 7
low_el_a32_serr:      eret

// ---------------------------------------------------------------------------
// Init rutini - vektor tablosunun hemen ardinda, yani +0x800
// ---------------------------------------------------------------------------
  .align 11
ASM_PFX(B2LEl2InitOffset):
b2l_el2_init:
  //
  // Kendi yiginimiz. Firmware cikistaki sp'yi "bootcore.sp" olarak kaydedip
  // diger cekirdeklere +4096*N veriyor, o yuzden burada dogru degeri
  // kurmak sekiz cekirdegin tamamini belirliyor.
  //
  MOV64 x9, EL2_STACK_TOP
  mov   sp, x9

  // Istisna vektoru
  MOV64 x9, EL2_VECT
  msr   vbar_el2, x9
  isb

  //
  // HVC koprusunun kendi calistigini isaretle. Bu, VMM arka kapisinin
  // basariyla bize atladiginin kaniti; HVC sayisi sifir olsa bile sonraki
  // acilista 0xCF030000'da gorunur.
  //
  MOV64 x9, EL2_LOG
  MOV64 x10, EL2_LOG_MAGIC
  str   w10, [x9]
  str   wzr, [x9, #4]
  str   wzr, [x9, #8]
  str   wzr, [x9, #12]
  str   xzr, [x9, #16]
  str   xzr, [x9, #24]

  //
  // Firmware'e don. status SIFIR olmak zorunda; degilse HYP durumu
  // varsayilana geri aliniyor ve butun bu is bosa gidiyor.
  //
  MOV64 x0, VMM_RETURN
  mov   x1, xzr
  smc   #0

  // Buraya normalde hic gelinmez.
1:
  wfe
  b     1b

// ---------------------------------------------------------------------------
// Tuzaklanan HVC
// ---------------------------------------------------------------------------
b2l_sync_handler:
  //
  // x0..x3 PSCI'nin argumanlari ve donusu, yani onlara dokunabiliriz.
  // BASKA HICBIR REGISTER BOZULMAMALI - misafir isletim sistemi calismaya
  // devam edecek. Kazidiklarimizi yigina alip geri koyuyoruz.
  //
  stp   x4, x5, [sp, #-32]!
  stp   x6, x7, [sp, #16]

  mrs   x4, esr_el2
  lsr   x4, x4, #26
  cmp   x4, #0x16                 // EC = 0x16: HVC, AArch64
  b.ne  b2l_not_supported

  MOV64 x4, EL2_LOG
  ldr   w5, [x4, #4]              // kayit sayisi
  add   w6, w5, #1
  str   w6, [x4, #4]
  str   w0, [x4, #8]
  mov   w6, #0x16
  str   w6, [x4, #12]
  str   x1, [x4, #16]
  str   x2, [x4, #24]

  // PSCI v0.2: zorunlu sorgulari EL2'de tamamla.
  MOV64 x4, PSCI_VERSION
  cmp   w0, w4
  b.eq  b2l_psci_version

  MOV64 x4, PSCI_AFFINITY_INFO
  cmp   w0, w4
  b.eq  b2l_psci_affinity

  // Samsung EL3'te ayni standart kimliklerle var olan uc eski islev.
  MOV64 x4, PSCI_CPU_SUSPEND
  cmp   w0, w4
  b.eq  b2l_forward_smc
  MOV64 x4, PSCI_CPU_OFF
  cmp   w0, w4
  b.eq  b2l_forward_smc
  MOV64 x4, PSCI_CPU_ON
  cmp   w0, w4
  b.eq  b2l_forward_smc

  // Trusted OS gocu yok: PSCI'nin tanimladigi "MP veya yok" cevabi.
  MOV64 x4, PSCI_MIGRATE_INFO
  cmp   w0, w4
  b.eq  b2l_psci_migrate_info

  // SYSTEM_OFF/SYSTEM_RESET v0.2'de zorunlu ama stok EL3'te yok. Bu ilk
  // deneyde sifirlama talebini tekrarlamak yerine NOT_SUPPORTED donuyoruz;
  // kayittaki son fonksiyon, istegin gercekten buraya gelip gelmedigini
  // sonraki acilista gosterir. Donus, calisan bir kabugun yerine gecen
  // kalici "basari" degildir.
  MOV64 x4, PSCI_SYSTEM_OFF
  cmp   w0, w4
  b.eq  b2l_not_supported
  MOV64 x4, PSCI_SYSTEM_RESET
  cmp   w0, w4
  b.eq  b2l_not_supported

  b     b2l_not_supported

b2l_psci_version:
  mov   x0, #2                     // PSCI v0.2
  b     b2l_return

b2l_psci_affinity:
  cbz   x1, b2l_affinity_cpu0
  mov   x0, #1                     // diger tum MPIDR'ler OFF
  b     b2l_return
b2l_affinity_cpu0:
  mov   x0, #0                     // CPU0 ON
  b     b2l_return

b2l_psci_migrate_info:
  mov   x0, #2                     // MP capable veya Trusted OS yok
  b     b2l_return

b2l_forward_smc:
  ldp   x6, x7, [sp, #16]
  ldp   x4, x5, [sp], #32

  //
  // Eski PSCI cagrilarini EL3'e ilet. HCR_EL2.TSC ayarlanmadigi icin EL2'den
  // yapilan bu SMC geri kendimize dusmez. x0..x3 hala cagiranin degerleri,
  // donuste de sonuclar onlarda kaliyor.
  //
  // SMC yalnizca x0..x3'u kullanmali, ama ELR'yi ilerletirken x4/x5'teki
  // misafir degerleri de aynen geri vermek icin onlar korunur.
  //
  stp   x4, x5, [sp, #-16]!
  smc   #0
  ldp   x4, x5, [sp], #16
  b     b2l_advance_elr

b2l_not_supported:
  movn  w0, #0                     // PSCI_NOT_SUPPORTED = int32(-1)

b2l_return:
  ldp   x6, x7, [sp, #16]
  ldp   x4, x5, [sp], #32

  //
  // Tuzaklanan HVC'de ELR_EL2 HVC KOMUTUNUN KENDISINI gosterir; ilerletmek
  // bize dusuyor, yoksa ayni komuta donup sonsuz tuzaga gireriz.
  //
b2l_advance_elr:
  stp   x4, x5, [sp, #-16]!
  mrs   x4, elr_el2
  add   x4, x4, #4
  msr   elr_el2, x4
  ldp   x4, x5, [sp], #16
  eret

ASM_PFX(B2LEl2PayloadEnd):
  .align 3
