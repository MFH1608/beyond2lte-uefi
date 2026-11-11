#!/usr/bin/env python3
"""Compare fixed DT reserved-memory ranges with the UEFI memory map.

The firmware does not consume the Android DTB at runtime.  A fixed ``reg``
range below ``/reserved-memory`` is unavailable to Linux, but remains
allocatable by Windows unless MemoryMapLib emits it as EfiReservedMemoryType.
Dynamic ``size``/``alloc-ranges`` heaps have no fixed address before Linux
allocates them, so this check reports but cannot reserve them.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def device_files(device: str) -> tuple[Path, Path]:
    """Return (DTB source, MemoryMapLib source) for a supported device."""
    return (
        ROOT / f"mu-silicium-overlay/Resources/DTBs/{device}.dts",
        ROOT
        / f"mu-silicium-overlay/Platforms/Samsung/{device}Pkg/Library/MemoryMapLib/MemoryMapLib.c",
    )


@dataclass
class Node:
    name: str
    parent: "Node | None"
    properties: list[str] = field(default_factory=list)

    def under_reserved_memory(self) -> bool:
        node: Node | None = self.parent
        while node is not None:
            if node.name == "reserved-memory":
                return True
            node = node.parent
        return False


@dataclass(frozen=True)
class Region:
    name: str
    base: int
    size: int
    secure: bool

    @property
    def end(self) -> int:
        return self.base + self.size


def parse_nodes(source: str) -> list[Node]:
    """Parse the simple brace/property subset used by the checked-in DTS."""
    nodes: list[Node] = []
    stack: list[Node] = []
    open_node = re.compile(r"^\s*(/|[A-Za-z0-9_,+\-]+)(?:@[^\s{]+)?\s*\{\s*$")

    for line in source.splitlines():
        stripped = line.strip()
        match = open_node.match(line)
        if match:
            node = Node(match.group(1), stack[-1] if stack else None)
            nodes.append(node)
            stack.append(node)
        elif stripped == "};":
            if not stack:
                raise ValueError("DTS brace nesting is invalid")
            stack.pop()
        elif stack and stripped and not stripped.startswith(("//", "/*", "*")):
            stack[-1].properties.append(stripped)

    if stack:
        raise ValueError("DTS has an unclosed node")
    return nodes


def property_value(node: Node, property_name: str) -> str | None:
    for line in node.properties:
        match = re.match(rf"{re.escape(property_name)}\s*=\s*(.*);$", line)
        if match:
            return match.group(1)
    return None


def fixed_regions(nodes: list[Node]) -> tuple[list[Region], list[str]]:
    fixed: list[Region] = []
    dynamic: list[str] = []
    reg_pattern = re.compile(
        r"<\s*(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s+(0x[0-9A-Fa-f]+)\s*>"
    )

    for node in nodes:
        if not node.under_reserved_memory():
            continue
        reg = property_value(node, "reg")
        if reg is not None:
            match = reg_pattern.fullmatch(reg)
            if not match:
                raise ValueError(f"{node.name}: unsupported reg form: {reg}")
            high, low, size = (int(value, 0) for value in match.groups())
            fixed.append(
                Region(
                    node.name,
                    (high << 32) | low,
                    size,
                    "ion,secure;" in node.properties,
                )
            )
        elif property_value(node, "size") is not None:
            dynamic.append(node.name)

    return sorted(fixed, key=lambda region: region.base), sorted(dynamic)


def reserved_map_regions(source: str) -> list[Region]:
    descriptor = re.compile(
        r'^\s*\{"([^"]+)",\s*(0x[0-9A-Fa-f]+)(?:ULL)?,\s*'
        r'(0x[0-9A-Fa-f]+)(?:ULL)?,\s*Add\w+,\s*\w+,\s*\w+,\s*'
        r'(\w+),',
        re.MULTILINE,
    )
    regions: list[Region] = []
    for match in descriptor.finditer(source):
        name, base, size, memory_type = match.groups()
        if memory_type == "Reserv":
            regions.append(Region(name, int(base, 0), int(size, 0), False))
    return regions


def is_covered(target: Region, reserved: list[Region]) -> bool:
    return any(region.base <= target.base and region.end >= target.end for region in reserved)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device",
        default="beyond2lte",
        help="Platform package / config basename (default: beyond2lte)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="exit non-zero if a fixed DT reserved-memory region is exposed",
    )
    args = parser.parse_args()

    dt_source, memory_map = device_files(args.device)
    for path in (dt_source, memory_map):
        if not path.is_file():
            print(f"HATA: dosya yok: {path}", file=sys.stderr)
            raise SystemExit(1)

    fixed, dynamic = fixed_regions(parse_nodes(dt_source.read_text(encoding="utf-8")))
    reserved = reserved_map_regions(memory_map.read_text(encoding="utf-8"))
    missing = [region for region in fixed if not is_covered(region, reserved)]

    print(f"Cihaz: {args.device}  (DTB: {dt_source.name})")
    print("DTB fixed reserved-memory -> UEFI EfiReservedMemoryType")
    for region in fixed:
        state = "KORUNDU" if region not in missing else "ACIK"
        kind = " secure" if region.secure else ""
        print(
            f"{state:7}  {region.name:20} "
            f"0x{region.base:010X}-0x{region.end - 1:010X} "
            f"({region.size // 1024:7} KiB){kind}"
        )

    if dynamic:
        print("Dinamik (sabit adres yok, bu denetim karar veremez): " + ", ".join(dynamic))

    if missing:
        print(
            f"HATA: {len(missing)} sabit DTB rezervi Windows'a tahsis edilebilir gorunuyor.",
            file=sys.stderr,
        )
        if args.check:
            raise SystemExit(1)
    else:
        print("SONUC: tum sabit DTB rezervleri EfiReservedMemoryType ile korunuyor.")


if __name__ == "__main__":
    main()
