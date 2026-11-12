#!/usr/bin/env python3
"""
UsbfnDwc3Dxe.c'ye interrupt uc nokta destegi ekler (CDC-ACM icin).

    python3 tools_usbfn_intr_patch.py ~/Mu-Silicium

NEDEN YAMA, NEDEN KOPYA DEGIL:
UsbfnDwc3Dxe.c ~1000 satir upstream dosyasi. Tamamini overlay'e kopyalarsak
upstream duzeltmelerini kaybederiz (bkz. tools_ufs_patch.py basindaki ayni
gerekce). Burada yalnizca dokundugumuz satirlar var; imza tutmazsa script
DURUYOR, sessizce eski davranisla devam etmiyor.

SORUN:
`Dwc3ConfigureEnableEndpoints` Bulk disindaki her uc nokta tipini
EFI_UNSUPPORTED ile reddediyor. CDC-ACM'in iletisim (comm) arayuzunde
bildirim uc noktasi interrupt'tir. Linux cdc_acm `acm_probe()` kontrol
arayuzunde en az bir uc nokta sart kosar (yoksa -EINVAL, /dev/ttyACM*
olusmaz); bu yuzden interrupt bildirim uc noktasi ZORUNLU.

BU YAMA NE YAPIYOR:
Yalnizca tip eslemesine interrupt'i ekler. Kurulum dongusunun cok-arayuzlu
hale getirilmesi cagiran surucude cozulur: B2LUsbSerialDxe, donanima
kurulacak uc noktalari tek bir "donanim konfig" arayuzu altinda toplar
(bkz. B2LUsbSerialDxe.c). Boylece paylasilan surucude degisiklik en aza iner.

Bu yalnizca DWC3 donanim uc noktalarini kurar; descriptor'lara dokunmaz.
"""
import sys, os

MU = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Mu-Silicium")
SRC = os.path.join(MU, "Silicon/Samsung/SamsungPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Dxe.c")

if not os.path.isfile(SRC):
    sys.exit(f"UsbfnDwc3Dxe.c bulunamadi: {SRC}")

t = open(SRC).read()

if "B2L_USBFN_INTR_PATCH" in t:
    print("  UsbfnDwc3Dxe intr yamasi zaten uygulanmis, atlaniyor")
    sys.exit(0)

# --- 1) Tip kontrolune interrupt dalini ekle (mevcut DEBUG'a dokunmadan) ----
old = "    if ((EpDesc->Attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_BULK) {"
new = """    if ((EpDesc->Attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT) {
      /* B2L_USBFN_INTR_PATCH: interrupt (CDC bildirim uc noktasi). */
    } else if ((EpDesc->Attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_BULK) {"""
if old not in t:
    sys.exit("HATA: tip kontrol blogu bulunamadi (1)")
t = t.replace(old, new, 1)

# --- 2) EpType eslemesi: interrupt -> DWC3_DEPCMD_TYPE_INTR -----------------
old = "    EpType    = DWC3_DEPCMD_TYPE_BULK;"
new = """    /* B2L_USBFN_INTR_PATCH: tipe gore DWC3 uc nokta tipi. */
    EpType    = ((EpDesc->Attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT)
                  ? DWC3_DEPCMD_TYPE_INTR
                  : DWC3_DEPCMD_TYPE_BULK;"""
if old not in t:
    sys.exit("HATA: EpType blogu bulunamadi (2)")
t = t.replace(old, new, 1)

open(SRC, "w").write(t)
print("  UsbfnDwc3Dxe intr yamasi uygulandi (2 bolge)")
