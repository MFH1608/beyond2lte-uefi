# beyond2lte UEFI

Samsung Galaxy S10+ (SM-G975F, Exynos 9820) için UEFI firmware —
[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) portu.

Bu depo Mu-Silicium'un **fork'u değil**, üzerine kopyalanan bir *overlay*.
Upstream'i güncel tutup bizim dosyalarımızı üzerine yazıyoruz.

İkinci cihaz: **Galaxy S10e (SM-G970F, `beyond0lte`, 6/128)**. Aynı SoC
(Exynos 9820) ve aynı S-Boot ailesi; farklar bellek (6 GiB), panel
(1080×2280) ve DTB ile sınırlı. Derleme: `bash yap.sh -d beyond0lte`.
Ayrıntı ve doğrulanmamış noktalar: [`PORT_S10E.md`](PORT_S10E.md).

## Durum

| Özellik | Durum |
|---|---|
| UEFI boot | ✅ |
| Display (1440×3040) | ✅ |
| Yan tuşlar | ✅ |
| **Dahili depolama (UFS)** | ✅ 5 LUN, BlockIO kurulu |
| Boot Manager | ✅ |
| EFI Shell | ✅ FV'den açılıyor, `startup.nsh` çalışıyor |
| ACPI | ✅ `acpiview -r 2`: 3 hata, üçü de kasıtlı (MCFG/SPCR/DBG2) |
| PPTT (CPU topolojisi) | ✅ Linux doğru okuyor (3 küme) |
| PSCI / SMP | ✅ 8 çekirdek açılıyor (SMC konduiti) |
| DSDT UFS düğümü | ✅ numaralanıyor, `_HID` geçici |
| UEFI'den UFS'e **yazma** | ✅ çalışıyor (`WriteBlocks` Success, dizin girdisi oluşuyor) |
| EFI Shell `>a` yönlendirmesi | ❌ `Invalid Parameter` — dosya değil, kabuk sorunu |
| USB | ❌ |
| **Linux (ACPI, DTB yok)** | ✅ 8 CPU açılıyor, initramfs'e kadar |
| Windows Boot Manager | ✅ çalışıyor, ekran çiziyor, ESP'den okuyor |
| Windows çekirdeği (`ntoskrnl`) | ✅ başlıyor |
| Windows erken kullanıcı modu | ❌ ~12 sn sonra reset, tanı kanalı yok |

Ayrıntı: [`STATUS.md`](STATUS.md)

## Kullanım

Klasörü WSL'den erişilebilir bir yere koy, sonra:

```bash
bash yap.sh --clean          # DEBUG derle
bash yap.sh --tar            # Odin için .tar da üret
bash yap.sh --acpi           # ACPI tablolarını derle + etkinleştir
```

`yap.sh` overlay'i `~/Mu-Silicium`'a kopyalar, derler, çıktıyı Windows
masaüstüne atar ve FV kullanımını raporlar.

İlk kurulum ve flash adımları: [`BUILD.md`](BUILD.md)

## Klasörler

| Yol | İçerik |
|---|---|
| `mu-silicium-overlay/` | Mu-Silicium ağacına birebir kopyalanan dosyalar |
| `beyond2ltePkg/` | Platform kaynağı (overlay ile senkron tutuluyor) |
| `silicium-acpi-overlay/` | S5E9820 ACPI tabloları (`.dsl`) + derleme script'i |
| `dtb-analiz/` | Stock firmware'den çıkarılmış decompile edilmiş DTB'ler |

## Belgeler

| Dosya | İçerik |
|---|---|
| [`STATUS.md`](STATUS.md) | Ne çalışıyor, bulunan hatalar, sıradaki iş |
| [`HARDWARE.md`](HARDWARE.md) | Donanım haritası — register adresleri, DTB bulguları |
| [`UFS_DRIVER.md`](UFS_DRIVER.md) | UFS sürücü mimarisi ve CAL tabloları |
| [`PARTITIONS.md`](PARTITIONS.md) | Bölüm tablosu (PIT'ten çıkarıldı) |
| [`BUILD.md`](BUILD.md) | Derleme ve flash |
| [`PORT_S10E.md`](PORT_S10E.md) | S10e (`beyond0lte`) portu — farklar, doğrulanacaklar |
| [`ORTAM.md`](ORTAM.md) | Cihaz/bilgisayar referansı (projeden bağımsız) |

## Upstream'e gönderilecek düzeltmeler

1. **`UfsDxe.inf` eksik `TimerLib`** — `UfsDxe.c` `MicroSecondDelay` çağırıyor
   ama `.inf` `TimerLib`'i bildirmiyor. 9810/9830'da `PlatformUfsLib` dolaylı
   getirdiği için fark edilmemiş.
2. **`beyond1lte` memory map çakışması** — DXE Heap, Display Reserved bölgesini
   tamamen kapsıyor. Muhtemelen o portun "Inactive" olma sebebi.
3. **`S5E9820Pkg/Library/PlatformUfsLib`** — yeni, 9820 için UFS desteği.

## Notlar

`stockfirmware/` (7 GB) depoya dahil değil. Gerekirse SamMobile/Frija ile
`G975FXXSGHWC1` indirilir. `dtb-analiz/` ve `BEYOND2LTE_EUR_OPEN.pit` ondan
türetildi ve depoda duruyor.
