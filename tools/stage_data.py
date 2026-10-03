#!/usr/bin/env python3
"""Put together the data directory the port runs from (on the 3DS: sdmc:/3ds/noma/).

    tools/stage_data.py OUT_DIR [--wad-dir extracted/wad] [--rfl-db RFL_DB.dat]

OUT_DIR/main.dol         the channel's program (orig/HCIJ/sys/main.dol)
OUT_DIR/content/N.app    the channel's contents by index (decrypted, from tools/wad_extract.py)
OUT_DIR/nand/            a minimal Wii NAND: system settings (SYSCONF, setting.txt) for a
                         Japanese console, the channel's save directory, and the Mii database
                         if you give one (--rfl-db, from your Wii's /shared2/menu/FaceLib)
Files already in OUT_DIR/nand (saves) are kept.
"""
import argparse
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sysconf_items():
    """(name, type, value) for SYSCONF. Types: 1 big array, 2 small array, 3 byte, 4 short,
    5 long, 7 bool."""
    return [
        ("IPL.LNG", 3, 0),            # language: Japanese
        ("IPL.AR", 3, 1),             # 16:9
        ("IPL.E60", 3, 1),
        ("IPL.PGS", 3, 0),
        ("IPL.SND", 3, 1),            # stereo
        ("IPL.SSV", 3, 1),            # screen saver on
        ("IPL.IDL", 2, b"\x00\x01"),  # idle mode
        ("IPL.CB", 5, 0),             # RTC counter bias
        ("IPL.CD", 7, 1),             # initial setup done
        ("IPL.CD2", 7, 1),
        ("IPL.EULA", 7, 1),
        ("IPL.UPT", 3, 2),
        ("IPL.SADR", 1, bytes([0x01]) + bytes(0x1007 - 1)),  # country: Japan
        ("IPL.NIK", 2, "Wii".encode("utf-16-be") + bytes(0x16 - 6)),
        ("IPL.PC", 2, bytes(0x4A)),   # parental controls off
        ("IPL.DH", 3, 0),
        ("IPL.INC", 3, 4),
        ("IPL.FRC", 5, 0x28),
        ("IPL.TID", 5, 0),
        ("IPL.ARN", 3, 0),
        ("BT.BAR", 3, 1),             # sensor bar above the screen
        ("BT.SENS", 5, 3),
        ("BT.SPKV", 3, 0x58),
        ("BT.MOT", 3, 1),
        ("BT.CDIF", 1, bytes(0x204)),
        ("BT.DINF", 1, bt_dinf()),
        ("NET.CTPC", 5, 0),
        ("NET.WCPC", 5, 0),
        ("NET.WCFG", 5, 1),           # WiiConnect24 on
        ("DEV.BTM", 3, 0),
        ("DEV.VIM", 3, 0),
        ("DEV.CTC", 3, 0),
        ("DEV.DSM", 3, 0),
        ("MPLS.MOVIE", 7, 1),
    ]


# the emulated Wii Remote (runtime/hw/ios/wiimote.cpp), in HCI byte order
WIIMOTE_BDADDR = bytes([0x11, 0x02, 0x19, 0x79, 0x00, 0x00])


def bt_dinf():
    """Paired Wii Remotes: a count, 10 registered and 4 active entries (address, most
    significant byte first, and a 0x40-byte name), the Balance Board, padding."""
    entry = WIIMOTE_BDADDR[::-1] + b"Nintendo RVL-CNT-01".ljust(0x40, b"\0")
    empty = bytes(0x46)
    data = bytes([1]) + entry + empty * 9 + entry + empty * 3 + empty
    return data + bytes(0x461 - len(data))


