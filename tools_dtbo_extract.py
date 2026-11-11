#!/usr/bin/env python3
"""Android `dtbo.img` (ve Samsung `dt.img`) icinden DTB sec ve cikar.

NEDEN VAR
---------------------------------------------------------------------------
`tools_dtb_s10e.sh --tar` bir stok AP tar'inden S10e DTB'sini uretmek
istiyor. Once `mkdtimg` gerektiriyordu; WSL'de o arac yok ve kurmak icin
Android SDK indirmek gerekiyor. Oysa DTBO bicimi kucuk ve sabittir; burada
saf Python ile cozulur.

IKI BICIM
---------------------------------------------------------------------------
* `dtbo.img`  — Android resmi tablo bicimi (mkdtimg ciktisi). Sihirli sayi
                `0xd7b7ab1e`. Basligi ve girdi tablosu asagida tanimli.
* `dt.img`    — Samsung'un base DTB kabi. Bicim belgelenmemis/degisken
                oldugu icin GUVENLI YOL secildi: dosyada `d00dfeed` FDT
                sihirli sayisini tarayip her gecerli FDT'yi cikaririz.
                Elimizdeki `dtb-analiz/exynos9820-base.dtb` de dumduz bir
                FDT oldugu icin bu yaklasim kanitli.

Bu araç YALNIZCA okur ve secer; `fdtoverlay` ile birlestirmeyi
`tools_dtb_s10e.sh` yapar.

KULLANIM
---------------------------------------------------------------------------
    # dtbo girdilerini listele (id/rev/custom + model/compatible)
    python3 tools_dtbo_extract.py --dtbo dtbo.img --list

    # S10e girdisini otomatik sec ve yaz
    python3 tools_dtbo_extract.py --dtbo dtbo.img --pick beyond0lte --out overlay.dtb

    # base DTB'yi dt.img'den al
    python3 tools_dtbo_extract.py --dt-img dt.img --out base.dtb

    # ikisini de tar'den cikar ve base+overlay sec
    python3 tools_dtbo_extract.py --tar AP_G970F....tar.md5 --dir work/

Cikti yalnizca ozettir; secilen dosyalar --out/--dir ile yazilir.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

FDT_MAGIC = 0xD00DFEED
DTBO_MAGIC = 0xD7B7AB1E
DTBO_HEADER_SIZE = 32
DTBO_ENTRY_SIZE = 32

# Android < 9 "version 1" basligi: custom alanlari baslik sonunda toplu
# gelir. Header size 32, entry size 32, ama custom sayisi header'da belirtilir.
# Pratikte Samsung 9820 v0 kullanir; v1'de de taban alanlar aynidir.

DEVICE_PATTERNS = {
    "beyond0lte": re.compile(rb"(beyond0lte|BEYOND0LTE|G970)", re.I),
    "beyond1lte": re.compile(rb"(beyond1lte|BEYOND1LTE|G973)", re.I),
    "beyond2lte": re.compile(rb"(beyond2lte|BEYOND2LTE|G975)", re.I),
}


def die(message: str) -> "None":
    print(f"HATA: {message}", file=sys.stderr)
    raise SystemExit(1)


# --------------------------------------------------------------------------
# FDT yardimcilari
# --------------------------------------------------------------------------
@dataclass
class FdtBlob:
    offset: int
    size: int
    data: bytes


def fdt_size_at(buf: bytes, offset: int) -> int | None:
    """Offset'te gecerli bir FDT varsa toplam boyutunu dondur, yoksa None."""
    if offset + 8 > len(buf):
        return None
    magic, totalsize = struct.unpack_from(">II", buf, offset)
    if magic != FDT_MAGIC:
        return None
    # totalsize basligin kendisini de kapsar; makul sinir koyalim.
    if totalsize < 72 or offset + totalsize > len(buf):
        return None
    return totalsize


def scan_fdts(buf: bytes) -> list[FdtBlob]:
    """Dosyadaki tum gecerli FDT'leri sirayla bulur."""
    blobs: list[FdtBlob] = []
    pos = 0
    while True:
        idx = buf.find(struct.pack(">I", FDT_MAGIC), pos)
        if idx < 0:
            break
        size = fdt_size_at(buf, idx)
        if size is None:
            pos = idx + 4
            continue
        blobs.append(FdtBlob(idx, size, buf[idx : idx + size]))
        pos = idx + size
    return blobs


