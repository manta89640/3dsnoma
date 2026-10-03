#!/usr/bin/env python3
"""Unpack an installable Wii WAD (your own NAND dump) into its decrypted contents.

    tools/wad_extract.py wii-no-ma-jp.wad extracted/wad --key KEYFILE

Writes cert.bin, ticket.bin, tmd.bin, footer.bin, one <content id>.app per content
(decrypted, trimmed to the size in the TMD) and info.txt. Every content is checked
against the SHA-1 in the TMD, so a wrong key or a damaged dump fails loudly.

The common key is not part of this repository. Give it from your own console:
  --key keys.bin     the keys.bin of a BootMii NAND backup (common key at 0x114), or
  --key FILE         a 16-byte file holding the key, or
  WII_COMMON_KEY=<32 hex digits> in the environment.

AES is done with the openssl command line tool, so no Python packages are needed.
"""
import hashlib
import os
import struct
import subprocess
import sys


def load_common_key(path):
    if path:
        raw = open(path, "rb").read()
        if len(raw) == 16:
            return raw
        if len(raw) >= 0x124:  # BootMii keys.bin
            return raw[0x114:0x124]
        raise SystemExit("%s: expected a 16-byte key or a BootMii keys.bin" % path)
    env = os.environ.get("WII_COMMON_KEY", "")
    if len(env) == 32:
        return bytes.fromhex(env)
    raise SystemExit("no common key: pass --key (BootMii keys.bin) or set WII_COMMON_KEY")


def align(n, a=0x40):
    return (n + a - 1) // a * a


def aes_cbc_decrypt(key, iv, data):
    if len(data) % 16:
        data = data + b"\0" * (16 - len(data) % 16)
    r = subprocess.run(
        ["openssl", "enc", "-d", "-aes-128-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
        input=data, capture_output=True, check=True)
    return r.stdout


def main():
    args = sys.argv[1:]
    key_path = None
    if "--key" in args:
        i = args.index("--key")
        key_path = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        sys.exit(1)
    wad_path, out_dir = args
    os.makedirs(out_dir, exist_ok=True)
    wad = open(wad_path, "rb").read()

    hdr_size, wad_type, wad_ver, cert_size, _res, tik_size, tmd_size, data_size, footer_size = \
        struct.unpack(">IHHIIIIII", wad[:0x20])
    assert hdr_size == 0x20, "not a WAD"
    pos = align(hdr_size)
    cert = wad[pos:pos + cert_size]; pos = align(pos + cert_size)
    tik = wad[pos:pos + tik_size]; pos = align(pos + tik_size)
    tmd = wad[pos:pos + tmd_size]; pos = align(pos + tmd_size)
    data_off = pos
    footer = wad[align(data_off + data_size):align(data_off + data_size) + footer_size]

    for name, blob in (("cert.bin", cert), ("ticket.bin", tik), ("tmd.bin", tmd), ("footer.bin", footer)):
        open(os.path.join(out_dir, name), "wb").write(blob)

    # Ticket: decrypt the title key with the common key, IV = title id + 8 zero bytes.
    enc_title_key = tik[0x1BF:0x1CF]
    tik_title_id = tik[0x1DC:0x1E4]
    key_index = tik[0x1F1]
    if key_index != 0:
        raise SystemExit("this ticket uses common key %d (Korean/vWii); not supported" % key_index)
    common_key = load_common_key(key_path)
    title_key = aes_cbc_decrypt(common_key, tik_title_id + b"\0" * 8, enc_title_key)

    # TMD header
    ios_tid, title_id, title_type, group_id = struct.unpack(">QQIH", tmd[0x184:0x19A])
    region = struct.unpack(">H", tmd[0x19C:0x19E])[0]
    access_rights, title_ver, n_contents, boot_index = struct.unpack(">IHHH", tmd[0x1D8:0x1E2])

    tid_hex = "%016x" % title_id
    lo = struct.pack(">I", title_id & 0xFFFFFFFF)
    lines = [
        "WAD type: %s, version %d" % (struct.pack(">H", wad_type).decode("latin1"), wad_ver),
        "title id: %s (%s)" % (tid_hex, lo.decode("latin1", "replace")),
        "title version: %d" % title_ver,
        "IOS: %016x (IOS%d)" % (ios_tid, ios_tid & 0xFFFFFFFF),
        "title type: %08x, group id: %04x, region: %d" % (title_type, group_id, region),
        "access rights: %08x" % access_rights,
        "common key index: %d" % key_index,
        "contents: %d, boot index: %d" % (n_contents, boot_index),
    ]

    ok = True
    for i in range(n_contents):
        rec = tmd[0x1E4 + i * 0x24:0x1E4 + (i + 1) * 0x24]
        cid, idx, ctype, size = struct.unpack(">IHHQ", rec[:16])
        sha1 = rec[16:36]
        enc = wad[pos:pos + align(size, 16)]
        pos = align(pos + align(size, 16))
        dec = aes_cbc_decrypt(title_key, struct.pack(">H", idx) + b"\0" * 14, enc)[:size]
        good = hashlib.sha1(dec).digest() == sha1
        ok &= good
        fname = "%08x.app" % cid
        open(os.path.join(out_dir, fname), "wb").write(dec)
        lines.append("  [%2d] id %08x type %04x size %10d %s%s  magic %s" % (
            idx, cid, ctype, size, "sha1 OK " if good else "SHA1 BAD",
            " (boot)" if idx == boot_index else "", dec[:8].hex()))

    info = "\n".join(lines) + "\n"
    open(os.path.join(out_dir, "info.txt"), "w").write(info)
    print(info, end="")
    if not ok:
        print("one or more contents failed the SHA-1 check", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
