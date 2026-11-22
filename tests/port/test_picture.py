from __future__ import annotations

import pytest

from rr2port.picture import compare_pictures, load_rgb, main

PIL = pytest.importorskip("PIL.Image")

PIXELS = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 16, 32, 48])  # 2x2


def write_ppm(path, pixels=PIXELS, size=(2, 2)):
    path.write_bytes(f"P6\n{size[0]} {size[1]}\n255\n".encode() + pixels)
    return path


def write_png(path, pixels=PIXELS, size=(2, 2)):
    PIL.frombytes("RGB", size, pixels).save(path)
    return path


def test_load_rgb_reads_a_binary_ppm(tmp_path):
    assert load_rgb(write_ppm(tmp_path / "a.ppm")) == (2, 2, PIXELS)


def test_load_rgb_reads_a_ppm_with_a_comment_and_odd_spacing(tmp_path):
    path = tmp_path / "a.ppm"
    path.write_bytes(b"P6 # made by hand\n2\t2\n255\n" + PIXELS)
    assert load_rgb(path) == (2, 2, PIXELS)


def test_load_rgb_reads_a_png(tmp_path):
    assert load_rgb(write_png(tmp_path / "a.png")) == (2, 2, PIXELS)


def test_load_rgb_rejects_a_truncated_ppm(tmp_path):
    path = tmp_path / "a.ppm"
    path.write_bytes(b"P6\n2 2\n255\n" + PIXELS[:5])
    with pytest.raises(ValueError):
        load_rgb(path)


def test_identical_pictures_in_different_formats_compare_equal(tmp_path):
    diff = compare_pictures(write_png(tmp_path / "a.png"), write_ppm(tmp_path / "b.ppm"))
    assert diff.same and diff.differing_pixels == 0 and diff.first is None


def test_one_differing_pixel_is_located(tmp_path):
    changed = bytearray(PIXELS)
    changed[9] = 17  # pixel (1, 1)
    diff = compare_pictures(
        write_ppm(tmp_path / "a.ppm"), write_ppm(tmp_path / "b.ppm", bytes(changed))
    )
    assert (diff.same, diff.differing_pixels, diff.first) == (False, 1, (1, 1))


def test_different_sizes_are_not_the_same(tmp_path):
    diff = compare_pictures(
        write_ppm(tmp_path / "a.ppm"), write_ppm(tmp_path / "b.ppm", PIXELS[:6], (2, 1))
    )
    assert not diff.same and diff.size_want == (2, 2) and diff.size_got == (2, 1)


def test_missing_reference_raises(tmp_path):
    with pytest.raises(FileNotFoundError) as e:
        compare_pictures(tmp_path / "frame-end.png", write_ppm(tmp_path / "b.ppm"))
    assert "frame-end.png" in str(e.value)


def test_cli_reports_a_missing_reference_picture(tmp_path, capsys):
    got = write_ppm(tmp_path / "b.ppm")
    assert main([str(tmp_path / "frame-end.png"), str(got)]) == 1
    assert "no reference picture" in capsys.readouterr().err


def test_cli_reports_sizes_when_they_differ(tmp_path, capsys):
    want = write_png(tmp_path / "a.png")
    got = write_ppm(tmp_path / "b.ppm", PIXELS[:6], (2, 1))
    assert main([str(want), str(got)]) == 1
    assert "2x2" in capsys.readouterr().out


def test_cli_passes_identical_pictures(tmp_path, capsys):
    assert main([str(write_png(tmp_path / "a.png")), str(write_ppm(tmp_path / "b.ppm"))]) == 0
    assert "identical" in capsys.readouterr().out
