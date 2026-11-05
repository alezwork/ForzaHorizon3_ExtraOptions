"""Statically check the FH3 patch table in features/fh3.c against the game executable.

Every original byte string must match the supported executable, jump patches
must keep their native target, VIP patches must replace a call to a known VIP
getter, and treasure map getters must be referenced only by their registration.
"""
import os
import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from xbox_image import Image, FH3

SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "features", "fh3.c")
VIP_GETTERS = {0x10b97b0: "profile VIP getter", 0x2fdda0: "main values bool getter"}

src = open(SOURCE).read()
macros = dict(re.findall(r"#define (\w+) (\{[^}]*\})", src))
entry = r"\{(0x[0-9a-f]+), (\d), FH3_GROUP_(\w+), \"(\w+)\",\s*(\{[^}]*\}|\w+), (\{[^}]*\}|\w+)\}"
rows = re.findall(entry, src)


def parse(text):
    text = macros.get(text, text)
    return bytes(ord(x.strip()[1]) if x.strip().startswith("'") else int(x, 16)
                 for x in text.strip("{}").split(","))


image = Image(FH3)


def callers_of(target):
    return [c for c in image.refs_to(target) if image.read(c, 1) in (b"\xe8", b"\xe9")]

md = Cs(CS_ARCH_X86, CS_MODE_64)
ok = bool(rows)
for rva, size, group, name, orig, new in rows:
    rva, size = int(rva, 16), int(size)
    orig, new = parse(orig), parse(new)
    current = image.read(rva, size)
    good = current == orig and len(orig) == len(new) == size
    detail = ""
    first = next(md.disasm(current + b"\x90" * 8, rva))
    if group == "INTRO":
        patched = next(md.disasm(new + b"\x90" * 8, rva))
        same = int(first.op_str, 16) == int(patched.op_str, 16)
        good &= first.mnemonic.startswith("j") and patched.mnemonic == "jmp" and same
        detail = "%s %s -> %s %s" % (first.mnemonic, first.op_str, patched.mnemonic, patched.op_str)
    elif group == "VIP" and first.mnemonic == "call":
        target = int(first.op_str, 16)
        good &= target in VIP_GETTERS
        detail = "call %x (%s) -> mov eax, 1" % (target, VIP_GETTERS.get(target, "UNKNOWN"))
    elif group == "VIP":
        detail = "%s %s -> forced VIP arguments" % (first.mnemonic, first.op_str)
    elif group == "BLUR_RIMS":
        cave = int(re.search(r"#define FH3_BLUR_RIM_THRESHOLD_RVA (0x[0-9a-f]+)", src).group(1), 16)
        patched = next(md.disasm(new, rva))
        target = rva + patched.size + int(re.search(r"rip \+ (0x[0-9a-f]+)", patched.op_str).group(1), 16)
        cave_free = image.read(cave, 4) == b"\xcc" * 4 and not image.refs_to(cave)
        good &= target == cave and cave_free and "0x2d20" in first.op_str
        detail = "%s %s -> %s %s (cave %x free=%s)" % (first.mnemonic, first.op_str,
                                                       patched.mnemonic, patched.op_str, cave, cave_free)
    elif group == "GROOVE":
        groove = open(os.path.join(os.path.dirname(SOURCE), "fh3_groove.h")).read()
        g = {k: int(v, 16) for k, v in re.findall(r"#define (\w+) (0x[0-9a-f]+)u", groove)}
        patched = next(md.disasm(new, rva))
        target = int(first.op_str, 16)
        if target == 0x196fb20:
            # SongStream's media type setup, replaced by "mov al, 1" (result unused).
            good &= (first.mnemonic == "call" and new == bytes.fromhex("b001909090")
                     and image.read(rva + 5, 3) == bytes.fromhex("0fb6c0") and callers_of(target) == [rva])
            print("%-36s %08x %-4s %s" % (name, rva, "OK" if good else "FAIL", "call %s -> mov al, 1" % first.op_str))
            ok &= good
            continue
        routes = {g["FH3_GROOVE_MF_READER_RVA"]: ("call", g["FH3_GROOVE_MF_THUNK_RVA"]),
                  g["FH3_GROOVE_DISPATCHER_RVA"]: ("call", g["FH3_GROOVE_DISPATCHER_THUNK_RVA"]),
                  g["FH3_GROOVE_PLAYER_UPDATE_RVA"]: ("jmp", g["FH3_GROOVE_PLAYER_THUNK_RVA"])}
        kind, thunk = routes.get(target, ("?", 0))
        cave_free = image.read(thunk, 14) == b"\xcc" * 14 and not image.func_start(thunk)
        good &= (first.mnemonic == kind and patched.mnemonic == kind
                 and int(patched.op_str, 16) == thunk and cave_free)
        detail = "%s %s -> %s %s (thunk free=%s)" % (first.mnemonic, first.op_str,
                                                     patched.mnemonic, patched.op_str, cave_free)
    elif group == "MINIMAP":
        mm = open(os.path.join(os.path.dirname(SOURCE), "fh3_minimap.h")).read()
        g = {k: int(v, 16) for k, v in re.findall(r"#define (\w+) (0x[0-9a-f]+)u", mm)}
        patched = next(md.disasm(new, rva))
        thunk = g["FH3_MINIMAP_THUNK_RVA"]
        cave_free = image.read(thunk, 14) == b"\xcc" * 14 and not image.func_start(thunk)
        good &= (rva == g["FH3_MINIMAP_CALL_RVA"] and first.mnemonic == "call"
                 and int(first.op_str, 16) == g["FH3_MINIMAP_UPDATE_RVA"]
                 and patched.mnemonic == "call" and int(patched.op_str, 16) == thunk and cave_free
                 and image.func_start(rva)[0] == 0x120b170)
        detail = "call %s -> call %s (thunk free=%s)" % (first.op_str, patched.op_str, cave_free)
    elif group == "SUN_SHADOWS":
        detail = "%s %s -> %s" % (first.mnemonic, first.op_str,
                                  " ; ".join("%s %s" % (i.mnemonic, i.op_str)
                                             for i in md.disasm(new, rva)))
    elif group == "DLC":
        start = rva
        while image.read(start - 1, 1) != b"\0":
            start -= 1
        refs = image.refs_to(start)
        good &= len(refs) == 1
        detail = "%r refs=%d -> %r" % (image.cstr(start, 80), len(refs), new.decode())
    elif group == "TREASURE_MAP" and first.mnemonic != "sub":
        detail = "%s %s -> %s" % (first.mnemonic, first.op_str,
                                  " ; ".join("%s %s" % (i.mnemonic, i.op_str)
                                             for i in md.disasm(new, rva)))
    elif group == "TREASURE_MAP":
        refs = image.refs_to(rva)
        good &= len(refs) == 1
        detail = "getter refs=%d -> mov al, 1; ret" % len(refs)
    ok &= good
    print("%-36s %08x %-4s %s" % (name, rva, "OK" if good else "FAIL", detail))

