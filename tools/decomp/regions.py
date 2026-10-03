"""Which code is decompiled, which is replaced by native 3DS code, which is stubbed.

Addresses are text ranges of main.dol (HCIJ v1025). The library anchors come from each
library's version-string registration (see docs/library_map.md).
"""

# (start, end, name, treatment)   treatment: decomp | native | stub
REGIONS = [
    (0x80004000, 0x80006740, "init", "native"),        # __start, memcpy, memset (.init)
    (0x8000DEA0, 0x802B46E8, "app", "decomp"),         # Wii no Ma itself
    (0x802B46E8, 0x802CA7C8, "msl", "native"),         # MSL C/C++ runtime
    (0x802CA7C8, 0x802CF870, "trk", "stub"),           # MetroTRK debugger
    (0x802CF870, 0x802DF280, "os", "native"),
    (0x802DF280, 0x802E1370, "exi", "decomp"),
    (0x802E1370, 0x802E33E0, "si", "decomp"),
    (0x802E33E0, 0x802E9390, "vi", "native"),
    (0x802E9390, 0x802F13E0, "gx", "native"),
    (0x802F13E0, 0x802FA330, "dvd", "native"),
    (0x802FA330, 0x802FA7A0, "ai", "native"),
    (0x802FA7A0, 0x80300AB0, "ax", "native"),
    (0x80300AB0, 0x803016E0, "dsp", "native"),
    (0x803016E0, 0x80306370, "nand", "native"),
    (0x80306370, 0x80308B90, "sc", "native"),
    (0x80308B90, 0x8030E8D0, "enc", "native"),
    (0x8030E8D0, 0x8030F510, "arc", "native"),
    (0x8030F510, 0x80310B60, "esp", "native"),
    (0x80310B60, 0x80315200, "ipc", "native"),
    (0x80315200, 0x80317E80, "pad", "native"),
    (0x80317E80, 0x80330C00, "wpad", "native"),
    (0x80330C00, 0x80363C60, "kpad_bt", "native"),
    (0x80363C60, 0x80368950, "cnt", "native"),
    (0x80368950, 0x8038A6C0, "hbm", "decomp"),
    (0x8038A6C0, 0x80390360, "pd_kpr_kbd_hid", "decomp"),
    (0x80390360, 0x80394ED0, "rso", "stub"),
    (0x80394ED0, 0x803A4910, "jpeg", "native"),
    (0x803A4910, 0x8040A000, "net", "decomp"),           # NCD MP NWC24 REX NHTTP SSL SO FCD EC DWCDL
    (0x8040A000, 0x804922E0, "nw4r", "decomp"),        # EF G3D LYT SND (+ ut, math, heap checks)
    # the content players, left out (runtime/patches/players.cpp): nothing starts them
    (0x804922E0, 0x804C4D90, "mobiclip", "stub"),       # Actimagine MobiClip (video)
    (0x804C4D90, 0x804CF4DC, "rfl", "decomp"),          # from RFLGetWorkSize
    (0x804CF4DC, 0x80520000, "atok", "decomp"),
    (0x80520000, 0x8055BB2C, "swkbd", "decomp"),        # (its last function: 0x8055BA10)
    (0x8055BB2C, 0x806D50A0, "flash", "stub"),          # Adobe Flash
]


def region_of(addr):
    for s, e, name, how in REGIONS:
        if s <= addr < e:
            return name, how
    return None, None
