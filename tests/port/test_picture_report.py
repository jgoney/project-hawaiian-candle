"""Reading the replay tool's picture files: which layer drew what, and the sprite list."""

import struct

import pytest

from rr2port.picture_report import (
    HEIGHT,
    WIDTH,
    PictureFormatError,
    layer_bands,
    read_line_state,
    read_sources,
    read_video_state,
    register_changes,
    sprites_on_screen,
)


def sources_file(tmp_path, layer_of_line):
    data = bytearray()
    for line in range(HEIGHT):
        for x in range(WIDTH):
            layer = layer_of_line(line, x)
            data += struct.pack(">BBH", layer, 5, 0x1234)
    path = tmp_path / "sources.bin"
    path.write_bytes(bytes(data))
    return path


def video_state(reg5=0x6C, sprites=()):
    reg = bytearray(24)
    reg[5] = reg5
    vram = bytearray(0x10000)
    table = (reg5 & 0x7E) << 9
    for index, (y, size, link, attr, x) in sprites:
        at = table + index * 8
        vram[at : at + 8] = struct.pack(">HBBHH", y, size, link, attr, x)
    return b"RR2VDPSTATE1\n" + bytes(reg) + bytes(vram) + bytes(128) + bytes(80)


def test_read_sources_gives_one_record_per_pixel(tmp_path):
    sources = read_sources(sources_file(tmp_path, lambda line, x: 4 if x == 3 else 1))
    assert len(sources) == HEIGHT and len(sources[0]) == WIDTH
    assert sources[7][3].layer == "sprite" and sources[7][2].layer == "plane B"
    assert sources[7][3].colour == 5 and sources[7][3].detail == 0x1234


def test_read_sources_rejects_a_file_of_the_wrong_size(tmp_path):
    path = tmp_path / "short.bin"
    path.write_bytes(bytes(100))
    with pytest.raises(PictureFormatError, match="short.bin"):
        read_sources(path)


def test_layer_bands_groups_lines(tmp_path):
    def layer(line, x):
        if line < 100:
            return 0
        if line < 180:
            return 2 if x < 160 else 4
        return 3

    bands = layer_bands(read_sources(sources_file(tmp_path, layer)))
    assert [(b.first_line, b.last_line) for b in bands] == [(0, 99), (100, 179), (180, 223)]
    assert bands[0].pixels == {"backdrop": 100 * WIDTH}
    assert bands[1].pixels == {"plane A": 80 * 160, "sprite": 80 * 160}
    assert bands[2].pixels == {"window": 44 * WIDTH}


def test_sprites_on_screen_follows_the_link_order(tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(
        video_state(
            sprites=[
                (0, (128 + 10, 0x05, 2, 0xA123, 128 + 20)),
                (1, (128, 0x00, 0, 0x0001, 128)),  # not linked from sprite 0
                (2, (128 + 50, 0x0F, 0, 0x1802, 128 + 60)),
            ]
        )
    )
    sprites = sprites_on_screen(read_video_state(path))
    assert [s.index for s in sprites] == [0, 2]
    first = sprites[0]
    assert (first.x, first.y, first.width, first.height) == (20, 10, 16, 16)
    assert (first.tile, first.palette, first.priority) == (0x123, 1, True)
    assert (sprites[1].width, sprites[1].height) == (32, 32)
    assert (sprites[1].flip_h, sprites[1].flip_v) == (True, True)


def test_sprite_table_base_comes_from_register_5(tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(video_state(reg5=0x5C, sprites=[(0, (128 + 1, 0, 0, 7, 128 + 2))]))
    state = read_video_state(path)
    assert state.sprite_table == 0xB800
    assert [(s.x, s.y, s.tile) for s in sprites_on_screen(state)] == [(2, 1, 7)]


def test_read_video_state_rejects_another_file(tmp_path):
    path = tmp_path / "state.bin"
    path.write_bytes(b"something else")
    with pytest.raises(PictureFormatError, match="RR2VDPSTATE1"):
        read_video_state(path)


def test_register_changes_lists_what_differs_between_lines(tmp_path):
    lines = []
    for line in range(HEIGHT):
        reg = [0] * 24
        reg[7] = 1 if line >= 100 else 0
        scroll = "0010 0020 0000 0000" if line < 50 else "0011 0020 0000 0000"
        lines.append(f"{line} " + " ".join(f"{r:02X}" for r in reg) + f" {scroll}\n")
    path = tmp_path / "lines.txt"
    path.write_text("".join(lines))
    states = read_line_state(path)
    assert states[49].hscroll == (0x10, 0x20) and states[50].hscroll == (0x11, 0x20)
    assert register_changes(states) == [(100, 7, 0, 1)]
