# SPDX-License-Identifier: GPL-2.0-or-later
"""Run libft's C unit tests (libft/test) from pytest."""
import shutil
import subprocess
from pathlib import Path

import pytest

LIBFT = Path(__file__).resolve().parent.parent / "libft"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")


@needs_cc
def test_libft_unit_tests():
    r = subprocess.run(["make", "-C", str(LIBFT), "test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


import zlib


@needs_cc
def test_png_writer_decodes_with_zlib(tmp_path):
    r = subprocess.run(["make", "-C", str(LIBFT), "libft.a"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    exe = LIBFT / "test" / "png_dump"
    r = subprocess.run(["cc", "-std=c99", "-I", str(LIBFT), "-o", str(exe),
                        str(LIBFT / "test" / "png_dump.c"),
                        str(LIBFT / "libft.a"), "-lm"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = tmp_path / "t.png"
    subprocess.run([str(exe), str(out)], check=True)
    data = out.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, []
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        crc = int.from_bytes(data[pos + 8 + n:pos + 12 + n], "big")
        assert zlib.crc32(typ + body) == crc, typ
        chunks.append((typ, body))
        pos += 12 + n
    assert [c[0] for c in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    ihdr = chunks[0][1]
    assert int.from_bytes(ihdr[:4], "big") == 3
    assert int.from_bytes(ihdr[4:8], "big") == 2
    assert ihdr[8:] == bytes([8, 2, 0, 0, 0])
    assert zlib.decompress(chunks[1][1]) == bytes(
        [0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
         0, 10, 20, 30, 40, 50, 60, 70, 80, 90])


import zipfile


@needs_cc
def test_zip_opens_with_zipfile(tmp_path):
    r = subprocess.run(["make", "-C", str(LIBFT), "test/test_zip"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    out = tmp_path / "t.zip"
    subprocess.run([str(LIBFT / "test" / "test_zip"), str(out)], check=True)
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None              # every CRC right
        assert z.namelist() == ["frame01.jpg", "frame01.jpg.txt"]
        assert z.read("frame01.jpg") == b"\xff\xd8\xff\xd9"
        assert z.read("frame01.jpg.txt") == b"emulsion=x\n"
        info = z.getinfo("frame01.jpg")
        assert info.compress_type == zipfile.ZIP_STORED
        assert info.date_time == (1980, 1, 1, 0, 0, 0)


import re

TOOLS = LIBFT.parent / "tools"
BUILD_BAT = TOOLS / "poc" / "build.bat"
PROJECT_SYM = re.compile(r"(?:ft|ring|worker|sc)_\w+")
needs_nm = pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("nm") is None,
    reason="needs a C compiler and nm")


def _bat_sources(line):
    root = {"LIBFT": LIBFT, "DETAIL": TOOLS / "detail",
            "SANCHO": LIBFT.parent / "sancho"}
    return sorted({root[d] / f for d, f in
                   re.findall(r"%(LIBFT|DETAIL|SANCHO)%\\(\w+\.c)", line)})


@needs_nm
def test_build_bat_links(tmp_path):
    """build.bat lists ftpoc's sources by hand and only the box builds it,
    so compile its list here and require every project symbol to resolve."""
    lines = [l for l in BUILD_BAT.read_text().splitlines()
             if "ftpoc.c" in l and not l.lstrip().upper().startswith("REM")]
    assert len(lines) == 2   # cl and mingw
    # the Windows transport (ftio.h); ftio_posix.c is the Makefile's
    assert all(" %FTIO%\\ftio_win.c " in l for l in lines)
    srcs = _bat_sources(lines[0])
    assert srcs == _bat_sources(lines[1])
    objs = []
    for s in srcs:
        o = tmp_path / (s.stem + ".o")
        r = subprocess.run(["cc", "-std=c99", "-c", "-I", str(LIBFT),
                            "-I", str(TOOLS / "detail"),
                            "-I", str(LIBFT.parent / "ftio"),
                            "-o", str(o), str(s)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        objs.append(str(o))

    def syms(*flags):
        r = subprocess.run(["nm", *flags, *objs], capture_output=True,
                           text=True, check=True)
        return {f.split()[-1] for f in r.stdout.splitlines()
                if f.strip() and not f.endswith(":")}

    # ftpoc.c needs <windows.h>: take its calls from the source text.
    poc = (TOOLS / "poc" / "ftpoc.c").read_text()
    wanted = syms("-u") | set(re.findall(r"\b(%s)\s*\(" % PROJECT_SYM.pattern,
                                         poc))
    missing = {s for s in wanted - syms("--defined-only")
               if PROJECT_SYM.fullmatch(s)}
    assert not missing, f"build.bat lacks the sources of {sorted(missing)}"
