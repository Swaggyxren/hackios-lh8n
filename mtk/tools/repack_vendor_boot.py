#!/usr/bin/env python3
"""
repack_vendor_boot.py: Append an XNU payload to the Android Boot Header v4
vendor_boot ramdisk so MediaTek LK loads it directly into DRAM via UFS DMA.
"""
import os
import struct
import sys

PAGE = 4096


def pages(n):
    return (n + PAGE - 1) // PAGE * PAGE


def repack(vendor_boot_path, payload_path, out_path):
    with open(vendor_boot_path, 'rb') as f:
        vb = f.read()

    assert vb[:8] == b'VNDRBOOT', f'{vendor_boot_path} is not a vendor_boot image'
    hver = struct.unpack_from('<I', vb, 8)[0]
    assert hver == 4, f'Expected header v4, got v{hver}'

    vrsize = struct.unpack_from('<I', vb, 24)[0]
    dtb_size = struct.unpack_from('<I', vb, 0x834)[0]
    vr_table_size = struct.unpack_from('<I', vb, 0x840)[0]

    with open(payload_path, 'rb') as f:
        payload = f.read()
    assert payload[:4] == b'XNUP', f'{payload_path} does not start with XNUP magic'

    hdr = vb[:PAGE]
    orig_ramdisk = vb[PAGE : PAGE + vrsize]

    dtb_off = PAGE + pages(vrsize)
    dtb = vb[dtb_off : dtb_off + dtb_size]

    table_off = dtb_off + pages(dtb_size)
    table = vb[table_off : table_off + vr_table_size]

    # In DRAM, vendor ramdisk starts first at 0x51100000 (LK: vendor_boot.ramdisk_addr).
    # Pad orig_ramdisk so payload lands at a 4096-aligned DRAM address (0x52f0e000).
    rd_dram_base = 0x51100000
    orig_end = rd_dram_base + vrsize
    target_dram = (orig_end + PAGE - 1) & ~(PAGE - 1)  # 0x52f0e000
    pad_bytes = target_dram - orig_end

    new_ramdisk = orig_ramdisk + (b'\0' * pad_bytes) + payload
    new_vrsize = len(new_ramdisk)

    new_hdr = bytearray(hdr)
    struct.pack_into('<I', new_hdr, 24, new_vrsize)

    out = bytearray()
    out += new_hdr
    out += new_ramdisk + (b'\0' * (-len(new_ramdisk) % PAGE))
    out += dtb + (b'\0' * (-len(dtb) % PAGE))
    out += table + (b'\0' * (-len(table) % PAGE))

    max_part_size = 64 << 20
    assert len(out) <= max_part_size, f'Result {len(out)} exceeds 64MB partition'

    with open(out_path, 'wb') as f:
        f.write(out)

    print(f'Repacked {vendor_boot_path} + {payload_path} -> {out_path}')
    print(f'  Original vrsize: {vrsize} (0x{vrsize:x})')
    print(f'  Payload size:    {len(payload)} (0x{len(payload):x})')
    print(f'  New vrsize:      {new_vrsize} (0x{new_vrsize:x})')
    print(f'  DRAM payload PA: 0x{target_dram:x}')
    print(f'  Total img size:  {len(out)} bytes ({len(out)/1024/1024:.2f} MB / 64 MB)')


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(f'Usage: {sys.argv[0]} <vendor_boot.img> <payload.bin> <out.img>')
        sys.exit(1)
    repack(sys.argv[1], sys.argv[2], sys.argv[3])
