/** @file
  B2LEl2Dxe - deneysel PSCI HVC koprusunu EL2'ye yukler.

  Tasarim ve gerekce B2LEl2Payload.S'nin basinda. Buradaki is uc adim:

    1) Onceki acilisin HVC kaydini kenara al, bu acilis icin temizle
    2) EL2 yukunu 0xCF000000'a kopyala
    3) smc #0 ile x0=0xC2000400 ... cagir, yuk EL2'de calissin

  FADT'de PSCI_USE_HVC biti acikken Windows'un PSCI cagrilari HVC ile EL2'ye
  gelir. Yuk, v0.2 sorgularini cevaplar ve stok EL3'te bulunan eski CPU
  cagrilarini SMC ile iletir. HCR_EL2.TSC KULLANILMAZ; normal EL1 SMC'leri
  bu surucunun disinda kalir.

  OKUMA (sonraki acilista, EFI Shell):
      dmem 0xCF030000 40

    42 32 48 56   'B2HV' imzasi
    +0x04         HVC sayisi
    +0x08         son PSCI fonksiyon no
    +0x10         son cagrinin x1, x2 degerleri

  SYSTEM_OFF veya SYSTEM_RESET gorulurse ilk kopru bunlari NOT_SUPPORTED
  doner; amac sessiz sifirlamayi yeniden uretmek degil, Windows'un cagrinin
  bu uyumsuz arayuze gercekten gelip gelmedigini gozlemlemektir.
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define EL2_BASE        0xCF000000ULL
#define EL2_INIT        0xCF000800ULL   // vektor tablosunun ardi
#define EL2_LOG         0xCF020000ULL   // bu acilis
#define EL2_LOG_PREV    0xCF030000ULL   // onceki acilis (Shell burayi okur)
#define EL2_LOG_SIZE    0x2000
#define EL2_DXE_DIAG    0x100            // EL2_LOG icinde, HVC kaydindan ayri

#define EL2_DXE_MAGIC   0x44483242U      // "B2HD" little-endian
#define EL2_DXE_GIRDI   1
#define EL2_DXE_DONDU   2

#define VMM_SMC_MAGIC   0xC2000400ULL
#define VMM_STACK_OFF   4096
#define VMM_AARCH64     1

extern UINT8 B2LEl2PayloadStart;
extern UINT8 B2LEl2PayloadEnd;

EFI_STATUS
EFIAPI
B2LEl2Entry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  ARM_SMC_ARGS  Args;
  UINTN         Boyut;
  VOID         *Kaynak;
  VOID         *Hedef;
  UINT32       *DxeTani;

  Kaynak = (VOID *)&B2LEl2PayloadStart;
  Boyut  = (UINTN)(&B2LEl2PayloadEnd - &B2LEl2PayloadStart);
  Hedef  = (VOID *)(UINTN)EL2_BASE;

  DEBUG ((EFI_D_ERROR, "B2L EL2: yuk %u bayt, hedef 0x%lx\n", (UINT32)Boyut, EL2_BASE));

  if ((Boyut == 0) || (Boyut > 0x8000)) {
    DEBUG ((EFI_D_ERROR, "B2L EL2: yuk boyutu mantiksiz, iptal\n"));
    return EFI_ABORTED;
  }

  //
  // 1) ONCEKI ACILISIN KAYDINI KENARA AL.
  //
  // Kayit DRAM'de ve sicak sifirlamayi atlatiyor (B2LResetHookDxe turunda
  // olculdu: sayac 0x403 -> 0x404). Ama bu acilista tampon temiz baslamali,
  // yoksa eski kayitlarin ustune yazariz. Kaydirmayi C tarafinda yapmak,
  // EL2 assembly'sinde yapmaktan cok daha ucuz.
  //
  CopyMem ((VOID *)(UINTN)EL2_LOG_PREV, (VOID *)(UINTN)EL2_LOG, EL2_LOG_SIZE);
  ZeroMem ((VOID *)(UINTN)EL2_LOG, EL2_LOG_SIZE);

  // HVC kaydi yalnizca EL2 yuku "B2HV" imzasini yazarsa gorunur. Bu ayri
  // kayit, DXE surucusunun calistigini ve VMM SMC'sinin donup donmedigini
  // sonraki acilista B2LGoster'a bildirir.
  DxeTani    = (UINT32 *)(UINTN)(EL2_LOG + EL2_DXE_DIAG);
  DxeTani[0] = EL2_DXE_MAGIC;
  DxeTani[1] = EL2_DXE_GIRDI;
  DxeTani[2] = (UINT32)Boyut;

  {
    UINT32 *Onceki = (UINT32 *)(UINTN)EL2_LOG_PREV;

    if (Onceki[0] == 0x56483242) {
      DEBUG ((EFI_D_ERROR, "B2L EL2: onceki acilista %u HVC, son PSCI=0x%x\n", Onceki[1], Onceki[2]));
    } else {
      DEBUG ((EFI_D_ERROR, "B2L EL2: onceki acilistan kayit yok\n"));
    }
  }

  //
  // 2) YUKU YERINE KOY.
  //
  // Bolge MemoryMapLib'de onbelleksiz; yine de temizlik ucuz ve EL2 MMU
  // kapali calisiyor - komutlarin gercekten DRAM'e inmis olmasi sart.
  //
  CopyMem (Hedef, Kaynak, Boyut);
  WriteBackDataCacheRange (Hedef, Boyut);
  InvalidateInstructionCacheRange (Hedef, Boyut);

  //
  // 3) ARKA KAPIYI CAGIR.
  //
  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = VMM_SMC_MAGIC;
  Args.Arg1 = EL2_INIT;         // EL2'de calisacak rutin
  Args.Arg2 = VMM_STACK_OFF;
  Args.Arg3 = VMM_AARCH64;
  Args.Arg4 = EL2_BASE;         // rutine x0 olarak gecer (kullanmiyoruz)
  Args.Arg5 = Boyut;            // rutine x1 olarak gecer (kullanmiyoruz)

  DEBUG ((EFI_D_ERROR, "B2L EL2: arka kapi cagriliyor (0x%lx)\n", VMM_SMC_MAGIC));
  ArmCallSmc (&Args);
  DEBUG ((EFI_D_ERROR, "B2L EL2: donus x0=0x%lx\n", (UINT64)Args.Arg0));

  DxeTani[1] = EL2_DXE_DONDU;
  DxeTani[3] = (UINT32)Args.Arg0;
  DxeTani[4] = (UINT32)Args.Arg1;

  //
  // Donus degerini yorumlamiyoruz: bu arayuzun 9820'deki tam semantigi
  // belgelenmemis ve yanlis yorumlayip erken pes etmek istemiyoruz.
  // Gercek testi sonraki acilista 0xCF030000'da imza olup olmadigi.
  //
  return EFI_SUCCESS;
}
