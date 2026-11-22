"""Reading the race scene from a work-RAM image and predicting what the game sends on."""

import struct

import pytest

from rr2port.scene import (
    NotARaceScene,
    RoadPoint,
    RoadTables,
    line_shifts,
    predict_line_tables,
    project_road,
    read_scene,
    road_edges,
)

# Tables shaped like the ROM's: every distance scaled by a half, and 65536 / n.
TABLES = RoadTables(
    perspective=(0,) + (0x8000,) * 0x3FFF,
    reciprocal=(0xFFFF, 0xFFFF) + tuple(65536 // n for n in range(2, 256)),
)


def race_ram(curve=0, hill=0, sideways=0, fraction=0, segments=4) -> bytearray:
    ram = bytearray(0x10000)
    struct.pack_into(">I", ram, 0x0300, 0xE038)  # the race's frame callback
    struct.pack_into(">H", ram, 0x1CA0, segments)
    struct.pack_into(">HH", ram, 0x0A0A, 7, fraction)  # on segment 7 of the track
    struct.pack_into(">h", ram, 0x0A02, sideways)
    struct.pack_into(">h", ram, 0x0A08, 100)  # camera height
    struct.pack_into(">HH", ram, 0xF2B0, 0, 152)  # the 3D view is lines 0..151
    struct.pack_into(">hh", ram, 0xF2BC, 60, 160)  # horizon reference line, screen centre
    for entry in range(256):
        struct.pack_into(">bb", ram, 0xD828 + entry * 2, curve, hill)
    struct.pack_into(">H", ram, 0x08D6, 0xFFFF)  # nothing to draw
    return ram


def put_points(ram, points):
    for i, p in enumerate(points):
        struct.pack_into(">h", ram, 0x1E20 + 2 * i, p.line)
        struct.pack_into(">h", ram, 0xD2B8 + 2 * i, p.clipped_line)
        struct.pack_into(">h", ram, 0x1DBC + 2 * i, p.centre)
        struct.pack_into(">h", ram, 0x1E84 + 2 * i, p.left)
        struct.pack_into(">h", ram, 0x1F4C + 2 * i, p.right)


def test_projection_of_a_straight_flat_road():
    points = project_road(bytes(race_ram()), TABLES)
    # The first point is at the camera: nothing is scaled. The rest are at half scale.
    assert points[0] == RoadPoint(line=160, clipped_line=152, centre=160, left=-224, right=224)
    assert points[1] == RoadPoint(line=110, clipped_line=110, centre=160, left=-112, right=112)
    assert points[2] == points[1] and len(points) == 4


def test_a_curve_byte_bends_the_road_more_with_every_segment():
    points = project_road(bytes(race_ram(curve=2)), TABLES)
    # Sideways position grows 0, 2, 6, 12: each segment adds a step that grows by the curve.
    assert [p.centre for p in points] == [160, 161, 163, 166]
    assert [p.right - p.left for p in points] == [448, 224, 224, 224]


def test_a_hill_byte_raises_the_road_ahead():
    points = project_road(bytes(race_ram(hill=4)), TABLES)
    # Height under the camera shrinks 100, 96, 92, 88: halved, plus the reference line.
    assert [p.line for p in points] == [160, 108, 106, 104]
    assert [p.clipped_line for p in points] == [152, 108, 106, 104]


def test_the_players_sideways_position_moves_the_whole_road_the_other_way():
    points = project_road(bytes(race_ram(sideways=40)), TABLES)
    assert [p.centre for p in points] == [120, 140, 140, 140]


def test_edges_run_straight_between_two_points():
    points = (
        RoadPoint(line=100, clipped_line=100, centre=160, left=-40, right=40),
        RoadPoint(line=96, clipped_line=96, centre=160, left=-20, right=24),
    )
    edges = road_edges(points, TABLES)
    assert sorted(edges) == [96, 97, 98, 99]
    assert [edges[line][0] for line in (96, 97, 98, 99)] == [-20, -25, -30, -35]
    assert [edges[line][1] for line in (96, 97, 98, 99)] == [24, 28, 32, 36]


def test_a_point_hidden_behind_a_hill_draws_no_lines():
    points = (
        RoadPoint(line=100, clipped_line=90, centre=160, left=-40, right=40),
        RoadPoint(line=96, clipped_line=90, centre=160, left=-20, right=24),
    )
    assert road_edges(points, TABLES) == {}


def test_read_scene_finds_objects_and_road():
    ram = race_ram()
    put_points(ram, project_road(bytes(ram), TABLES))
    struct.pack_into(">HHH", ram, 0x08D6, 1, 0, 0xFFFF)  # draw record 1, then record 0
    struct.pack_into(">hhHhHH", ram, 0xCEB8, 50, 120, 0x2000, 130, 0x21, 0x9005)
    struct.pack_into(">hh", ram, 0xCEB8 + 0x0C, -30, 0x0340)
    struct.pack_into(">hhHhHH", ram, 0xCEB8 + 22, 200, 100, 0x4000, 224, 0x30, 0x0002)
    struct.pack_into(">H", ram, 0xCEB8 + 22 + 0x12, 0x31)
    scene = read_scene(bytes(ram))
    assert scene.camera_segment == 7 and scene.camera_height == 100
    assert (scene.view_top, scene.view_bottom, scene.centre_x) == (0, 152, 160)
    assert len(scene.road) == 4 and scene.road[1].line == 110
    assert scene.horizon_line == 110
    assert scene.track[0] == (0, 0) and len(scene.track) == 4
    far, near = scene.objects
    assert (far.x, far.foot_line, far.size) == (200, 100, 1.0)
    assert (far.picture, far.second_picture) == (0x30, 0x31)
    assert (near.x, near.foot_line, near.size, near.hidden_below) == (50, 120, 0.5, 130)
    assert (near.mirrored, near.colour_set, near.segment) == (True, 1, 5)
    assert (near.offset, near.distance) == (-30, 0x0340)
    assert (far.mirrored, far.order, near.order) == (False, 0, 1)


def test_scene_reader_refuses_a_menu_image():
    ram = race_ram()
    struct.pack_into(">I", ram, 0x0300, 0x035A)  # some other screen's frame callback
    with pytest.raises(NotARaceScene, match="frame callback"):
        read_scene(bytes(ram))


def test_scene_reader_refuses_a_two_player_race():
    ram = race_ram()
    struct.pack_into(">H", ram, 0x0304, 1)
    with pytest.raises(NotARaceScene, match="two-player"):
        read_scene(bytes(ram))


def test_predicted_line_table_for_a_straight_flat_road():
    ram = race_ram()
    put_points(ram, project_road(bytes(ram), TABLES))
    table = predict_line_tables(read_scene(bytes(ram)), TABLES)
    # From the horizon (line 110) to the bottom of the view. Left edge -112 and right
    # edge 112 at the horizon, plus the centre 160: plane A 48 - 0x2A0, plane B 272 + 0x2A0.
    assert sorted(table) == list(range(110, 152))
    assert table[110] == ((48 - 0x2A0) & 0xFFFF, 272 + 0x2A0)
    edges = road_edges(read_scene(bytes(ram)).road, TABLES)
    assert table[151] == line_shifts(edges[151][0], edges[151][1], 160)


def test_shifts_stop_at_their_limits():
    # Plane A carries the left edge, plane B the right edge, each from a fixed base.
    assert line_shifts(-100, 100, 160) == ((60 - 0x2A0) & 0xFFFF, 260 + 0x2A0)
    assert line_shifts(-1000, 1000, 160) == ((-0x160 - 0x2A0) & 0xFFFF, 0x180 + 0x2A0)
    assert line_shifts(500, -500, 160) == ((0x140 - 0x2A0) & 0xFFFF, 0x2A0)
