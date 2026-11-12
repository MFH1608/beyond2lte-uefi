#ifndef _PMU_H_
#define _PMU_H_

//
// PMU Registers
//
#define PS_HOLD_CONTROL        0x030C
#define SWRESET                0x3A00

//
// SWRESET Register Bits
//
// Reset tetikleyicisi bit 1'dir (0x2). Referans: exynos-reboot.c
// (CruelKernel/samsung-exynos9820) -> SWRESET_TRIGGER (1 << 1).
// "Exynos: Refactor" bu degeri TRUE (0x1) yapmisti ve reset calismiyordu.
//
#define SWRESET_TRIGGER        (1 << 1)

//
// PS_HOLD Register Bits
//
#define PS_HOLD_LOW            BIT8

#endif /* _PMU_H_ */
