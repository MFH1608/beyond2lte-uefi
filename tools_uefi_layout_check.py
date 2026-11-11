#!/usr/bin/env python3
"""Validate the fixed physical layout shared by BootShim and beyond2lte UEFI.

BootShim copies exactly ``[uefi_fd].size`` bytes to ``[uefi_fd].base`` before
branching into it.  The memory map and the primary CPU stack must therefore
describe a non-overlapping region outside that copy.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def device_files(device: str) -> tuple[Path, Path, Path]:
    """Return (config, MemoryMapLib, DSC) paths for a supported device."""
    base = ROOT / "mu-silicium-overlay" / "Platforms" / "Samsung" / f"{device}Pkg"
    return (
        ROOT / "mu-silicium-overlay" / "Resources" / "Configs" / f"{device}.toml",
        base / "Library" / "MemoryMapLib" / "MemoryMapLib.c",
        base / f"{device}.dsc",
    )


def fail(message: str) -> None:
    print(f"HATA: UEFI bellek yerlesimi gecersiz: {message}", file=sys.stderr)
    raise SystemExit(1)


def region(source: str, name: str) -> tuple[int, int]:
    match = re.search(
        rf'\{{"{re.escape(name)}",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)',
        source,
    )
    if not match:
        fail(f'MemoryMapLib icinde "{name}" bolgesi bulunamadi')
    return int(match.group(1), 0), int(match.group(2), 0)


def overlaps(first: tuple[int, int], second: tuple[int, int]) -> bool:
    first_base, first_size = first
    second_base, second_size = second
    return first_base < second_base + second_size and second_base < first_base + first_size


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device",
        default="beyond2lte",
        help="Platform package / config basename (default: beyond2lte)",
    )
    args = parser.parse_args()

    config_path, memory_map_path, dsc_path = device_files(args.device)
    for path in (config_path, memory_map_path, dsc_path):
        if not path.is_file():
            fail(f"dosya yok: {path}")

    with config_path.open("rb") as config_file:
        config = tomllib.load(config_file)

    fd_config = config.get("uefi_fd")
    if not isinstance(fd_config, dict):
        fail("[uefi_fd] bolumu yok")

    try:
        fd = (int(fd_config["base"]), int(fd_config["size"]))
    except (KeyError, TypeError, ValueError):
        fail("[uefi_fd].base/size tamsayi degil")

    if fd[1] == 0:
        fail("FD boyutu sifir")

    memory_map = memory_map_path.read_text(encoding="utf-8")
    dsc = dsc_path.read_text(encoding="utf-8")
    mapped_fd = region(memory_map, "UEFI FD")
    stack = region(memory_map, "UEFI Stack")
    display = region(memory_map, "Display Reserved")

    pcd_match = re.search(
        r"gArmPlatformTokenSpaceGuid\.PcdCPUCoresStackBase\|(0x[0-9A-Fa-f]+)",
        dsc,
    )
    if not pcd_match:
        fail("PcdCPUCoresStackBase bulunamadi")
    pcd_stack_base = int(pcd_match.group(1), 0)

    if mapped_fd != fd:
        fail(
            "config FD=0x%X+0x%X, MemoryMapLib FD=0x%X+0x%X"
            % (*fd, *mapped_fd)
        )
    if stack[0] != pcd_stack_base:
        fail(
            "stack tabani MemoryMapLib=0x%X, DSC PCD=0x%X"
            % (stack[0], pcd_stack_base)
        )
    if overlaps(fd, stack):
        fail("FD ve CPU stack cakiliyor")
    if overlaps(fd, display) or overlaps(stack, display):
        fail("FD veya CPU stack framebuffer rezerviyle cakisiyor")

    print(
        "UEFI yerlesimi dogrulandi: "
        f"FD=0x{fd[0]:08X}..0x{fd[0] + fd[1]:08X}, "
        f"stack=0x{stack[0]:08X}..0x{stack[0] + stack[1]:08X}"
    )


if __name__ == "__main__":
    main()
