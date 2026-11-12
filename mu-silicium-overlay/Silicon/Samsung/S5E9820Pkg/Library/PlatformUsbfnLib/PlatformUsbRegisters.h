#ifndef _EXYNOS9820_USB_H_
#define _EXYNOS9820_USB_H_

//
// S10e (beyond0lte) / Exynos 9820 USBCON ve DWC3 yerlesimi.
//
// Kaynak: stok DTB Resources/DTBs/beyond0lte.dts
//   usb@10C00000  (dwc3, reg 0x10000)          -> LINK / ControllerBase
//   phy@10B00000  (combo PHY, reg 0x200)       -> USBCON
//   pmu_offset = 0x72C, pmu_offset_dp = 0x704
//
// USBCON register ofsetleri S5E7885Pkg ile aynidir (ayni USBDRD PHY nesli);
// S5E9830Pkg ofsetlerinden farki SSPPLLCTL (0x30) burada da mevcuttur.
//

//
// PMU
//
#define EXYNOS9820_PMU_BASE                 0x15860000
#define EXYNOS9820_PMU_USBDRD_PHY_CTRL      (EXYNOS9820_PMU_BASE + 0x072C)
#define EXYNOS9820_PMU_USBDP_PHY_CTRL       (EXYNOS9820_PMU_BASE + 0x0704)
#define EXYNOS9820_PMU_PHY_ENABLE           BIT0

//
// PHY ve Link tabanlari
//
#define EXYNOS9820_USBDRD_LINK_BASE         0x10C00000
#define EXYNOS9820_USBDRD_CTRL_BASE         0x10B00000
#define EXYNOS9820_USBDRD_CTRL_SIZE         0x1000
#define EXYNOS9820_USBDRD_LINK_SIZE         0x10000

//
// USBCON Registers, relative to EXYNOS9820_USBDRD_CTRL_BASE.
//
#define EXYNOS_USBCON_CTRL_VER              0x00
#define EXYNOS_USBCON_LINK_CTRL             0x04
#define EXYNOS_USBCON_LINK_PORT             0x08
#define EXYNOS_USBCON_CLKRST                0x20
#define EXYNOS_USBCON_PWR                   0x24
#define EXYNOS_USBCON_DUALPHYSEL            0x28
#define EXYNOS_USBCON_SSPPLLCTL             0x30
#define EXYNOS_USBCON_COMBO_PMA_CTRL        0x48
#define EXYNOS_USBCON_UTMI                  0x50
#define EXYNOS_USBCON_HSP                   0x54
#define EXYNOS_USBCON_HSP_TUNE              0x58
#define EXYNOS_USBCON_HSP_TEST              0x5C

//
// LINK_CTRL
//
#define LINKCTRL_PIPE3_FORCE_RX_ELEC_IDLE   BIT18
#define LINKCTRL_PIPE3_FORCE_PHY_STATUS     BIT17
#define LINKCTRL_PIPE3_FORCE_EN             BIT16
#define LINKCTRL_DIS_QACT_BUSPEND           BIT13
#define LINKCTRL_DIS_QACT_LINKGATE          BIT12
#define LINKCTRL_DIS_QACT_ID0               BIT11
#define LINKCTRL_DIS_QACT_VBUS_VALID        BIT10
#define LINKCTRL_DIS_QACT_BVALID            BIT9
#define LINKCTRL_FORCE_QACT                 BIT8
#define LINKCTRL_BUS_FILTER_BYPASS(x)       (((x) & 0xF) << 4)

//
// LINK_PORT
//
#define LINKPORT_HUB_PORT_SEL_OCD_U3        BIT3
#define LINKPORT_HUB_PORT_SEL_OCD_U2        BIT2
#define LINKPORT_HOST_NUM_U3_SHIFT          16
#define LINKPORT_HOST_NUM_U3_MASK           (0xFU << LINKPORT_HOST_NUM_U3_SHIFT)

//
// CLKRST
//
#define CLKRST_PHY20_SW_POR                 BIT13
#define CLKRST_PHY20_SW_POR_SEL             BIT12
#define CLKRST_PHY_SW_RST                   BIT3
#define CLKRST_PHY_RESET_SEL                BIT2
#define CLKRST_PORT_RST                     BIT1
#define CLKRST_LINK_SW_RST                  BIT0

//
// SSPPLLCTL
//
#define SSPPLLCTL_FSEL_MASK                 0x7
#define SSPPLLCTL_FSEL_50MHZ                7

//
// COMBO_PMA_CTRL (SECPMACTL)
//
#define SECPMACTL_PMA_LOW_PWR               BIT4

//
// UTMI
//
#define UTMI_FORCE_VBUSVALID                BIT5
#define UTMI_FORCE_BVALID                   BIT4
#define UTMI_DP_PULLDOWN                    BIT3
#define UTMI_DM_PULLDOWN                    BIT2
#define UTMI_FORCE_SUSPEND                  BIT1
#define UTMI_FORCE_SLEEP                    BIT0

//
// HSP
//
#define HSP_VBUSVLDEXTSEL                   BIT13
#define HSP_VBUSVLDEXT                      BIT12
#define HSP_EN_UTMISUSPEND                  BIT9
#define HSP_COMMONONN                       BIT8

//
// HSP_TUNE (HSPPARACON) - stok DTB'de tune dugumleri "disabled",
// o yuzden ilk turda kullanilmaz; degerler ileride gerekirse burada.
//
#define HSP_TUNE_TXVREF_MASK                (0xFU << 28)
#define HSP_TUNE_TXVREF(x)                  (((UINT32)(x) & 0xF) << 28)
#define HSP_TUNE_TXRES_MASK                 (0x3U << 21)
#define HSP_TUNE_TXRES(x)                   (((x) & 0x3) << 21)
#define HSP_TUNE_TXPREEMPA_MASK             (0x3U << 18)
#define HSP_TUNE_TXPREEMPA(x)               (((x) & 0x3) << 18)
#define HSP_TUNE_SQRX_MASK                  (0x7U << 8)
#define HSP_TUNE_SQRX(x)                    (((x) & 0x7) << 8)
#define HSP_TUNE_COMPDIS_MASK               (0x7U << 0)
#define HSP_TUNE_COMPDIS(x)                 (((x) & 0x7) << 0)

//
// HSP_TEST
//
#define HSP_TEST_SIDDQ                      BIT24
#define HSP_TEST_LINESTATE_MASK             (0x3U << 20)
#define HSP_TEST_LINESTATE(x)               (((x) & HSP_TEST_LINESTATE_MASK) >> 20)

#endif /* _EXYNOS9820_USB_H_ */
