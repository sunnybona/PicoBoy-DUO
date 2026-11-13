#!/usr/bin/env python3
"""
merge_uf2.py — 把多个地址不重叠的 RP2040 UF2 合并成单个 UF2。

每个 UF2 块 512 字节、自带目标地址，块之间相互独立，
因此按 (目标地址, 块内偏移) 排序后直接拼接即为合法 UF2。
用法: python merge_uf2.py out.uf2 in1.uf2 in2.uf2 ...
"""
import struct
import sys

BLOCK_SIZE = 512
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC2 = 0x0AB16F30


def read_blocks(path):
    data = open(path, "rb").read()
    if len(data) % BLOCK_SIZE:
        raise SystemExit(f"{path}: 长度不是 512 的整数倍")
    blocks = []
    for off in range(0, len(data), BLOCK_SIZE):
        blk = data[off:off + BLOCK_SIZE]
        m0, m1, flags, addr, payload_len = struct.unpack_from("<IIIII", blk)
        (m2,) = struct.unpack_from("<I", blk, BLOCK_SIZE - 4)
        if (m0, m1, m2) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC2):
            raise SystemExit(f"{path}: 非法 UF2 块 @{off}")
        blocks.append((addr, blk))
    return blocks


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法: python merge_uf2.py out.uf2 in1.uf2 ...")
    out, inputs = sys.argv[1], sys.argv[2:]

    all_blocks = []
    for p in inputs:
        all_blocks.extend(read_blocks(p))

    # 检查地址区间是否重叠
    spans = []
    for p in inputs:
        blks = read_blocks(p)
        addrs = [a for a, _ in blks]
        spans.append((min(addrs), max(addrs) + 256, p))
    spans.sort()
    for i in range(1, len(spans)):
        if spans[i][0] < spans[i - 1][1]:
            raise SystemExit(
                f"地址重叠: {spans[i - 1][2]} 与 {spans[i][2]}")

    all_blocks.sort(key=lambda x: x[0])

    # 重写块序号（部分烧录工具会校验 blockNo 连续性）
    total = len(all_blocks)
    out_data = bytearray()
    for i, (_, blk) in enumerate(all_blocks):
        blk = bytearray(blk)
        struct.pack_into("<II", blk, 20, i, total)  # block_no, num_blocks
        out_data += blk

    open(out, "wb").write(out_data)
    print(f"合并 {total} 块 -> {out} ({len(out_data)} 字节)")
    for lo, hi, p in spans:
        print(f"  0x{lo:08X}..0x{hi:08X}  {p}")


if __name__ == "__main__":
    main()
