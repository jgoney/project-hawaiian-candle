"""The self-timed boot runner, on a tiny synthetic ROM."""

from __future__ import annotations

import subprocess

from harness import REPO, ROM_SIZE, build_snippet

from rr2port.picture import load_rgb

RUNNER = REPO / "port" / "tools" / "rr_boot_run.c"

SETUP = (
    "33FC816400C00004"  # move.w #$8164,$c00004   display on, VBlank interrupt on, mode 5
    "33FC870500C00004"  # move.w #$8705,$c00004   backdrop = colour 5
    "33FC8C8100C00004"  # move.w #$8c81,$c00004   40 cells
    "23FCC00A000000C00004"  # move.l #$c00a0000,$c00004   CRAM address of colour 5
    "33FC000E00C00000"  # move.w #$000e,$c00000   red
)
ENABLE = "46FC2000"  # move #$2000,sr
LOOP = "60FE"  # bra.s *
HANDLER = "523900FF00004E73"  # addq.b #1,$ff0000 ; rte


def image(enable_interrupts: bool = True) -> bytes:
    rom = bytearray(0x400)
    rom[0:4] = bytes.fromhex("00FFFF00")  # initial stack pointer
    rom[4:8] = bytes.fromhex("00000200")  # reset vector
    rom[0x78:0x7C] = bytes.fromhex("00000300")  # level 6 vector
    code = bytes.fromhex(SETUP + (ENABLE if enable_interrupts else "") + LOOP)
    rom[0x200 : 0x200 + len(code)] = code
    handler = bytes.fromhex(HANDLER)
    rom[0x300 : 0x300 + len(handler)] = handler
    return bytes(rom)


def run(tmp_path, rom_image: bytes, *args: str, rom=None):
    binary, rom_path = build_snippet(
        rom_image, tmp_path, entries=(0x200, 0x300), main=RUNNER, defines=["RR_ANY_ROM"]
    )
    return subprocess.run(
        [str(binary), "--rom", str(rom or rom_path), *args], capture_output=True, text=True
    )


def test_vblank_interrupts_arrive_once_a_frame_and_the_picture_is_the_backdrop(tmp_path):
    frame = tmp_path / "frame.ppm"
    ram = tmp_path / "ram.bin"
    done = run(tmp_path, image(), "--snap", f"4:{frame}", "--ram", f"4:{ram}")
    assert done.returncode == 0, done.stdout + done.stderr
    assert done.stdout.startswith("DONE vblanks=4 ")
    # Taken as the 4th handler entry begins, so the handler has run 3 times.
    assert ram.read_bytes()[0] == 3
    width, height, pixels = load_rgb(frame)
    assert (width, height) == (320, 224)
    assert pixels == bytes([255, 0, 0]) * (320 * 224)


def test_masked_interrupts_are_never_delivered(tmp_path):
    frame = tmp_path / "frame.ppm"
    done = run(tmp_path, image(False), "--snap", f"1:{frame}", "--max-frames", "5")
    assert done.returncode == 1
    assert "not reached" in done.stderr and not frame.exists()


def test_wrong_rom_is_refused(tmp_path):
    other = tmp_path / "other.rom"
    other.write_bytes(b"\x4e\x71" * (ROM_SIZE // 2))
    done = run(tmp_path, image(), "--snap", f"1:{tmp_path / 'f.ppm'}", rom=other)
    assert done.returncode == 2 and "CRC" in done.stderr


def test_a_request_is_required(tmp_path):
    done = run(tmp_path, image())
    assert done.returncode == 2 and "usage" in done.stderr


def line_interrupt_image() -> bytes:
    """Each line interrupt moves the backdrop to the next colour; the frame interrupt resets it.

    Colour 1 is red and colour 2 is green; the rest are black.
    """
    rom = bytearray(0x400)
    rom[0:4] = bytes.fromhex("00FFFF00")
    rom[4:8] = bytes.fromhex("00000200")
    rom[0x70:0x74] = bytes.fromhex("00000340")  # level 4 (line interrupt)
    rom[0x78:0x7C] = bytes.fromhex("00000300")  # level 6 (frame interrupt)
    code = bytes.fromhex(
        "33FC801400C00004"  # move.w #$8014,$c00004   line interrupts on
        "33FC816400C00004"  # move.w #$8164,$c00004   display and frame interrupt on
        "33FC8A0000C00004"  # move.w #$8a00,$c00004   line interrupt every line
        "33FC8C8100C00004"  # move.w #$8c81,$c00004   40 cells
        "33FC8F0200C00004"  # move.w #$8f02,$c00004   auto-increment 2
        "23FCC002000000C00004"  # move.l #$c0020000,$c00004   CRAM address of colour 1
        "33FC000E00C00000"  # move.w #$000e,$c00000   colour 1 = red
        "33FC00E000C00000"  # move.w #$00e0,$c00000   colour 2 = green
        "46FC2000"  # move #$2000,sr
        "60FE"  # bra.s *
    )
    rom[0x200 : 0x200 + len(code)] = code
    frame_handler = bytes.fromhex(
        "423900FF0000"  # clr.b $ff0000
        "33FC870000C00004"  # move.w #$8700,$c00004   backdrop = colour 0
        "4E73"
    )
    line_handler = bytes.fromhex(
        "523900FF0000"  # addq.b #1,$ff0000
        "303C8700"  # move.w #$8700,d0
        "803900FF0000"  # or.b $ff0000,d0
        "33C000C00004"  # move.w d0,$c00004   backdrop = colour <count>
        "4E73"
    )
    rom[0x300 : 0x300 + len(frame_handler)] = frame_handler
    rom[0x340 : 0x340 + len(line_handler)] = line_handler
    return bytes(rom)


def test_a_line_interrupt_changes_the_picture_from_the_next_line(tmp_path):
    frame = tmp_path / "frame.ppm"
    binary, rom_path = build_snippet(
        line_interrupt_image(),
        tmp_path,
        entries=(0x200, 0x300, 0x340),
        main=RUNNER,
        defines=["RR_ANY_ROM"],
    )
    done = subprocess.run(
        [str(binary), "--rom", str(rom_path), "--snap", f"4:{frame}"],
        capture_output=True,
        text=True,
    )
    assert done.returncode == 0, done.stdout + done.stderr
    _, _, pixels = load_rgb(frame)
    row = 320 * 3
    black, red, green = (bytes(colour) * 320 for colour in ([0, 0, 0], [255, 0, 0], [0, 255, 0]))
    # The counter also expires on the first blanking line, after the frame
    # interrupt, so line 0 is drawn with colour 1. The interrupt raised by
    # line 0 is serviced after line 0 has been drawn, so line 1 shows colour 2.
    assert pixels[0:row] == red
    assert pixels[row : 2 * row] == green
    assert pixels[2 * row : 3 * row] == black


def test_snap_frame_takes_the_picture_after_whole_frames(tmp_path):
    frame = tmp_path / "frame.ppm"
    done = run(tmp_path, image(), "--snap-frame", f"3:{frame}")
    assert done.returncode == 0, done.stdout + done.stderr
    assert " frames=3 " in done.stdout
    _, _, pixels = load_rgb(frame)
    assert pixels == bytes([255, 0, 0]) * (320 * 224)