def fdt_strings(blob: bytes, limit: int = 0x40000) -> str:
    """DTB'nin string blogu + butun iceriginde okunabilir metinleri toplar.

    Tam cozumleme yerine kaba tarama yeterli: `beyond0lte` gibi ayirt edici
    metin ya model ya compatible ya da fragment yolunda gecer.
    """
    chunk = blob[:limit] if limit else blob
    return chunk.decode("latin-1", errors="replace")


def dtc_dts(blob: bytes) -> str:
    """FDT'yi dtc ile dts'e cevirir (yoksa bos string doner)."""
    import tempfile

    with tempfile.NamedTemporaryFile(suffix=".dtb", delete=True) as src:
        src.write(blob)
        src.flush()
        proc = subprocess.run(
            ["dtc", "-I", "dtb", "-O", "dts", src.name],
            capture_output=True,
            text=True,
        )
    if proc.returncode != 0:
        return ""
    return proc.stdout


def model_of(dts: str) -> str:
    match = re.search(r'^\s*model = "([^"]*)"', dts, re.M)
    return match.group(1) if match else ""


def compatible_of(dts: str) -> str:
    match = re.search(r"^\s*compatible = (.+);", dts, re.M)
    return match.group(1).strip() if match else ""


# --------------------------------------------------------------------------
# DTBO tablosu
# --------------------------------------------------------------------------
@dataclass
class DtboEntry:
    index: int
    dt_size: int
    dt_offset: int
    id: int
    rev: int
    custom: tuple[int, int, int, int]
    blob: bytes = b""
    strings: str = field(default="", repr=False)


@dataclass
class Dtbo:
    total_size: int
    header_size: int
    entry_size: int
    page_size: int
    version: int
    entries: list[DtboEntry]


def parse_dtbo(buf: bytes) -> Dtbo:
    if len(buf) < DTBO_HEADER_SIZE:
        die("dtbo.img cok kucuk")
    (
        magic,
        total_size,
        header_size,
        entry_size,
        entry_count,
        entries_offset,
        page_size,
        version,
    ) = struct.unpack_from("<8I", buf, 0)
    if magic != DTBO_MAGIC:
        die(
            "bu bir Android DTBO imaji degil "
            f"(sihirli sayi 0x{magic:08x}, beklenen 0x{DTBO_MAGIC:08x})"
        )
    if header_size < DTBO_HEADER_SIZE or entry_size < DTBO_ENTRY_SIZE:
        die(f"beklenmeyen baslik/girdi boyutu: header={header_size} entry={entry_size}")
    if total_size > len(buf):
        die(f"total_size ({total_size}) dosya boyutunu ({len(buf)}) asiyor")
    if entries_offset + entry_count * entry_size > len(buf):
        die("girdi tablosu dosyanin disinda")

    entries: list[DtboEntry] = []
    for i in range(entry_count):
        base = entries_offset + i * entry_size
        dt_size, dt_offset, ent_id, rev = struct.unpack_from("<4I", buf, base)
        custom = struct.unpack_from("<4I", buf, base + 16)
        if dt_offset + dt_size > len(buf):
            die(f"girdi {i}: dt_offset+dt_size dosyanin disinda")
        blob = buf[dt_offset : dt_offset + dt_size]
        entries.append(
            DtboEntry(
                i,
                dt_size,
                dt_offset,
                ent_id,
                rev,
                custom,
                blob,
                fdt_strings(blob),
            )
        )
    return Dtbo(total_size, header_size, entry_size, page_size, version, entries)


# --------------------------------------------------------------------------
# Secim
# --------------------------------------------------------------------------
def score_entry(entry: DtboEntry, needle: str) -> int:
    """Girdiyi hedef cihaza ne kadar benzettigini puanlar."""
    pat = DEVICE_PATTERNS.get(needle)
    needle_l = needle.lower()
    score = 0
    text = entry.strings
    if pat is not None:
        if pat.search(text.encode("latin-1", errors="replace")):
            score += 10
    elif needle_l and needle_l in text.lower():
        score += 5

    if text:
        dts = dtc_dts(entry.blob)
        if dts:
            dts_l = dts.lower()
            model = model_of(dts)
            if needle_l and needle_l in model.lower():
                score += 50
            elif needle_l and needle_l in dts_l:
                score += 20
            comp = compatible_of(dts)
            if needle_l and needle_l in comp.lower():
                score += 30
    return score


def describe_entry(entry: DtboEntry, verify: bool) -> str:
    custom = ",".join(f"0x{c:X}" for c in entry.custom)
    line = (
        f"  [{entry.index:2}] size={entry.dt_size:7} "
        f"id=0x{entry.id:X} rev=0x{entry.rev:X} custom={custom}"
    )
    if verify:
        dts = dtc_dts(entry.blob)
        model = model_of(dts)
        comp = compatible_of(dts)
        extra = []
        if model:
            extra.append(f"model={model!r}")
        if comp:
            extra.append(f"compatible={comp}")
        if not dts:
            extra.append("(dtc cozemedi)")
        line += "\n       " + "  ".join(extra)
    return line