# Facts features/fh3_lights.c relies on (its vtable patch is filled at runtime).
lights = open(os.path.join(os.path.dirname(SOURCE), "fh3_lights.c")).read()
lights += open(os.path.join(os.path.dirname(SOURCE), "fh3_lights.h")).read()
const = {k: int(v, 16) for k, v in re.findall(r"#define (\w+) (0x[0-9a-f]+)u", lights)}
base = image.pe.OPTIONAL_HEADER.ImageBase


def qword(rva):
    return int.from_bytes(image.read(rva, 8), "little")


def rtti_name(vtable):
    col = qword(vtable - 8) - base
    return image.cstr(int.from_bytes(image.read(col + 12, 4), "little") + 0x10, 80)


checks = [
    ("ForzaCar vtable", rtti_name(const["CAR_VTABLE_RVA"]) == ".?AVForzaCar@@"),
    ("CPlayer vtable", rtti_name(const["PLAYER_VTABLE_RVA"]) == ".?AVCPlayer@@"),
    ("car owner getter (FH2 0xf62270)",
     image.read(0x188e420, 8) == b"\x48\x8b\x81" + const["CAR_OWNER_OFFSET"].to_bytes(4, "little") + b"\xc3"),
    ("car driver setter (0x18b00d0)",
     image.read(0x18b00d6, 7) == b"\x48\x89\x91" + const["CAR_DRIVER_OFFSET"].to_bytes(4, "little")
     and image.read(0x1882a47, 5) == b"\xe8\x84\xd6\x02\x00"),
    ("car owner copied from driver (0x18b66b0)",
     image.read(0x18b66b3, 14) == b"\x48\x8b\x82" + const["CAR_DRIVER_OFFSET"].to_bytes(4, "little")
     + b"\x48\x89\x82" + const["CAR_OWNER_OFFSET"].to_bytes(4, "little")),
    ("CPlayer SetCar at vtable +0x288",
     qword(const["PLAYER_VTABLE_RVA"] + 0x288) - base == 0x3d1680
     and image.read(0x3d169f, 7) == b"\x48\x89\x93\xa0\x09\x00\x00"),
    ("car update slot -> native update",
     const["FH3_CAR_UPDATE_SLOT_RVA"] == const["CAR_VTABLE_RVA"] + 16 * 8
     and qword(const["FH3_CAR_UPDATE_SLOT_RVA"]) - base == const["FH3_CAR_UPDATE_RVA"]),
    ("manual headlight setter",
     image.read(const["SET_MANUAL_RVA"], 7) == b"\x88\x91" + const["MANUAL_OFFSET"].to_bytes(4, "little") + b"\xc3"),
    ("forced headlight setter",
     image.read(const["SET_FORCED_RVA"], 7) == b"\x88\x91" + const["FORCED_OFFSET"].to_bytes(4, "little") + b"\xc3"),
    ("automatic headlight reset",
     image.read(0x18ab0c0, 6) == b"\xc7\x81" + const["AUTOMATIC_OFFSET"].to_bytes(4, "little")),
    ("player car getter",
     image.read(const["GET_PLAYER_CAR_RVA"], 14) == bytes.fromhex("48895c2408574883ec30488bdac7")),
    ("player at game + 0x288",
     image.read(0x1724f40, 8) == b"\x48\x8d\x81" + const["GAME_PLAYER_OFFSET"].to_bytes(4, "little") + b"\xc3"),
    # CControllerManager::CControllerManager stores nine 0x1720 controllers from
    # +0x10 and the selected one at +0xd048; its creator saves it in the global.
    ("controller manager global",
     image.read(0x1884fda, 3) == b"\x48\x89\x05"
     and 0x1884fe1 + int.from_bytes(image.read(0x1884fdd, 4), "little", signed=True) == const["CONTROLLER_MANAGER_RVA"]),
    ("controller stride and count",
     image.read(0x1a0c5fc, 11) == b"\xba" + const["CONTROLLER_STRIDE"].to_bytes(4, "little") + b"\x41\xb8\x09\x00\x00\x00"),
    ("selected controller field",
     image.read(0x1a0c652, 7) == b"\x48\x89\x9f" + const["SELECTED_CONTROLLER_OFFSET"].to_bytes(4, "little")),
]
# Facts features/fh3_groove.c relies on: the result layouts it fills, the task
# interface the title server call pump polls and the globals it logs.
groove = open(os.path.join(os.path.dirname(SOURCE), "fh3_groove.c")).read()
gc = {k: int(v, 16) for k, v in re.findall(r"#define (\w+) (0x[0-9a-f]+)u", groove)}


