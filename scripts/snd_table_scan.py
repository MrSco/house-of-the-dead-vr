#!/usr/bin/env python3
"""Find the sound 68000's computed jumps into tables (SCSP board, program ROM at
0x600000) and print every table entry as a seed for seeds/<set>_snd.txt.

  snd_table_scan.py SOUND_PROGRAM.bin

The pattern (vcop2_snd.txt's notes): "lea (d8,PC,Dn),An; jmp (An)" with Dn masked
just before by "andi #mask,Dn", into a table of bra.b (2-byte) or bra.w (4-byte)
entries; the table has (mask / step) + 1 entries, the last ones possibly code
rather than a bra. Needs capstone (pip install capstone).
"""
import struct
import sys

import capstone

BASE = 0x600000


def main():
    d = open(sys.argv[1], "rb").read()
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_000)
    seeds = set()
    for off in range(0, len(d) - 8, 2):
        w0, w1, w2 = struct.unpack_from(">HhH", d, off)
        # lea (d8,PC,Xn),An: 0100 aaa1 1111 1011; extension word: Xn and d8
        if (w0 & 0xf1ff) != 0x41fb:
            continue
        an = (w0 >> 9) & 7
        if w2 != (0x4ed0 | an):  # jmp (An)
            continue
        ext = w1 & 0xffff
        xn = (ext >> 12) & 7
        disp = struct.unpack(">b", bytes([ext & 0xff]))[0]
        table = BASE + off + 2 + disp
        # the mask: an andi on Dn within the 8 words before
        mask = None
        for back in range(2, 18, 2):
            for i in md.disasm(d[off - back:off], BASE + off - back):
                if i.mnemonic.startswith("andi") and i.op_str.endswith("d%d" % xn) and i.address + i.size <= BASE + off:
                    mask = int(i.op_str.split(",")[0].lstrip("#$"), 16)
            if mask is not None:
                break
        t = table - BASE
        first = struct.unpack_from(">H", d, t)[0]
        step = 4 if first == 0x6000 else 2
        count = (mask // step + 1) if mask is not None else 0
        if not count:  # no mask found: the bra entries in a row
            while t + step * count < len(d) and (struct.unpack_from(">H", d, t + step * count)[0] & 0xff00) == 0x6000:
                count += 1
        print("# %06x: lea (%d,PC,D%d),A%d; jmp (A%d): table %06x, %d entries of %d bytes (mask %s)"
              % (BASE + off, disp, xn, an, an, table, count, step, hex(mask) if mask is not None else "?"))
        for k in range(min(count, 128)):
            seeds.add(table + step * k)
    # The second pattern: "move.w (d8,PC,Xn),Dn; jmp (d8,PC,Xn)" (0x3x3b, then 0x4efb): a table
    # of 16-bit offsets from its own start, its size from the bound checked before
    # ("cmpi #N; bgt/bhi": N + 1 entries), else while the entries land on code.
    for off in range(0, len(d) - 8, 2):
        w0, e0, w1, e1 = struct.unpack_from(">HHHH", d, off)
        if (w0 & 0xf1ff) != 0x303b or w1 != 0x4efb:
            continue
        table = BASE + off + 2 + struct.unpack(">b", bytes([e0 & 0xff]))[0]
        if BASE + off + 6 + struct.unpack(">b", bytes([e1 & 0xff]))[0] != table:
            continue
        bound = None
        for back in range(2, 24, 2):
            for i in md.disasm(d[off - back:off], BASE + off - back):
                if i.mnemonic.startswith("cmpi") and i.address + i.size <= BASE + off:
                    bound = int(i.op_str.split(",")[0].lstrip("#$"), 16)
            if bound is not None:
                break
        t = table - BASE
        count = bound + 1 if bound is not None and bound < 256 else 0
        if not count:
            while count < 256:
                o = struct.unpack_from(">h", d, t + 2 * count)[0]
                if (o & 1) or not (0 <= t + o < len(d)):
                    break
                count += 1
        targets = {table + struct.unpack_from(">h", d, t + 2 * k)[0] for k in range(count)}
        targets = {x for x in targets if not x & 1 and BASE <= x < BASE + len(d)}  # (code is even and in the ROM)
        print("# %06x: move.w (tbl,PC,Xn),Dn; jmp (tbl,PC,Dn): table %06x, %d entries, %d targets"
              % (BASE + off, table, count, len(targets)))
        seeds |= targets
    for s in sorted(seeds):
        print("%08x" % s)


if __name__ == "__main__":
    main()
