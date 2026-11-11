# beyond2lte UEFI — derleme ve flash

Bu adımlar Mu-Silicium'un gerçek `build_uefi.py` script'ine göre doğrulandı.
İş bölümü ORTAM.md'ye uygun: **derleme WSL'de, flash Windows'ta** (WSL'de USB yok).

---

## 1. WSL — kaynağı al

```bash
cd ~
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
```

`--recursive` **şart** — `Mu_Basecore`, `Common/Mu`, `Silicium-ACPI` submodule.
Toplam ~5 GB. WSL'in kendi ext4'ünde olsun, `/mnt/c/` üzerinde **değil**.

## 2. Bağımlılıklar

```bash
./setup_env.sh -p apt
python3 -m pip install --upgrade -r pip-requirements.txt
```

`setup_env.sh` clang/llvm dahil her şeyi apt'ten kuruyor. Elle LLVM indirmene gerek yok.

## 3. beyond2lte dosyalarını içeri kopyala

Bu klasördeki `mu-silicium-overlay/`, Mu-Silicium'un dizin yapısını birebir
yansıtıyor. Tek komutla yerleşiyor:

```bash
cp -r /mnt/c/Users/orhun/OneDrive/Masaüstü/UEFI_beyond2lte/mu-silicium-overlay/* ~/Mu-Silicium/
```

Yerleşen üç şey:

| Dosya | Ne |
|---|---|
| `Platforms/Samsung/beyond2ltePkg/` | Platform tanımı (dsc/fdf/memory map/keypad) |
| `Resources/Configs/beyond2lte.toml` | FD adresi, boot image ayarları |
| `Resources/DTBs/beyond2lte.dtb` | Cihaz DTB'si — boot image'a ekleniyor |
| `Silicon/Samsung/SamsungPkg/Drivers/UfsDxe/UfsDxe.inf` | **Upstream dosyanın üzerine yazıyor** — aşağıya bak |

> **UfsDxe.inf neden değişti:** `UfsDxe.c` `<Library/TimerLib.h>` include edip
> `MicroSecondDelay` çağırıyor, ama `.inf` `TimerLib`'i `[LibraryClasses]`'ta
> bildirmiyor. Bu upstream hatası. 9830/9810'da fark edilmiyor çünkü onların
> `PlatformUfsLib`'i `TimerLib`'i dolaylı getiriyor; 9820'de o kütüphane yok,
> Null'a düşüyoruz ve link `undefined symbol: MicroSecondDelay` ile patlıyor.
> Tek satır ekledik. Upstream'e PR açmaya değer.
>
> Mu-Silicium'u güncellersen (`-u`) bu dosya geri gelebilir — overlay'i
> yeniden kopyala.

> `beyond2lte.dtb`'yi kendi stock firmware'inden ürettim: `dt.img` base DTB'si +
> `dtbo.img` rev 26 overlay'i `fdtoverlay` ile birleştirildi, `model` özelliği eklendi.
> Yani sboot'un beklediği gerçek beyond2lte tanımı.

Merkezi bir cihaz listesi **yok** — `build_uefi.py` `Platforms/` altında
`<isim>Pkg` klasörünü glob'layıp `Resources/Configs/<isim>.toml`'u okuyor. Kayıt gerekmez.

Overlay artık **iki cihaz** taşıyor: `beyond2lte` (S10+) ve `beyond0lte`
(S10e). Cihaz seçimi `yap.sh -d <isim>` ile yapılır; `build_uefi.py`'ye
doğrudan `-d beyond0lte` verilebilir. Farklar ve doğrulanacak noktalar:
[`PORT_S10E.md`](PORT_S10E.md).

## 4. Derle

```bash
cd ~/Mu-Silicium
python3 build_uefi.py -d beyond2lte
```

Faydalı bayraklar:

| Bayrak | Ne işe yarar |
|---|---|
| `-r DEBUG` | Debug build (daha çok log, daha büyük) |
| `-c` | Temiz build — eski çıktıları siler |
| `-u` | Derlemeden önce repoyu günceller |

**İlk denemede `-r DEBUG` kullanma.** Önce RELEASE ile derlenip derlenmediğini gör;
derleme hatalarıyla çalışma zamanı hatalarını aynı anda kovalamak istemezsin.

Çıktı: **`Mu-beyond2lte.img`** — hazır Android boot image. Script `mkbootimg`'i
kendisi çalıştırıyor, DTB'yi kendisi ekliyor. Elle paketleme yok.

## 5. Windows'a taşı

```bash
cp Mu-beyond2lte.img /mnt/c/Users/orhun/Desktop/
```

## 6. Flash — önce oku

**Bu adımda cihaz bricklenebilir.** Devam etmeden önce:

- [ ] Stock firmware elimizde (`stockfirmware/`, G975FXXSGHWC1) — Odin ile geri dönüş açık
- [ ] LineageOS 23.2 build'inin zip'i elinde — dirty flash ile geri dönersin
- [ ] Download Mode'a girebildiğini test ettin: cihaz kapalıyken **Bixby + Ses Kısma + USB**
- [ ] Recovery: **Bixby + Ses Açma + USB**

UEFI `boot` partition'ına yazılıyor, `sboot`'a **değil**. Samsung'un asıl
bootloader'ı el değmeden kalıyor, yani Download Mode her hâlükârda erişilebilir
olmalı. Riski büyük ölçüde azaltan şey bu — ama sıfırlamıyor.

Odin için tar'la (PowerShell'de, WSL'de tar'layıp kopyalamak daha kolay):

```bash
cd ~/Mu-Silicium
cp Mu-beyond2lte.img boot.img
tar -H ustar -cf uefi-boot.tar boot.img
cp uefi-boot.tar /mnt/c/Users/orhun/Desktop/
```

Windows'ta Odin → **AP** slotuna `uefi-boot.tar` → Start.
(Re-Partition **işaretli olmasın**.)

## 7. Ne bekliyoruz

Cihaz açıldığında ekranda Mu-Silicium logosu / UEFI ekranı. Bu kadar.

Çalışmayacak olanlar ve nedenleri `HARDWARE.md` ile `UFS_DRIVER.md`'de:
depolama (UFS sürücüsü yok), USB, OS boot.

Ses tuşlarının menüde çalışması iyi işaret — `KeypadDeviceLib`'in GPIO
tanımlarının doğru olduğunu gösterir.

## 8. Geri dönüş

Ekran siyah kalırsa ya da bootloop olursa:

1. **Bixby + Ses Kısma + USB** ile Download Mode
2. Odin → AP slotuna stock `AP_G975FXXSGHWC1_*.tar.md5`
   (sadece boot'u geri istiyorsan LineageOS zip'ini recovery'den sideload et)

`pstore/ramoops` burada **işe yaramaz** — o kernel'in halka tamponu, UEFI aşamasında
kernel yok. UEFI hata ayıklaması için ekran çıktısı ya da UART gerekiyor.

---

## Beklenen takılma noktaları

Bunlar tahmin edilebilir; çıkarsa hata metnini olduğu gibi gönder.

- **`Silicium-ACPI` boş** → `--recursive` unutulmuş.
  `git submodule update --init --recursive` ile düzelir.
- **`stuart_setup` / `edk2toolext` hatası** → pip paketleri eski.
  `python3 -m pip install --upgrade -r pip-requirements.txt`
- **Disk dolu** → clone 5 GB + build birkaç GB. `df -h ~` ile bak.
- **`ACPI.inc` ile ilgili hata** → beyond1lte'de o dosyanın tamamı yorum satırı,
  bizimki de öyle. Normal, ACPI henüz devrede değil.