def rip_target(at, length):
    return at + length + int.from_bytes(image.read(at + length - 4, 4), "little", signed=True)


def has(rva, hexbytes):
    return image.read(rva, len(hexbytes) // 2) == bytes.fromhex(hexbytes)


checks += [
    ("groove: CForza updates the XboxMusicPlayer global",
     rtti_name(0x45f7340) == ".?AVCForza@@" and qword(0x45f7340 + 5 * 8) - base == 0x1731ae0
     and rip_target(0x1731b18, 7) == gc["PLAYER_GLOBAL_RVA"] and rip_target(0x1731b2b, 7) == gc["PLAYER_GLOBAL_RVA"]),
    ("groove: accessible check reads the audio settings",
     rip_target(0x1946039, 7) == gc["AUDIO_GLOBAL_RVA"] and has(0x1946043, "ff9010030000")
     and has(0x1946049, "80783a00") and has(0x194605f, "80b88000000000")
     and has(0x1946068, "80bb0069000000") and has(0x1946071, "83bbf868000001") and has(0x194607a, "80bbe966000000")),
    ("groove: permissions copied from +0x80 (UserProfilePermissions)",
     rtti_name(0x4634ce0) == ".?AVXboxMusicAuthentication@XboxMusic@@" and qword(0x4634ce0) - base == 0x1946e10
     and has(0x1946e38, "0fb68b80000000884b78440fb6838100000044884379")),
    ("groove: playlist browser result layout",
     rtti_name(0x4634cc8) == ".?AVPlaylistBrowser@XboxMusic@@" and qword(0x4634cc8) - base == 0x1946b30
     and has(0x1946b59, "8b81bc000000") and has(0x1946b90, "4869d8") and image.read(0x1946b93, 4) == gc["PLAYLIST_STRIDE"].to_bytes(4, "little")
     and has(0x1946bb0, "488d942bc4000000") and has(0x1946bec, "488d942bba020000") and has(0x1946d25, "80bd6064000000")
     and gc["PLAYLIST_ID"] == 0xc4 - 0xc0 and gc["PLAYLIST_NAME"] == 0x2ba - 0xc0 and gc["PLAYLISTS_MORE"] == 0x6460 - 0xbc),
    ("groove: playlist tracks result layout",
     rtti_name(0x4634ca0) == ".?AVPlaylist@XboxMusic@@" and qword(0x4634ca0) - base == 0x1946650
     and has(0x19466f7, "807a0800") and has(0x1946709, "8b420c") and has(0x1946723, "80bac8950200")
     and has(0x1946787, "488d8106020000") and has(0x1946792, "488d810c060000") and has(0x194679d, "488d4110")
     and has(0x1946998, "4981c7781a0000") and has(0x1947118, "b9c0970200")
     and gc["TRACKS_VALID"] == 8 and gc["TRACKS_COUNT"] == 0xc and gc["TRACKS_FIRST"] == 0x10
     and gc["TRACK_TITLE"] == 0x206 - 0x10 and gc["TRACK_ARTIST"] == 0x60c - 0x10 and gc["TRACK_STRIDE"] == 0x1a78
     and gc["TRACKS_MORE"] == 0x295c8 and gc["TRACKS_TOKEN"] == 0x295ca),
    ("groove: song URL result at SongStream +0x1b0",
     rtti_name(0x4634c78) == ".?AVSongStream@XboxMusic@@" and qword(0x4634c78) - base == 0x1946d80
     and has(0x1946d93, "488d91b0010000")),
    ("groove: station URL filter accepts audio.m4a, which uses the MF reader",
     image.read(rip_target(0x19a7116, 7), 20) == "audio.m4a\0".encode("utf-16le")
     and image.read(rip_target(0x1983db2, 7), 20) == "audio.mp3\0".encode("utf-16le")
     and has(0x198c224, "e8c7ae0100") and has(0x196fd6a, "e831400100")
     and rip_target(0x196fea4, 7) == 0x1991d20 and has(0x1991dab, "e8d6090200")),
    ("groove: FMOD stream is PCM16 stereo 44.1 kHz, 4096-frame reads by 0x198a520",
     has(0x198ff56, "c744243c02000000") and has(0x198ff64, "c7442440" + (44100).to_bytes(4, "little").hex())
     and has(0x198ff6c, "c744244402000000") and has(0x198ff7c, "c744244800100000")
     and rip_target(0x198ff3b, 7) == 0x198a520 and gc["CHUNK_BYTES"] == 4096),
    ("groove: task status, close and pump",
     has(0x17870ca, "488b03488bcb4883c4205b48ff6010") and has(0x178703a, "488b03")
     and has(0x1787047, "ff5020") and has(0x1787057, "ff5008") and has(0x17bc7a8, "e8c3a8fcff894718")
     and has(0x17bc7b0, "8d48fe83f901760a")),
]

# Facts features/fh3_minimap.c relies on.
mm = open(os.path.join(os.path.dirname(SOURCE), "fh3_minimap.c")).read()
mc = {k: int(v, 16) for k, v in re.findall(r"#define (\w+) (0x[0-9a-f]+)u", mm)}
checks += [
    ("minimap: tracking controllers slot 27",
     qword(0x44eaf98 + 27 * 8) - base == 0x120b170 and qword(0x44eb0d8 + 27 * 8) - base == 0x120b170
     and rtti_name(0x44eb0d8) == ".?AVMapViewControllerTrackingMiniMap@@"),
    ("minimap: rotation test before heading",
     image.read(0x120b371, 5) == b"\xe8\x5a\x3c\x00\x00"),
    ("minimap: CForza +0x58 simulation manager",
     rtti_name(0x45f7340) == ".?AVCForza@@" and image.read(0x1724e60, 5) == b"\x48\x8b\x41"
     + bytes([mc["SIMULATION_MANAGER_OFFSET"]]) + b"\xc3"
     and image.read(0x172faf4, 8) == b"\x48\x8b\x78\x58\x48\x83\xc7\x08"),
    ("minimap: ForzaSimulation vtable", rtti_name(mc["FORZA_SIMULATION_VTABLE_RVA"]) == ".?AVForzaSimulation@@"),
    ("minimap: simulation views at +0x80",
     image.read(0x193dcfe, 7) == b"\x48\x8b\x86" + mc["VIEWS_OFFSET"].to_bytes(4, "little")),
    ("minimap: view vtables",
     rtti_name(mc["SPLIT_VIEW_VTABLE_RVA"]) == ".?AVSplitView@@"
     and rtti_name(mc["MULTICAM_VIEW_VTABLE_RVA"]) == ".?AVMultiCamView@@"),
    ("minimap: view camera getter",
     all(qword(v + 8 * 8) - base == 0x3e5400 for v in (mc["SPLIT_VIEW_VTABLE_RVA"], mc["MULTICAM_VIEW_VTABLE_RVA"]))
     and image.read(0x3e5400, 8) == b"\x48\x8b\x81" + mc["VIEW_CAMERA_OFFSET"].to_bytes(4, "little") + b"\xc3"),
    ("minimap: camera matrix getter",
     rtti_name(mc["MULTICAM_VTABLE_RVA"]) == ".?AVForzaMultiCam@Camera@@"
     and qword(mc["MULTICAM_VTABLE_RVA"] + 10 * 8) - base == 0x18eeb80
     and image.read(0x18eeb80, 8) == b"\xc5\xfc\x10\x81" + mc["CAMERA_MATRIX_OFFSET"].to_bytes(4, "little")),
]
for label, good in checks:
    ok &= good
    print("%-36s %-4s" % (label, "OK" if good else "FAIL"))
print("ALL OK" if ok else "FAILURES FOUND")
sys.exit(0 if ok else 1)