def pick_entry(
    dtbo: Dtbo, needle: str, verify: bool, explicit_index: int | None
) -> DtboEntry:
    if explicit_index is not None:
        if not (0 <= explicit_index < len(dtbo.entries)):
            die(f"--index {explicit_index} araligin disinda (0..{len(dtbo.entries)-1})")
        return dtbo.entries[explicit_index]

    scored = [(score_entry(e, needle), e) for e in dtbo.entries]
    best = max(scored, key=lambda pair: pair[0]) if scored else (0, None)
    top = [e for s, e in scored if s == best[0] and s > 0]
    if not top:
        print(
            "HATA: hicbir girdi hedef cihaza benzemiyor. "
            "Asagidaki tablodan --index ile sec:",
            file=sys.stderr,
        )
        for e in dtbo.entries:
            print(describe_entry(e, verify), file=sys.stderr)
        raise SystemExit(1)
    if len(top) > 1:
        print(
            f"UYARI: {len(top)} girdi ayni puani aldi; ilki secildi. "
            "Emin degilsen --index kullan.",
            file=sys.stderr,
        )
    return top[0]


# --------------------------------------------------------------------------
# Tar / sikistirma
# --------------------------------------------------------------------------
def extract_from_tar(tar_path: Path, workdir: Path) -> dict[str, Path]:
    """AP tar'inden dt.img ve dtbo.img cikarir. .md5 uzantisi onemsiz."""
    workdir.mkdir(parents=True, exist_ok=True)
    names = [
        "dt.img",
        "dtbo.img",
        "dt.img.lz4",
        "dtbo.img.lz4",
        "dtbo",
    ]
    wanted = {n: workdir / n for n in names}
    # tar uyelerinin bas yolunu yok say: 'dtbo.img', './dtbo.img', 'a/dtbo.img'
    proc = subprocess.run(
        ["tar", "-xf", str(tar_path), "-C", str(workdir),
         "--wildcards", "--no-anchored"] + names,
        capture_output=True,
        text=True,
    )
    found: dict[str, Path] = {}
    for name, dest in wanted.items():
        if dest.is_file():
            found[name] = dest
    if not found:
        print(
            "HATA: tar icinde dt.img / dtbo.img bulunamadi.\n"
            f"  tar cikisi: {proc.stderr.strip()}\n"
            "  Tar icerigini kontrol et: tar -tf <dosya> | grep -i dt",
            file=sys.stderr,
        )
        raise SystemExit(1)
    # .lz4 varsa ac
    for name in list(found):
        if name.endswith(".lz4"):
            plain = found[name].with_suffix("")
            r = subprocess.run(
                ["lz4", "-d", "-f", str(found[name]), str(plain)],
                capture_output=True,
                text=True,
            )
            if r.returncode != 0:
                die(f"lz4 acilamadi ({name}): {r.stderr.strip()}  (sudo apt install lz4)")
            found[plain.name] = plain
    return found


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def resolve_dtbo(path: Path) -> Dtbo:
    buf = path.read_bytes()
    return parse_dtbo(buf)


def cmd_list(path: Path, verify: bool) -> int:
    dtbo = resolve_dtbo(path)
    print(
        f"DTBO: {path.name}  surum={dtbo.version} sayfa={dtbo.page_size} "
        f"girdi={len(dtbo.entries)}"
    )
    for entry in dtbo.entries:
        print(describe_entry(entry, verify))
        if verify:
            for dev in DEVICE_PATTERNS:
                if score_entry(entry, dev) > 0:
                    print(f"       -> eslesiyor: {dev}")
    return 0


def cmd_extract_dtbo(path: Path, needle: str, out: Path, verify: bool, index) -> int:
    dtbo = resolve_dtbo(path)
    if len(dtbo.entries) == 0:
        die("dtbo.img icinde hic girdi yok")
    entry = pick_entry(dtbo, needle, verify, index)
    out.write_bytes(entry.blob)
    dts = dtc_dts(entry.blob)
    print(
        f"  secildi: girdi [{entry.index}] id=0x{entry.id:X} rev=0x{entry.rev:X} "
        f"({entry.dt_size} bayt)"
    )
    if dts:
        model = model_of(dts)
        if model:
            print(f"  model     : {model}")
        print(f"  compatible: {compatible_of(dts)}")
    print(f"  yazildi   : {out}")
    return 0