def make_sysconf():
    items = []
    for name, typ, val in sysconf_items():
        n = name.encode()
        head = bytes([(typ << 5) | (len(n) - 1)]) + n
        if typ == 1:
            data = struct.pack(">H", len(val) - 1) + val
        elif typ == 2:
            data = bytes([len(val) - 1]) + val
        elif typ in (3, 7):
            data = bytes([val])
        elif typ == 4:
            data = struct.pack(">H", val)
        elif typ == 5:
            data = struct.pack(">I", val)
        else:
            data = struct.pack(">Q", val)
        items.append(head + data)
    count = len(items)
    header = b"SCv0" + struct.pack(">H", count)
    offset = len(header) + 2 * (count + 1)
    offsets = []
    body = b""
    for it in items:
        offsets.append(offset + len(body))
        body += it
    offsets.append(offset + len(body))
    out = header + b"".join(struct.pack(">H", o) for o in offsets) + body
    assert len(out) < 0x4000 - 4
    out += bytes(0x4000 - 4 - len(out)) + b"SCed"
    return out


def make_setting_txt():
    text = ("AREA=JPN\r\nMODEL=RVL-001(JPN)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LJF\r\n"
            "SERNO=100000000\r\nVIDEO=NTSC\r\nGAME=JP\r\n").encode()
    data = bytearray(text + bytes(0x100 - len(text)))
    key = 0x73B5DBFA
    for i in range(len(data)):
        data[i] ^= key & 0xFF
        key = ((key << 1) | (key >> 31)) & 0xFFFFFFFF
    return bytes(data)


# ---------------------------------------------------------------------------------------------
# WiiConnect24's files in /shared2/wc24, as the System Menu sets them up (formats as the
# channel's NWC24 library checks them): the configuration with a Wii number, the friend lists,
# the download task list and the mailbox control files.
def nwc24_make_user_id(hollywood_id, id_ctr, hardware_model=1, area_code=0):
    """The 16-digit Wii number of a console (the scrambling the system uses to make it)."""
    table2 = [0x1, 0x5, 0x0, 0x4, 0x2, 0x3, 0x6, 0x7]
    table1 = [0x4, 0xB, 0x7, 0x9, 0xF, 0x1, 0xD, 0x3, 0xC, 0x2, 0x6, 0xE, 0x8, 0x0, 0xA, 0x5]
    M64 = (1 << 64) - 1
    mix = (area_code << 50) | (hardware_model << 47) | (hollywood_id << 15) | (id_ctr << 10)
    copy1 = mix
    for ctr in range(43):
        if (mix >> (52 - ctr)) & 1:
            mix ^= 0x635 << (42 - ctr)
    mix = (copy1 | (mix & 0xFFFFFFFF)) ^ 0x0000B3B3B3B3B3B3
    mix = ((mix >> 10) | ((mix & 0x3FF) << (11 + 32))) & M64
    for ctr in range(6):
        b = (mix >> (ctr * 8)) & 0xFF
        nb = (table1[(b >> 4) & 0xF] << 4) | table1[b & 0xF]
        mix = (mix & ~(0xFF << (ctr * 8)) & M64) | (nb << (ctr * 8))
    copy2 = mix
    for ctr in range(6):
        b = (copy2 >> (ctr * 8)) & 0xFF
        sh = table2[ctr] * 8
        mix = (mix & ~(0xFF << sh) & M64) | (b << sh)
    mix &= 0x001FFFFFFFFFFFFF
    mix = (mix << 1) | ((mix >> 52) & 1)
    mix ^= 0x00005E5E5E5E5E5E
    return mix & 0x001FFFFFFFFFFFFF


