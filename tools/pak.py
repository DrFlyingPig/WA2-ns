#!/usr/bin/env python3
"""pak.py — WA2 PC 版归档工具(PACK / LAC + LZSS)

用法:
  python pak.py list   <archive>
  python pak.py extract <archive> [输出目录,默认 archive 同名目录]
  python pak.py pack    <输入目录> <输出.pac>   (生成 PACK,LZSS 压缩文本类,图片音频原样)

格式说明见 docs/formats.md。
"""
import os
import struct
import sys

LZSS_N = 0x1000
LZSS_F = 0x12
LZSS_THRESHOLD = 2


# ---------------- LZSS ----------------
def lzss_compress(data: bytes) -> bytes:
    """经典 LZSS(0x1000 环形缓冲,初值 0x20,写指针 0xFEE,匹配长度 3-18)

    输出流:[inlim u32][outlim u32] 之后每 8 个符号一组:1 标志字节 + 符号,
    标志位 LSB 在前,1=单字节数据,0=双字节回溯引用。
    """
    ring = bytearray(b"\x20" * LZSS_N)
    r = LZSS_N - LZSS_F
    body = bytearray()
    n = len(data)
    i = 0
    while i < n:
        # 收集 8 个符号
        flag = 0
        chunk = bytearray()
        for bit in range(8):
            if i >= n:
                break
            match_len, match_pos = 0, 0
            if i + 2 < n:
                best, bestp = 0, 0
                for p in range(LZSS_N):
                    if p == r:
                        continue
                    l = 0
                    while l < LZSS_F and i + l < n and ring[(p + l) % LZSS_N] == data[i + l]:
                        l += 1
                    if l > best:
                        best, bestp = l, p
                        if best == LZSS_F:
                            break
                match_len, match_pos = best, bestp
            if match_len > LZSS_THRESHOLD:
                b1 = match_pos & 0xFF
                b2 = ((match_pos >> 4) & 0xF0) | (match_len - (LZSS_THRESHOLD + 1))
                chunk += bytes([b1, b2])
                for k in range(match_len):
                    ring[r] = data[i + k]
                    r = (r + 1) % LZSS_N
                i += match_len
            else:
                b = data[i]
                chunk.append(b)
                flag |= (1 << bit)
                ring[r] = b
                r = (r + 1) % LZSS_N
                i += 1
        body.append(flag)
        body += chunk
    out = struct.pack("<II", len(body), len(data)) + bytes(body)
    return out


def lzss_decompress(data: bytes) -> bytes:
    inlim, outlim = struct.unpack_from("<II", data, 0)
    ring = bytearray(b"\x20" * LZSS_N)
    r = LZSS_N - LZSS_F
    out = bytearray()
    i = 8
    while i < 8 + inlim and len(out) < outlim:
        flags = data[i]
        i += 1
        for bit in range(8):
            if i >= 8 + inlim or len(out) >= outlim:
                break
            if flags & 1:
                b = data[i]
                i += 1
                ring[r] = b
                r = (r + 1) % LZSS_N
                out.append(b)
            else:
                b1 = data[i]
                b2 = data[i + 1]
                i += 2
                pos = b1 | ((b2 & 0xF0) << 4)
                cnt = (b2 & 0x0F) + 3
                for _ in range(cnt):
                    b = ring[pos & 0xFFF]
                    pos += 1
                    ring[r] = b
                    r = (r + 1) % LZSS_N
                    out.append(b)
                    if len(out) >= outlim:
                        break
            flags >>= 1
    return bytes(out)


# ---------------- PACK ----------------
def read_pack(path):
    with open(path, "rb") as f:
        buf = f.read()
    magic = struct.unpack_from("<I", buf, 0)[0]
    if magic not in (0x5041434B, 0x4B434150):
        raise ValueError("not a PACK archive")
    n = struct.unpack_from("<I", buf, 12)[0]
    entries = []
    for i in range(n):
        off = 16 + i * 44
        crypted, = struct.unpack_from("<I", buf, off)
        name = buf[off + 4:off + 28].split(b"\0")[0].decode("cp932", "replace").lower()
        offset, size = struct.unpack_from("<II", buf, off + 36)
        entries.append((name, offset, size, crypted != 0))
    return buf, entries


def read_lac(path):
    with open(path, "rb") as f:
        buf = f.read()
    magic = struct.unpack_from("<I", buf, 0)[0]
    if magic != 0x0043414C:
        raise ValueError("not a LAC archive")
    n = struct.unpack_from("<I", buf, 4)[0]
    entries = []
    for i in range(n):
        off = 8 + i * 40
        raw = bytes(b ^ 0xFF if b else 0 for b in buf[off:off + 32])
        name = raw.split(b"\0")[0].decode("cp932", "replace").lower()
        size, offset = struct.unpack_from("<II", buf, off + 32)
        entries.append((name, offset, size, False))
    return buf, entries


TEXT_EXT = (".txt", ".bnr", ".dat", ".ini", ".csv", ".json")


def cmd_list(path):
    try:
        _, entries = read_pack(path)
    except ValueError:
        _, entries = read_lac(path)
    for name, off, size, comp in entries:
        print(f"{name:28s} off={off:10d} size={size:9d} {'LZSS' if comp else 'raw '}")


def cmd_extract(path, outdir=None):
    try:
        buf, entries = read_pack(path)
    except ValueError:
        buf, entries = read_lac(path)
    outdir = outdir or os.path.splitext(path)[0] + "_out"
    os.makedirs(outdir, exist_ok=True)
    for name, off, size, comp in entries:
        data = buf[off:off + size]
        if comp:
            data = lzss_decompress(data)
        dst = os.path.join(outdir, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True) if os.path.dirname(dst) else None
        with open(dst, "wb") as f:
            f.write(data)
    print(f"extracted {len(entries)} files -> {outdir}")


def cmd_pack(srcdir, outpath):
    pack_dir(srcdir, outpath)


def pack_dir(srcdir, outpath):
    names = []
    for root, _dirs, files in os.walk(srcdir):
        for fn in files:
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, srcdir).replace("\\", "/").lower()
            names.append((rel, full))
    names.sort()
    header_size = 16 + len(names) * 44
    blob = bytearray()
    index = bytearray()
    index += struct.pack("<I", 0x4B434150)  # LE 读出即 0x5041434B
    index += struct.pack("<Q", 0)           # 保留
    index += struct.pack("<I", len(names))
    for rel, full in names:
        with open(full, "rb") as f:
            data = f.read()
        ext = os.path.splitext(rel)[1]
        if ext in TEXT_EXT:
            data = lzss_compress(data)
            comp = 1
        else:
            comp = 0
        off = header_size + len(blob)
        blob += data
        nb = rel.encode("cp932", "replace")[:23]
        index += struct.pack("<I", comp)
        index += nb + b"\0" * (24 - len(nb))
        index += struct.pack("<Q", 0)
        index += struct.pack("<II", off, len(data))
    with open(outpath, "wb") as f:
        f.write(index + blob)
    print(f"packed {len(names)} files -> {outpath}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list(sys.argv[2])
    elif cmd == "extract":
        cmd_extract(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    elif cmd == "pack":
        cmd_pack(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