def cmd_extract_dtimg(path: Path, out: Path, index: int) -> int:
    buf = path.read_bytes()
    # Once tablo bicimi olabilir mi diye DTBO'ya bakalim.
    if len(buf) >= 4 and struct.unpack_from("<I", buf, 0)[0] == DTBO_MAGIC:
        dtbo = parse_dtbo(buf)
        if not (0 <= index < len(dtbo.entries)):
            die(f"--index {index} aralik disinda (0..{len(dtbo.entries)-1})")
        blob = dtbo.entries[index].blob
        out.write_bytes(blob)
        print(f"  (dt.img aslinda DTBO tablosu; girdi [{index}] alindi)")
        return 0

    blobs = scan_fdts(buf)
    if not blobs:
        die("dt.img icinde gecerli FDT bulunamadi")
    blobs.sort(key=lambda b: b.size, reverse=True)
    if not (0 <= index < len(blobs)):
        die(f"--index {index} aralik disinda (0..{len(blobs)-1})")
    blob = blobs[index]
    out.write_bytes(blob.data)
    dts = dtc_dts(blob.data)
    print(f"  dt.img icinde {len(blobs)} FDT bulundu; en buyugu secildi (index {index})")
    print(f"  offset={blob.offset} size={blob.size}")
    if dts:
        model = model_of(dts)
        if model:
            print(f"  model     : {model}")
    print(f"  yazildi   : {out}")
    return 0


def cmd_from_tar(
    tar_path: Path, workdir: Path, needle: str, verify: bool, index, quiet: bool
) -> int:
    found = extract_from_tar(tar_path, workdir)
    print(f"  tar'dan cikarildi: {', '.join(sorted(found))}")
    base_out = workdir / "base.dtb"
    overlay_out = workdir / "overlay.dtb"
    if "dt.img" in found:
        cmd_extract_dtimg(found["dt.img"], base_out, 0)
    else:
        print("  UYARI: dt.img yok; base ayrica --base ile verilmeli", file=sys.stderr)
    if "dtbo.img" in found:
        cmd_extract_dtbo(found["dtbo.img"], needle, overlay_out, verify, index)
    else:
        print("  UYARI: dtbo.img yok; overlay ayrica --overlay ile verilmeli", file=sys.stderr)
    if not quiet:
        print()
        print("SIRADAKI ADIM")
        print(f"  bash tools_dtb_s10e.sh --base {base_out} --overlay {overlay_out}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Android dtbo.img / Samsung dt.img icinden DTB sec ve cikar",
    )
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--dtbo", type=Path, help="dtbo.img yolu (listele/sec)")
    src.add_argument("--dt-img", type=Path, help="Samsung dt.img yolu (base FDT)")
    src.add_argument("--tar", type=Path, help="stok AP tar(.md5): dt.img+dtbo.img cikar")

    parser.add_argument("--list", action="store_true", help="dtbo girdilerini listele")
    parser.add_argument(
        "--pick",
        default="beyond0lte",
        help="hedef cihaz anahtari: beyond0lte|beyond1lte|beyond2lte "
        "veya serbest metin (varsayilan: beyond0lte)",
    )
    parser.add_argument("--index", type=int, default=None, help="girdiyi elle sec")
    parser.add_argument("--no-verify", action="store_true", help="dtc ile model/compatible okuma")
    parser.add_argument("--out", type=Path, help="cikti dosyasi")
    parser.add_argument("--dir", type=Path, default=Path("dtbo-cikti"), help="--tar calisma klasoru")
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="--tar sonrasi 'siradaki adim' onerisini yazma (baska betikten cagrilirken)",
    )

    args = parser.parse_args()
    verify = not args.no_verify

    for candidate in (args.dtbo, args.dt_img, args.tar):
        if candidate is not None and not candidate.is_file():
            die(f"dosya yok: {candidate}")

    if args.dtbo is not None:
        if args.list:
            return cmd_list(args.dtbo, verify)
        if args.out is None:
            die("--dtbo icin --list ya da --out gerekli")
        return cmd_extract_dtbo(args.dtbo, args.pick, args.out, verify, args.index)

    if args.dt_img is not None:
        if args.out is None:
            die("--dt-img icin --out gerekli")
        return cmd_extract_dtimg(args.dt_img, args.out, args.index or 0)

    if args.tar is not None:
        return cmd_from_tar(
            args.tar, args.dir, args.pick, verify, args.index, args.quiet
        )

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