def make_nwc24msg_cfg(user_id):
    urls = [b"https://amw.wc24.wii.com/cgi-bin/account.cgi",
            b"http://rcw.wc24.wii.com/cgi-bin/check.cgi",
            b"https://mtw.wc24.wii.com/cgi-bin/receive.cgi",
            b"https://mtw.wc24.wii.com/cgi-bin/delete.cgi",
            b"https://mtw.wc24.wii.com/cgi-bin/send.cgi"]
    d = bytearray(1024)
    struct.pack_into(">4sIQII", d, 0, b"WcCf", 8, user_id, 0, 2)  # id generation 0, registered
    email = ("w%016d@wii.com" % user_id).encode()
    d[0x18:0x18 + len(email)] = email
    for i, u in enumerate(urls):
        d[0x9C + 0x80 * i:0x9C + 0x80 * i + len(u)] = u
    struct.pack_into(">I", d, 0x3F8, 1)  # enable booting
    checksum = sum(struct.unpack(">255I", bytes(d[:0x3FC]))) & 0xFFFFFFFF
    struct.pack_into(">I", d, 0x3FC, checksum)
    return bytes(d)


def make_wc24_files(nand):
    wc24 = os.path.join(nand, "shared2/wc24")
    os.makedirs(os.path.join(wc24, "mbox"), exist_ok=True)
    files = {}
    cfg = make_nwc24msg_cfg(nwc24_make_user_id(0x0BADC0DE & 0xFFFFFFFF, 0))
    files["nwc24msg.cfg"] = cfg
    files["nwc24msg.cbk"] = cfg
    fl = bytearray(0x360 + 100 * 0x140)
    struct.pack_into(">4sIII", fl, 0, b"WcFl", 2, 100, 0)
    files["nwc24fl.bin"] = bytes(fl)
    fls = bytearray(0x800)
    struct.pack_into(">4sI", fls, 0, b"WcFs", 2)
    files["nwc24fls.bin"] = bytes(fls)
    dl = bytearray(0x800 + 120 * 0x200)
    struct.pack_into(">4sIIIHHH", dl, 0, b"WcDl", 1, 0, 0, 32, 8, 120)
    files["nwc24dl.bin"] = bytes(dl)
    for box in ("wc24send", "wc24recv"):
        ctl = bytearray(0x80 * 0x40)
        struct.pack_into(">4sI", ctl, 0, b"WcTf", 4)
        files["mbox/%s.ctl" % box] = bytes(ctl)
        files["mbox/%s.mbx" % box] = bytes(0x4000)
    for name, data in files.items():
        path = os.path.join(wc24, name)
        if not os.path.exists(path):  # keep a console's own files
            open(path, "wb").write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--wad-dir", default=os.path.join(ROOT, "extracted/wad"))
    ap.add_argument("--dol", default=os.path.join(ROOT, "orig/HCIJ/sys/main.dol"))
    ap.add_argument("--rfl-db")
    a = ap.parse_args()

    os.makedirs(os.path.join(a.out, "content"), exist_ok=True)
    shutil.copyfile(a.dol, os.path.join(a.out, "main.dol"))

    tmd = open(os.path.join(a.wad_dir, "tmd.bin"), "rb").read()
    n = struct.unpack(">H", tmd[0x1DE:0x1E0])[0]
    for i in range(n):
        cid, idx = struct.unpack(">IH", tmd[0x1E4 + i * 0x24:0x1E4 + i * 0x24 + 6])
        src = os.path.join(a.wad_dir, "%08x.app" % cid)
        dst = os.path.join(a.out, "content", "%08x.app" % idx)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copyfile(src, dst)

    nand = os.path.join(a.out, "nand")
    for d in ("shared2/sys", "shared2/menu/FaceLib", "title/00000001/00000002/data",
              "title/00010001/4843494a/data", "tmp", "import", "meta", "ticket", "sys"):
        os.makedirs(os.path.join(nand, d), exist_ok=True)
    open(os.path.join(nand, "shared2/sys/SYSCONF"), "wb").write(make_sysconf())
    open(os.path.join(nand, "title/00000001/00000002/data/setting.txt"), "wb").write(make_setting_txt())
    make_wc24_files(nand)
    if a.rfl_db:
        shutil.copyfile(a.rfl_db, os.path.join(nand, "shared2/menu/FaceLib/RFL_DB.dat"))
    print("staged %s: main.dol, %d contents, nand/" % (a.out, n))


if __name__ == "__main__":
    main()
