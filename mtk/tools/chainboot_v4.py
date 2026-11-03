#!/usr/bin/env python3
"""
chainboot_v4.py: Put a bare-metal program (mtkloader / fbdiag) in front of Android's kernel
in an Android boot image supporting Boot Header v4 (GKI 2.0 / Dimensity / MT6833) and v2.

Usage:
  chainboot_v4.py <android-boot.img> <program.bin> <out.img> [payload.bin]
"""
import gzip
import hashlib
import struct
import sys

CHAIN_OFFSET = 0x40000
BOOT_PARTITION_V4 = 40 << 20  # 40 MB partition on LH8n / MT6833
BOOT_PARTITION_V2 = 32 << 20


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def build_v4(d, prog_path, out, payload):
    magic, ksize, rsize, os_ver, hdr_size, _, _, _, _, hver, cmdline, sig_size = struct.unpack_from(
        '<8sIIII4II1536sI', d, 0
    )
    assert hver == 4, f'Expected header v4, got v{hver}'
    page = 4096

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]
    off += pages(ksize)
    ramdisk = d[off:off + rsize]
    off += pages(rsize)
    signature = d[off:off + sig_size]

    # Decompress kernel if gzipped
    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, f'program too large ({len(prog)} > {CHAIN_OFFSET})'

    total = CHAIN_OFFSET + linux_image_size
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))

    orig_rsize = len(ramdisk)
    if payload:
        padded_ramdisk = pad(ramdisk, page)
        payload_off = len(padded_ramdisk)
        new_ramdisk = padded_ramdisk + payload
        struct.pack_into('<Q', prog, 0x20, orig_rsize)
        struct.pack_into('<Q', prog, 0x30, payload_off)
    else:
        new_ramdisk = ramdisk

    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image

    # Compress new kernel if original was compressed
    if kernel[:2] == b'\x1f\x8b':
        new_kernel = gzip.compress(blob, 9, mtime=0)
    else:
        new_kernel = blob

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))  # update kernel_size
    struct.pack_into('<I', hdr, 12, len(new_ramdisk))  # update ramdisk_size

    img = bytes(hdr) + pad(new_kernel, page) + pad(new_ramdisk, page) + pad(signature, page)
    if len(img) > BOOT_PARTITION_V4:
        raise SystemExit(f'{out}: {len(img)} bytes exceeds boot partition size {BOOT_PARTITION_V4}')

    open(out, 'wb').write(img)
    print(f'[v4] {out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, kernel {len(new_kernel)})')


def build_v2(d, prog_path, out, payload):
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from('<9I', d, 8)
    assert hver == 2, f'Expected header v2, got v{hver}'
    rdo_size = struct.unpack_from('<I', d, 1632)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0]

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]

    image = gzip.decompress(kernel) if kernel[:2] == b'\x1f\x8b' else kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in (new_kernel, ramdisk, second, rdo, dtb):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)

    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) + pad(second, page) + pad(rdo, page) + pad(dtb, page)
    if len(img) > BOOT_PARTITION_V2:
        raise SystemExit(f'{out}: {len(img)} bytes exceeds boot partition size {BOOT_PARTITION_V2}')
    open(out, 'wb').write(img)
    print(f'[v2] {out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, kernel gz {len(new_kernel)})')


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <android-boot.img> <program.bin> <out.img> [payload.bin]")
        sys.exit(1)
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    hver = struct.unpack_from('<I', d, 40)[0]
    if hver == 4:
        build_v4(d, prog_path, out, payload)
    else:
        build_v2(d, prog_path, out, payload)


if __name__ == '__main__':
    main()
