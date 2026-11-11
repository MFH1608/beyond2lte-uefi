/**
 * Exynos 9820 (S5E9820) UFS CAL.
 *
 * Iskelet: Mu-Silicium S5E9830Pkg/Library/PlatformUfsLib/UfsCal.c
 * Tablolar: LineageOS exynos9820 kernel, drivers/scsi/ufs/ufs-cal-9820.c
 *
 * Fonksiyon govdeleri 9830 portundan degistirilmeden alindi; yalnizca
 * 26 kalibrasyon tablosunun icerigi 9820 ile degistirildi. Tablo satir
 * formati iki tarafta ayni oldugu icin satirlar birebir tasindi.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 **/

#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UfsCalAdapterLib.h>

#include "UfsCal.h"

static struct UfsCalParam *ufs_cal[NUM_OF_UFS_HOST];

static const struct UfsCalPhyCfg init_cfg_evt0[] = {
 {0x44, 0x00, PMD_ALL, UNIPRO_DBG_PRD, BRD_ALL},

 {0x200, 0x40, PMD_ALL, PHY_PCS_COMN, BRD_ALL},
 {0x12, 0x00, PMD_ALL, PHY_PCS_RX_PRD_ROUND_OFF, BRD_ALL},
 {0xAA, 0x00, PMD_ALL, PHY_PCS_TX_PRD_ROUND_OFF, BRD_ALL},
 {0xA9, 0x02, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0xAB, 0x00, PMD_ALL, PHY_PCS_TX_LR_PRD, BRD_ALL},
 {0x11, 0x00, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x1B, 0x00, PMD_ALL, PHY_PCS_RX_LR_PRD, BRD_ALL},
 {0x2F, 0x79, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x76, 0x03, PMD_ALL, PHY_PCS_RX, BRD_ZEBU},
 {0x84, 0x01, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x04, 0x01, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0x25, 0xF6, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x200, 0x0, PMD_ALL, PHY_PCS_COMN, BRD_ALL},

 {0x155E, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3000, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3001, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4021, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4020, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},

 {0x6B0, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x584, 0x44, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x6E4, 0x10, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x644, 0x05, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x5A4, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x708, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x6D8, 0x40, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x708, 0x25, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x430, 0x04, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x628, 0x13, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x67C, 0x19, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x688, 0x30, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x00, 0xC8, PMD_ALL, COMMON_WAIT, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg init_cfg_evt1[] = {
 {0x44, 0x00, PMD_ALL, UNIPRO_DBG_PRD, BRD_ALL},

 {0x200, 0x40, PMD_ALL, PHY_PCS_COMN, BRD_ALL},
 {0x12, 0x00, PMD_ALL, PHY_PCS_RX_PRD_ROUND_OFF, BRD_ALL},
 {0xAA, 0x00, PMD_ALL, PHY_PCS_TX_PRD_ROUND_OFF, BRD_ALL},
 {0xA9, 0x02, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0xAB, 0x00, PMD_ALL, PHY_PCS_TX_LR_PRD, BRD_ALL},
 {0x11, 0x00, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x1B, 0x00, PMD_ALL, PHY_PCS_RX_LR_PRD, BRD_ALL},
 {0x2F, 0x79, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x76, 0x03, PMD_ALL, PHY_PCS_RX, BRD_ZEBU},
 {0x84, 0x01, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x04, 0x01, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0x25, 0xF6, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x200, 0x0, PMD_ALL, PHY_PCS_COMN, BRD_ALL},

 {0x155E, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3000, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3001, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4021, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4020, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},

 {0x6B0, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x584, 0x44, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x6E4, 0x10, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x644, 0x05, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x5A4, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},
 {0x708, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ZEBU},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x6D8, 0x40, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x708, 0x25, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x430, 0x04, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x628, 0x13, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x67C, 0x19, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x688, 0x30, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x5A0, 0x40, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5A4, 0x38, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x6E4, 0x18, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x00, 0xC8, PMD_ALL, COMMON_WAIT, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg init_cfg_card[] = {
 {0x9514, 0x00, PMD_ALL, UNIPRO_DBG_PRD, BRD_ALL},

 {0x200, 0x40, PMD_ALL, PHY_PCS_COMN, BRD_ALL},
 {0x12, 0x00, PMD_ALL, PHY_PCS_RX_PRD, BRD_ALL},
 {0xAA, 0x00, PMD_ALL, PHY_PCS_TX_PRD, BRD_ALL},
 {0x5C, 0x38, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x0F, 0x0, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x65, 0x01, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x69, 0x1, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x21, 0x0, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x22, 0x0, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x84, 0x1, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x04, 0x1, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0x8F, 0x3E, PMD_ALL, PHY_PCS_TX, BRD_ALL},
 {0x200, 0x0, PMD_ALL, PHY_PCS_COMN, BRD_ALL},

 {0x9536, 0x4E20, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x9564, 0x2e820183, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x155E, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3000, 0x0, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x3001, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4021, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x4020, 0x1, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},

 {0x8C, 0x80, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x74, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL^BRD_ZEBU},
 {0x110, 0xB5, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x43, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x16C, 0x20, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x178, 0xC0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x1B0, 0x18, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0xE0, 0x12, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x164, 0x58, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x8C, 0xC0, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x8C, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_init_cfg_evt0_g3[] = {
 {0x15D2, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},
 {0x15D3, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},

 {0x9529, 0x01, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x15A4, 0x3E8, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x9529, 0x00, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},

 {0xA006, 0x80000000, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x00, 0x7D0, PMD_ALL, COMMON_WAIT, BRD_ALL},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x6D8, 0xC0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x404, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x408, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x410, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x414, 0x10, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xDC, 0x28, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x44C, 0x80, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4BC, 0x12, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4C4, 0x12, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4CC, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4D4, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x9C, 0x11, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xA0, 0x11, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x10C, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x464, 0x33, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x468, 0x37, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x46C, 0x31, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x470, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x604, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x608, 0x82, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x60C, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x610, 0x98, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x618, 0x60, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x620, 0x70, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x54C, 0xFC, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x474, 0x33, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x478, 0x37, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x47C, 0x31, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x480, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x498, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x49C, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x7B4, 0x08, PMD_ALL, PHY_EMB_CAL_WAIT, BRD_ALL},

 {0xA006, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_init_cfg_evt0_g4[] = {
 {0x15D2, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},
 {0x15D3, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},

 {0x9529, 0x01, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x15A4, 0x3E8, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x9529, 0x00, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},

 {0xA006, 0x80000000, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x00, 0x7D0, PMD_ALL, COMMON_WAIT, BRD_ALL},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x6D8, 0xC0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x404, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x408, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x40C, 0x0A, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x410, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x414, 0x11, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xDC, 0x28, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x44C, 0x80, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x0F4, 0x01, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x48C, 0x1C, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4C8, 0xF0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x534, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x538, 0x80, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x53C, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x540, 0x80, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5A0, 0x40, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5A4, 0x20, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x70C, 0x09, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x534, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x53C, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x600, 0x04, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5B0, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x7CC, 0x0A, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x7DC, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5E4, 0x02, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5EC, 0x05, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5F0, 0x2F, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5F8, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4DC, 0x7F, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x7B4, 0x08, PMD_ALL, PHY_EMB_CAL_WAIT, BRD_ALL},

 {0xA006, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_init_cfg_evt1_g3[] = {
 {0x15D2, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},
 {0x15D3, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},

 {0x9529, 0x01, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x15A4, 0x3E8, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x9529, 0x00, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},

 {0xA006, 0x80000000, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x00, 0x7D0, PMD_ALL, COMMON_WAIT, BRD_ALL},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x6D8, 0xC0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x404, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x408, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x410, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x414, 0x11, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x64C, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x668, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x670, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x4BC, 0x12, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4C4, 0x12, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4CC, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4D4, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x9C, 0x11, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xA0, 0x11, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x10C, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x464, 0x33, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x468, 0x37, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x46C, 0x31, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x470, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x604, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x608, 0x82, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x60C, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x610, 0x98, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x618, 0x60, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x620, 0x70, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x474, 0x33, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x478, 0x37, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x47C, 0x31, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x480, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x498, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x49C, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x548, 0x2D, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x54C, 0xB7, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x550, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x7B4, 0x08, PMD_ALL, PHY_EMB_CAL_WAIT, BRD_ALL},

 {0xA006, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_init_cfg_evt1_g4[] = {
 {0x15D2, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},
 {0x15D3, 0x0, PMD_ALL, UNIPRO_ADAPT_LENGTH, BRD_ALL},

 {0x9529, 0x1, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x15A4, 0x3E8, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x9529, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},

 {0xA006, 0x80000000, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x00, 0x7D0, PMD_ALL, COMMON_WAIT, BRD_ALL},

 {0xFC, 0x10, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x600, 0x04, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x6D8, 0xC0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x400, 0x00, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x404, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x408, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x40C, 0x0A, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x410, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x414, 0x11, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x0F4, 0x01, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x48C, 0x1C, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4C8, 0xF0, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x5B0, 0x01, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x7CC, 0x0A, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x7DC, 0x06, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0x4E0, 0x56, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x54C, 0xFD, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x64C, 0x0A, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},
 {0x4DC, 0x7F, PMD_ALL, PHY_PMA_TRSV, BRD_ALL},

 {0xFC, 0x18, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0xFC, 0x00, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x7B4, 0x08, PMD_ALL, PHY_EMB_CAL_WAIT, BRD_ALL},

 {0xA006, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_init_cfg_card[] = {
 {0x9529, 0x1, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x15A4, 0xFA, PMD_ALL, UNIPRO_STD_MIB, BRD_ALL},
 {0x9529, 0x0, PMD_ALL, UNIPRO_DBG_MIB, BRD_ALL},
 {0x200, 0x40, PMD_ALL, PHY_PCS_COMN, BRD_ALL},
 {0x35, 0x05, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x73, 0x01, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x41, 0x02, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x42, 0xAC, PMD_ALL, PHY_PCS_RX, BRD_ALL},
 {0x200, 0x0, PMD_ALL, PHY_PCS_COMN, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg calib_of_pwm[] = {
 {0x2041, 8064, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg calib_of_pwm_card[] = {
 {0x2041, 8064, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_PWM, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_PWM, UNIPRO_DBG_APB, BRD_ALL},

 {0xC8, 0x40, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0xF0, 0x77, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0x120, 0x80, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x00, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0x12C, 0x00, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x43, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_calib_of_pwm[] = {
 {0x18, 0x60, PMD_PWM, PHY_PMA_COMN, BRD_ALL},
 {0x460, 0x08, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {0x4E8, 0x01, PMD_PWM, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg post_calib_of_pwm_card[] = {
 {},
};

static const struct UfsCalPhyCfg calib_of_hs_rate_a[] = {
 {0x15D4, 0x1, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x2041, 8064, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},

 {0x708, 0x08, PMD_HS, PHY_PMA_TRSV, BRD_ZEBU},

 {0x418, 0x10, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x418, 0x3C, PMD_HS_G2_L2|PMD_HS_G3_L2|PMD_HS_G4_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x4E8, 0x03, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg calib_of_hs_rate_a_card[] = {
 {0x2041, 8064, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},

 {0xC8, 0xBC, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0xF0, 0x7F, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x120, 0xC0, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x08, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x02, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x00, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x12C, 0x10, PMD_HS_G1_L2|PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x12C, 0x00, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0xE3, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x73, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x63, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},

 {0x108, 0x7C, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x10C, 0x80, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_calib_of_hs_rate_a[] = {
 {0x7b4, 0x02, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ZEBU},
 {0x7b8, 0x08, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ALL^BRD_ZEBU},
 {0x4E8, 0x01, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg post_calib_of_hs_rate_a_card[] = {
 {0x1fc, 0x01, PMD_HS, PHY_CDR_WAIT, BRD_ZEBU},
 {0x1fc, 0x40, PMD_HS, PHY_CDR_AFC_WAIT, BRD_ALL^BRD_ZEBU},
 {},
};

static const struct UfsCalPhyCfg calib_of_hs_rate_b[] = {
 {0x15D4, 0x1, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x2041, 8064, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},

 {0x708, 0x08, PMD_HS, PHY_PMA_TRSV, BRD_ZEBU},

 {0x418, 0x10, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x418, 0x3C, PMD_HS_G2_L2|PMD_HS_G3_L2|PMD_HS_G4_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x4E8, 0x03, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg calib_of_hs_rate_b_card[] = {
 {0x2041, 8064, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2042, 28224, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x2043, 20160, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B0, 12000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B1, 32000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},
 {0x15B2, 16000, PMD_HS, UNIPRO_STD_MIB, BRD_ALL},

 {0x7888, 8064, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x788C, 28224, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x7890, 20160, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78B8, 12000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78BC, 32000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},
 {0x78C0, 16000, PMD_HS, UNIPRO_DBG_APB, BRD_ALL},

 {0xC8, 0xBC, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0xF0, 0x7F, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x120, 0xC0, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x08, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x02, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x128, 0x00, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x12C, 0x10, PMD_HS_G1_L2|PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x12C, 0x00, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0xE3, PMD_HS_G1_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x73, PMD_HS_G2_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x134, 0x63, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},

 {0x108, 0x7c, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {0x10C, 0x80, PMD_HS_G3_L2, PHY_PMA_TRSV, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg post_calib_of_hs_rate_b[] = {
 {0x7b4, 0x02, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ZEBU},
 {0x7b8, 0x08, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ALL^BRD_ZEBU},
 {0x4E8, 0x01, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg post_calib_of_hs_rate_b_card[] = {
 {0x1fc, 0x01, PMD_HS, PHY_CDR_WAIT, BRD_ZEBU},
 {0x1fc, 0x40, PMD_HS, PHY_CDR_AFC_WAIT, BRD_ALL^BRD_ZEBU},
 {},
};

static const struct UfsCalPhyCfg post_h8_enter[] = {
 {0x558, 0x10, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x568, 0x0A, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x04, 0x08, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x00, 0x86, PMD_ALL, PHY_PMA_COMN, BRD_ALL},

 {0x18, 0x60, PMD_HS, PHY_PMA_COMN, BRD_ALL},
 {0x460, 0x08, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x4E8, 0x01, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {},
};
static const struct UfsCalPhyCfg post_h8_enter_card[] = {
 {0x0C4, 0x99, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x0E8, 0x7F, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x0F0, 0x7F, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x004, 0x02, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg pre_h8_exit[] = {
 {0x00, 0xC6, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x04, 0x0C, PMD_ALL, PHY_PMA_COMN, BRD_ALL},
 {0x558, 0x00, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x568, 0x00, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},

 {0x18, 0xE0, PMD_HS, PHY_PMA_COMN, BRD_ALL},
 {0x4E8, 0x03, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x460, 0x18, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x7b4, 0x02, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ZEBU},
 {0x7b8, 0x08, PMD_HS, PHY_EMB_CDR_WAIT, BRD_ALL^BRD_ZEBU},
 {},
};
static const struct UfsCalPhyCfg pre_h8_exit_card[] = {
 {0x004, 0x3F, PMD_HS, PHY_PMA_COMN, BRD_ALL},
 {0x004, 0x00, PMD_PWM, PHY_PMA_COMN, BRD_ALL},
 {0x0C4, 0xD9, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x0E8, 0x77, PMD_ALL, PHY_PMA_TRSV_SQ, BRD_ALL},
 {0x00, 0x0A, PMD_HS, COMMON_WAIT, BRD_ALL},
 {0x0F0, 0xFF, PMD_HS, PHY_PMA_TRSV, BRD_ALL},
 {0x1fc, 0x01, PMD_HS, PHY_CDR_AFC_WAIT, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg lane1_sq_off[] = {
 {0x558, 0x10, PMD_ALL, PHY_PMA_TRSV_LANE1_SQ_OFF, BRD_ALL},
 {0x568, 0x0A, PMD_ALL, PHY_PMA_TRSV_LANE1_SQ_OFF, BRD_ALL},
 {},
};

static const struct UfsCalPhyCfg lane1_sq_off_card[] = {
 {0x0C4, 0x19, PMD_ALL, PHY_PMA_TRSV_LANE1_SQ_OFF, BRD_ALL},
 {0x0E8, 0xFF, PMD_ALL, PHY_PMA_TRSV_LANE1_SQ_OFF, BRD_ALL},
 {},
};

static UfsCalError ufs_cal_wait_pll_lock (void *hba, UINT32 addr, UINT32 mask)
{
  for (INTN i = 0; i < 100; i++) {
    if ((ufs_lld_pma_read (hba, PHY_PMA_COMN_ADDR (addr)) & mask) == mask)
      return UFS_CAL_NO_ERROR;
    MicroSecondDelay (1);
  }

  DEBUG ((EFI_D_ERROR, "UFS CAL: PLL lock timeout\n"));
  return UFS_CAL_ERROR;
}

// Yes, both ufs_cal_wait_cdr_lock and ufs30_cal_wait_cdr_lock are present in the original cal file.
static UfsCalError ufs_cal_wait_cdr_lock (void *hba, UINT32 addr, UINT32 mask, int lane)
{
  for (INTN i = 0; i < 1000; i++) {
    if ((ufs_lld_pma_read (hba, PHY_PMA_TRSV_ADDR (addr, lane)) & mask) == mask)
      return UFS_CAL_NO_ERROR;

    MicroSecondDelay (1);
  }

  DEBUG ((EFI_D_ERROR, "UFS CDR lock timeout! lane %d\n", lane));
  return UFS_CAL_ERROR;
}

static UfsCalError ufs30_cal_wait_cdr_lock (void *hba, UINT32 addr, UINT32 mask, int lane)
{
  for (INTN i = 0; i < 100; i++) {
    MicroSecondDelay (40);

    if ((ufs_lld_pma_read (hba, PHY_PMA_TRSV_ADDR (addr, lane)) & mask) == mask)
      return UFS_CAL_NO_ERROR;

    MicroSecondDelay (1);

    ufs_lld_pma_write (hba, 0x10, PHY_PMA_TRSV_ADDR (0x888, lane));
    ufs_lld_pma_write (hba, 0x18, PHY_PMA_TRSV_ADDR (0x888, lane));
  }

  DEBUG ((EFI_D_ERROR, "UFS 3p0 CDR lock timeout! lane %d\n", lane));
  return UFS_CAL_ERROR;
}


static UfsCalError ufs_cal_wait_cdr_afc_check (void *hba, UINT32 addr, UINT32 mask, int lane)
{
  for (INTN i = 0; i < 100; i++) {
    MicroSecondDelay (40);

    if ((ufs_lld_pma_read (hba, PHY_PMA_TRSV_ADDR (addr, lane)) & mask) == mask)
      return UFS_CAL_NO_ERROR;

    MicroSecondDelay (1);

    ufs_lld_pma_write (hba, 0x7F, PHY_PMA_TRSV_ADDR (0xF0, lane));
    ufs_lld_pma_write (hba, 0xFF, PHY_PMA_TRSV_ADDR (0xF0, lane));
  }

  return UFS_CAL_ERROR;
}

static UfsCalError ufs30_cal_done_wait (void *hba, UINT32 addr, UINT32 mask, int lane)
{
  for (INTN i = 0; i < 100; i++) {
    MicroSecondDelay (40);
    if ((ufs_lld_pma_read (hba, PHY_PMA_TRSV_ADDR (addr, lane)) & mask) == mask)
      return UFS_CAL_NO_ERROR;
  }

  return UFS_CAL_ERROR;
}

static UfsCalError MatchModeByFlag (struct UicPwrMode *Pmd, UINT32 Flag)
{
  UINT8 m = Pmd->Mode;
  UINT8 g = Pmd->Gear;

  if (Flag == PMD_ALL)
    return UFS_CAL_NO_ERROR;

  if (IS_PWR_MODE_HS (m) && Flag == PMD_HS)
    return UFS_CAL_NO_ERROR;
  if (IS_PWR_MODE_PWM (m) && Flag == PMD_PWM)
    return UFS_CAL_NO_ERROR;

  if (IS_PWR_MODE_HS (m)) {
    if (g == 1 && (Flag & (PMD_HS_G1_L1 | PMD_HS_G1_L2))) return UFS_CAL_NO_ERROR;
    if (g == 2 && (Flag & (PMD_HS_G2_L1 | PMD_HS_G2_L2))) return UFS_CAL_NO_ERROR;
    if (g == 3 && (Flag & (PMD_HS_G3_L1 | PMD_HS_G3_L2))) return UFS_CAL_NO_ERROR;
    if (g == 4 && (Flag & (PMD_HS_G4_L1 | PMD_HS_G4_L2))) return UFS_CAL_NO_ERROR;
  }

  if (IS_PWR_MODE_PWM (m)) {
    if (g == 1 && (Flag & (PMD_PWM_G1_L1 | PMD_PWM_G1_L2))) return UFS_CAL_NO_ERROR;
    if (g == 2 && (Flag & (PMD_PWM_G2_L1 | PMD_PWM_G2_L2))) return UFS_CAL_NO_ERROR;
    if (g == 3 && (Flag & (PMD_PWM_G3_L1 | PMD_PWM_G3_L2))) return UFS_CAL_NO_ERROR;
    if (g == 4 && (Flag & (PMD_PWM_G4_L1 | PMD_PWM_G4_L2))) return UFS_CAL_NO_ERROR;
    if (g == 5 && (Flag & (PMD_PWM_G5_L1 | PMD_PWM_G5_L2))) return UFS_CAL_NO_ERROR;
  }

  return UFS_CAL_ERROR;
}

static UfsCalError ufs_cal_config_uic (
  struct UfsCalParam        *p,
  const struct UfsCalPhyCfg *cfg,
  struct UicPwrMode         *Pmd)
{
  void  *hba      = p->Host;
  UINT8  Lane;
  UINT8  NumLanes = p->AvailableLane;

  if (!cfg)
    return UFS_CAL_INV_ARG;

  for_each_phy_cfg (cfg) {
    if (!(cfg->Board & p->Board))
      continue;

    for (Lane = 0; Lane < NumLanes; Lane++) {
      if (Pmd && (MatchModeByFlag (Pmd, cfg->Flag) == UFS_CAL_ERROR))
        continue;

      switch (cfg->Layer) {
      case UNIPRO_STD_MIB:
      case UNIPRO_DBG_MIB:
        if (Lane == 0)
          ufs_lld_dme_set (hba, UIC_ARG_MIB (cfg->Address), cfg->Value);
        break;

      case UNIPRO_DBG_PRD:
        if (Lane == 0) {
          if (p->Table == HOST_EMBD)
            ufs_lld_unipro_write (hba, (UINT32)UNIPRO18_MCLK_PERIOD (p), cfg->Address);
          else
            ufs_lld_dme_set (hba, UIC_ARG_MIB (cfg->Address), UNIPRO_MCLK_PERIOD (p));
        }
        break;

      case UNIPRO_ADAPT_LENGTH:
        if (Lane == 0) {
          UINT32 Val = 0;
          ufs_lld_dme_get (hba, UIC_ARG_MIB (cfg->Address), &Val);
          if (Val & 0x80) {
            if ((Val & 0x7F) < 2)
              ufs_lld_dme_set (hba, UIC_ARG_MIB (cfg->Address), 0x82);
          } else {
            if (((Val + 1) % 4) != 0) {
              do { Val++; } while (((Val + 1) % 4) != 0);
              ufs_lld_dme_set (hba, UIC_ARG_MIB (cfg->Address), Val);
            }
          }
        }
        break;

      case PHY_PCS_COMN:
        if (Lane == 0)
          ufs_lld_dme_set (hba, UIC_ARG_MIB (cfg->Address), cfg->Value);
        break;

      case PHY_PCS_RXTX:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, TX_LANE_0 + Lane), cfg->Value);
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, RX_LANE_0 + Lane), cfg->Value);
        break;

      case PHY_PCS_RX:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, RX_LANE_0 + Lane), cfg->Value);
        break;

      case PHY_PCS_TX:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, TX_LANE_0 + Lane), cfg->Value);
        break;

      case PHY_PCS_RX_PRD:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, RX_LANE_0 + Lane),
                         UNIPRO_MCLK_PERIOD (p));
        break;

      case PHY_PCS_TX_PRD:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, TX_LANE_0 + Lane),
                         UNIPRO_MCLK_PERIOD (p));
        break;

      case PHY_PCS_RX_PRD_ROUND_OFF:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, RX_LANE_0 + Lane),
                         UNIPRO_MCLK_PERIOD_ROUND_OFF (p));
        break;

      case PHY_PCS_TX_PRD_ROUND_OFF:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address, TX_LANE_0 + Lane),
                         UNIPRO_MCLK_PERIOD_ROUND_OFF (p));
        break;

      case PHY_PCS_RX_LR_PRD:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address,   RX_LANE_0 + Lane),
                         (PCS_RX_LINE_RESET_DETECT_PERIOD (p) >> 16) & 0xFF);
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address+1, RX_LANE_0 + Lane),
                         (PCS_RX_LINE_RESET_DETECT_PERIOD (p) >>  8) & 0xFF);
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address+2, RX_LANE_0 + Lane),
                         (PCS_RX_LINE_RESET_DETECT_PERIOD (p) >>  0) & 0xFF);
        break;

      case PHY_PCS_TX_LR_PRD:
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address,   TX_LANE_0 + Lane),
                         (PCS_TX_LINE_RESET_PERIOD (p) >> 16) & 0xFF);
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address+1, TX_LANE_0 + Lane),
                         (PCS_TX_LINE_RESET_PERIOD (p) >>  8) & 0xFF);
        ufs_lld_dme_set (hba, UIC_ARG_MIB_SEL (cfg->Address+2, TX_LANE_0 + Lane),
                         (PCS_TX_LINE_RESET_PERIOD (p) >>  0) & 0xFF);
        break;

      case PHY_PMA_COMN:
        if (Lane == 0)
          ufs_lld_pma_write (hba, cfg->Value, PHY_PMA_COMN_ADDR (cfg->Address));
        break;

      case PHY_PMA_TRSV:
        ufs_lld_pma_write (hba, cfg->Value, PHY_PMA_TRSV_ADDR (cfg->Address, Lane));
        break;

      case PHY_PMA_TRSV_LANE1_SQ_OFF:
        if (Lane == 1) {
          if (p->ConnectedRxLane < p->AvailableLane)
            ufs_lld_pma_write (hba, cfg->Value, PHY_PMA_TRSV_ADDR (cfg->Address, Lane));
        }
        break;

      case PHY_PMA_TRSV_SQ:
        if (Lane < p->ConnectedRxLane)
          ufs_lld_pma_write (hba, cfg->Value, PHY_PMA_TRSV_ADDR (cfg->Address, Lane));
        break;

      case UNIPRO_DBG_APB:
        if (Lane == 0)
          ufs_lld_unipro_write (hba, cfg->Value, cfg->Address);
        break;

      case PHY_PLL_WAIT:
        if (Lane == 0) {
          if (ufs_cal_wait_pll_lock (hba, cfg->Address, cfg->Value) == UFS_CAL_ERROR)
            return UFS_CAL_TIMEOUT;
        }
        break;

      case PHY_CDR_WAIT:
        if (Lane < p->ActiveRxLane) {
          if (ufs_cal_wait_cdr_lock (hba, cfg->Address, cfg->Value, Lane) == UFS_CAL_ERROR)
            return UFS_CAL_TIMEOUT;
        }
        break;

      case PHY_EMB_CDR_WAIT:
        if (Lane < p->ActiveRxLane) {
          if (ufs30_cal_wait_cdr_lock (hba, cfg->Address, cfg->Value, Lane) == UFS_CAL_ERROR)
            return UFS_CAL_TIMEOUT;
        }
        break;

      case PHY_CDR_AFC_WAIT:
        if (Lane < p->ActiveRxLane) {
          if (p->Table == HOST_CARD) {
            if (ufs_cal_wait_cdr_afc_check (hba, cfg->Address, cfg->Value, Lane) == UFS_CAL_ERROR)
              return UFS_CAL_TIMEOUT;
          }
        }
        break;

      case COMMON_WAIT:
        if (Lane == 0)
          MicroSecondDelay (cfg->Value);
        break;

      case PHY_EMB_CAL_WAIT:
        if (ufs30_cal_done_wait (hba, cfg->Address, cfg->Value, Lane) == UFS_CAL_ERROR)
          return UFS_CAL_TIMEOUT;
        break;

      default:
        break;
      }
    }
  }

  return UFS_CAL_NO_ERROR;
}

/*
 * This is a recommendation from Samsung UFS device vendor.
 *
 * Activate time: host < device
 * Hibern time: host > device
 */
static VOID ufs_cal_calib_hibern8_values (void *hba)
{
  UINT32 hw_cap_min_tactivate;
  UINT32 peer_rx_min_actv_time_cap;
  UINT32 max_rx_hibern8_time_cap;

  ufs_lld_dme_get (hba, UIC_ARG_MIB_SEL (0x8F, RX_LANE_0), &hw_cap_min_tactivate);
  ufs_lld_dme_get (hba, UIC_ARG_MIB (0x15A8), &peer_rx_min_actv_time_cap);
  ufs_lld_dme_get (hba, UIC_ARG_MIB (0x15A7), &max_rx_hibern8_time_cap);

  if (peer_rx_min_actv_time_cap >= hw_cap_min_tactivate)
    ufs_lld_dme_peer_set (hba, UIC_ARG_MIB (0x15A8), peer_rx_min_actv_time_cap + 1);
  ufs_lld_dme_set (hba, UIC_ARG_MIB (0x15A7), max_rx_hibern8_time_cap + 1);
}

UfsCalError ufs_cal_post_h8_enter (struct UfsCalParam *p)
{
  const struct UfsCalPhyCfg *cfg =
    (p->Table == HOST_CARD) ? post_h8_enter_card : post_h8_enter;
  return ufs_cal_config_uic (p, cfg, p->Pmd);
}

UfsCalError ufs_cal_pre_h8_exit (struct UfsCalParam *p)
{
  const struct UfsCalPhyCfg *cfg =
    (p->Table == HOST_CARD) ? pre_h8_exit_card : pre_h8_exit;
  return ufs_cal_config_uic (p, cfg, p->Pmd);
}

UfsCalError UfsCalPrePmc (struct UfsCalParam *p)
{
  const struct UfsCalPhyCfg *cfg;

  if ((p->Pmd->Mode == SLOW_MODE) || (p->Pmd->Mode == SLOWAUTO_MODE))
    cfg = (p->Table == HOST_CARD) ? calib_of_pwm_card : calib_of_pwm;
  else if (p->Pmd->HsSeries == PA_HS_MODE_B)
    cfg = (p->Table == HOST_CARD) ? calib_of_hs_rate_b_card : calib_of_hs_rate_b;
  else if (p->Pmd->HsSeries == PA_HS_MODE_A)
    cfg = (p->Table == HOST_CARD) ? calib_of_hs_rate_a_card : calib_of_hs_rate_a;
  else
    return UFS_CAL_INV_ARG;

  return ufs_cal_config_uic (p, cfg, p->Pmd);
}

UfsCalError UfsCalPostPmc (struct UfsCalParam *p)
{
  const struct UfsCalPhyCfg *cfg;

  if ((p->Pmd->Mode == SLOWAUTO_MODE) || (p->Pmd->Mode == SLOW_MODE))
    cfg = (p->Table == HOST_CARD) ? post_calib_of_pwm_card : post_calib_of_pwm;
  else if (p->Pmd->HsSeries == PA_HS_MODE_B)
    cfg = (p->Table == HOST_CARD) ? post_calib_of_hs_rate_b_card : post_calib_of_hs_rate_b;
  else if (p->Pmd->HsSeries == PA_HS_MODE_A)
    cfg = (p->Table == HOST_CARD) ? post_calib_of_hs_rate_a_card : post_calib_of_hs_rate_a;
  else
    return UFS_CAL_INV_ARG;

  return ufs_cal_config_uic (p, cfg, p->Pmd);
}

UfsCalError UfsCalPostLink (struct UfsCalParam *p)
{
  UfsCalError                ret = UFS_CAL_NO_ERROR;
  const struct UfsCalPhyCfg *cfg;

  ufs_cal_calib_hibern8_values (p->Host);

  switch (p->MaxGear) {
  case GEAR_1:
  case GEAR_2:
  case GEAR_3:
    cfg = (p->EvtVer == 0) ? post_init_cfg_evt0_g3 : post_init_cfg_evt1_g3;
    if (p->Table == HOST_CARD) cfg = post_init_cfg_card;
    break;
  case GEAR_4:
    cfg = (p->EvtVer == 0) ? post_init_cfg_evt0_g4 : post_init_cfg_evt1_g4;
    if (p->Table == HOST_CARD) cfg = post_init_cfg_card;
    break;
  default:
    return UFS_CAL_INV_ARG;
  }

  ret = ufs_cal_config_uic (p, cfg, NULL);

  if (ret == UFS_CAL_NO_ERROR) {
    if ((p->AvailableLane == 2) && (p->ConnectedRxLane == 1)) {
      cfg = (p->Table == HOST_CARD) ? lane1_sq_off_card : lane1_sq_off;
      ret = ufs_cal_config_uic (p, cfg, NULL);
    }
  }

  return ret;
}

UfsCalError UfsCalPreLink (struct UfsCalParam *p)
{
  const struct UfsCalPhyCfg *cfg;

  if (p->Table == HOST_CARD)
    cfg = init_cfg_card;
  else
    cfg = (p->EvtVer == 0) ? init_cfg_evt0 : init_cfg_evt1;

  return ufs_cal_config_uic (p, cfg, NULL);
}

UINT8 UfsCalGetTargetBoard (VOID)
{
  //
  // DTB: ufs@0x13D60000 -> brd-for-cal = [01]
  // Linux bunu dogrudan maske olarak kullaniyor: (board & cfg->Board).
  // 0x01 = BRD_SMDK biti; BRD_ALL satirlari eslesir, BRD_ZEBU gibi
  // tek-bit satirlar atlanir. (9830 kendi DTB'sine gore BRD_UNIV donuyor.)
  //
  return 0x01;
}

UfsCalError UfsCalInit (struct UfsCalParam *p)
{
  ufs_cal[0]            = p;

  return UFS_CAL_NO_ERROR;
}
