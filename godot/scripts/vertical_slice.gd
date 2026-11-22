extends Node3D

const BIKES_PATH := "res://data/bikes.json"
const PhysicsModel := preload("res://scripts/bike_physics_model.gd")

const ROAD_LENGTH := 2000.0
const ROAD_HALF_WIDTH := 4.5
const ROAD_STEP := 4.0
const MODEL_TICK_FRAMES := 5
const SHURIKEN_TOP_SPEED := 12000.0
const PROTOTYPE_TOP_METERS_PER_SECOND := 42.0

@onready var bike: Node3D = %Bike
@onready var camera: Camera3D = %ChaseCamera
@onready var speed_label: Label = %SpeedLabel
@onready var state_label: Label = %StateLabel
@onready var assumption_label: Label = %AssumptionLabel

var bikes: Dictionary
var bike_key := "shuriken_400"
var tuning: Dictionary
var speed_units := 0
var gear := 0
var current_delta := 0
var model_frame_accumulator := 0
var distance := 12.0
var lateral_offset := 0.0
var traffic: Array[Dictionary] = []


func _ready() -> void:
    bikes = _load_json(BIKES_PATH)
    tuning = bikes[bike_key]
    _ensure_input_actions()
    _build_world()
    _spawn_traffic()
    _place_vehicle(bike, distance, lateral_offset)
    camera.global_position = bike.global_position + Vector3(0.0, 3.0, -7.0)
    assumption_label.text = (
        "VERIFIED: gear delta + speed integrator   |   "
        + "PROVISIONAL: 12 Hz scheduling, MPH scale, steering and road"
    )


func _physics_process(delta_seconds: float) -> void:
    var accelerating := Input.is_action_pressed("accelerate")
    var braking := Input.is_action_pressed("brake")
    var nitro := Input.is_action_pressed("nitro") and accelerating

    model_frame_accumulator += 1
    if model_frame_accumulator >= MODEL_TICK_FRAMES:
        var delta_result: Dictionary = PhysicsModel.calculate_clean_delta(
            tuning,
            speed_units,
            gear,
            accelerating,
            braking,
            nitro,
        )
        gear = delta_result.gear
        current_delta = delta_result.delta
        var integrated: Dictionary = PhysicsModel.integrate_speed(
            speed_units,
            current_delta,
            model_frame_accumulator,
        )
        speed_units = integrated.speed
        model_frame_accumulator = 0

    var top_speed: float = float(tuning.upper_threshold[tuning.max_gear])
    if nitro and tuning.nitro_parameter != 0:
        top_speed += top_speed / 8.0
    var meters_per_second := (
        float(speed_units) / maxf(top_speed, 1.0) * PROTOTYPE_TOP_METERS_PER_SECOND
    )

    var steering := Input.get_axis("steer_left", "steer_right")
    var steering_authority := clampf(meters_per_second / 8.0, 0.25, 1.0)
    lateral_offset += steering * 4.2 * steering_authority * delta_seconds
    lateral_offset = clampf(lateral_offset, -ROAD_HALF_WIDTH + 0.65, ROAD_HALF_WIDTH - 0.65)
    distance += meters_per_second * delta_seconds
    if distance > ROAD_LENGTH - 80.0:
        _reset_run()

    _place_vehicle(bike, distance, lateral_offset)
    bike.rotation.z = lerpf(bike.rotation.z, -steering * 0.22, 0.16)
    _update_camera(delta_seconds)
    _update_traffic(delta_seconds)
    _update_hud(meters_per_second, nitro)


func _unhandled_input(event: InputEvent) -> void:
    if event.is_action_pressed("reset_run"):
        _reset_run()
    elif event is InputEventKey and event.pressed and not event.echo:
        if event.physical_keycode == KEY_1:
            _select_bike("shuriken_400")
        elif event.physical_keycode == KEY_2:
            _select_bike("wild_thing_2000")


func _select_bike(key: String) -> void:
    bike_key = key
    tuning = bikes[bike_key]
    speed_units = 0
    gear = 0
    current_delta = 0


func _reset_run() -> void:
    speed_units = 0
    gear = 0
    current_delta = 0
    model_frame_accumulator = 0
    distance = 12.0
    lateral_offset = 0.0


func _update_camera(delta_seconds: float) -> void:
    var forward := _road_forward(distance)
    var desired_position := bike.global_position - forward * 7.5 + Vector3.UP * 3.2
    camera.global_position = camera.global_position.lerp(
        desired_position,
        1.0 - exp(-8.0 * delta_seconds),
    )
    camera.look_at(bike.global_position + forward * 7.0 + Vector3.UP * 0.6, Vector3.UP)


func _update_hud(meters_per_second: float, nitro: bool) -> void:
    var display_kph := meters_per_second * 3.6
    speed_label.text = "%03d km/h" % roundi(display_kph)
    state_label.text = "%s   GEAR %d   RAW %04X   DELTA %+d" % [
        tuning.display_name,
        gear + 1,
        speed_units,
        current_delta,
    ]
    if nitro and tuning.nitro_parameter != 0:
        state_label.text += "   NITRO"


func _build_world() -> void:
    _add_road_strip("Ground", 38.0, Color("6b7d3b"), -0.08)
    _add_road_strip("Road", ROAD_HALF_WIDTH, Color("3b4048"), 0.0)
    _add_lane_markers()
    _add_roadside_posts()


func _add_road_strip(name: String, half_width: float, color: Color, y_offset: float) -> void:
    var surface := SurfaceTool.new()
    surface.begin(Mesh.PRIMITIVE_TRIANGLES)
    var z := 0.0
    while z < ROAD_LENGTH:
        var next_z := minf(z + ROAD_STEP, ROAD_LENGTH)
        var left_a := _road_edge(z, -half_width) + Vector3.UP * y_offset
        var right_a := _road_edge(z, half_width) + Vector3.UP * y_offset
        var left_b := _road_edge(next_z, -half_width) + Vector3.UP * y_offset
        var right_b := _road_edge(next_z, half_width) + Vector3.UP * y_offset
        _add_triangle(surface, left_a, right_a, right_b)
        _add_triangle(surface, left_a, right_b, left_b)
        z = next_z

    var instance := MeshInstance3D.new()
    instance.name = name
    instance.mesh = surface.commit()
    instance.material_override = _material(color)
    %GeneratedWorld.add_child(instance)


func _add_triangle(surface: SurfaceTool, a: Vector3, b: Vector3, c: Vector3) -> void:
    surface.set_normal(Vector3.UP)
    surface.add_vertex(a)
    surface.set_normal(Vector3.UP)
    surface.add_vertex(b)
    surface.set_normal(Vector3.UP)
    surface.add_vertex(c)


func _add_lane_markers() -> void:
    var marker_mesh := BoxMesh.new()
    marker_mesh.size = Vector3(0.16, 0.025, 3.2)
    marker_mesh.material = _material(Color("e8dfb0"))
    var z := 8.0
    while z < ROAD_LENGTH:
        var marker := MeshInstance3D.new()
        marker.mesh = marker_mesh
        %GeneratedWorld.add_child(marker)
        _place_vehicle(marker, z, 0.0, 0.035)
        z += 11.0


func _add_roadside_posts() -> void:
    var post_mesh := BoxMesh.new()
    post_mesh.size = Vector3(0.14, 0.9, 0.14)
    post_mesh.material = _material(Color("d9d4bd"))
    var z := 18.0
    while z < ROAD_LENGTH:
        for side: float in [-1.0, 1.0]:
            var post := MeshInstance3D.new()
            post.mesh = post_mesh
            %GeneratedWorld.add_child(post)
            _place_vehicle(post, z, side * (ROAD_HALF_WIDTH + 1.1), 0.45)
        z += 24.0


func _spawn_traffic() -> void:
    var traffic_mesh := BoxMesh.new()
    traffic_mesh.size = Vector3(1.45, 1.15, 2.8)
    traffic_mesh.material = _material(Color("e18b43"))
    for index: int in range(8):
        var car := MeshInstance3D.new()
        car.name = "Traffic%02d" % index
        car.mesh = traffic_mesh
        %GeneratedWorld.add_child(car)
        traffic.append({
            "node": car,
            "distance": 75.0 + index * 145.0,
            "lane": -2.1 if index % 2 == 0 else 2.1,
            "speed": 9.0 + float(index % 3) * 2.5,
        })


func _update_traffic(delta_seconds: float) -> void:
    for vehicle: Dictionary in traffic:
        vehicle.distance += vehicle.speed * delta_seconds
        if vehicle.distance < distance - 35.0:
            vehicle.distance += 650.0
        if vehicle.distance > ROAD_LENGTH - 20.0:
            vehicle.distance -= ROAD_LENGTH - 80.0
        _place_vehicle(vehicle.node, vehicle.distance, vehicle.lane, 0.58)


func _place_vehicle(node: Node3D, at_distance: float, lateral: float, height := 0.5) -> void:
    var center := _road_center(at_distance)
    var forward := _road_forward(at_distance)
    var side := Vector3(forward.z, 0.0, -forward.x).normalized()
    node.global_position = center + side * lateral + Vector3.UP * height
    node.rotation.y = atan2(forward.x, forward.z)


func _road_center(z: float) -> Vector3:
    return Vector3(
        sin(z * 0.012) * 18.0 + sin(z * 0.031) * 4.0,
        sin(z * 0.009) * 2.4 + sin(z * 0.023) * 0.55,
        z,
    )


func _road_forward(z: float) -> Vector3:
    return (_road_center(z + 0.5) - _road_center(z)).normalized()


func _road_edge(z: float, lateral: float) -> Vector3:
    var center := _road_center(z)
    var forward := _road_forward(z)
    var side := Vector3(forward.z, 0.0, -forward.x).normalized()
    return center + side * lateral


func _material(color: Color) -> StandardMaterial3D:
    var material := StandardMaterial3D.new()
    material.albedo_color = color
    material.roughness = 0.9
    return material


func _ensure_input_actions() -> void:
    _add_key("accelerate", KEY_W)
    _add_key("accelerate", KEY_UP)
    _add_key("brake", KEY_S)
    _add_key("brake", KEY_DOWN)
    _add_key("steer_left", KEY_A)
    _add_key("steer_left", KEY_LEFT)
    _add_key("steer_right", KEY_D)
    _add_key("steer_right", KEY_RIGHT)
    _add_key("nitro", KEY_SHIFT)
    _add_key("reset_run", KEY_R)
    _add_joy_button("accelerate", JOY_BUTTON_A)
    _add_joy_button("brake", JOY_BUTTON_B)
    _add_joy_button("nitro", JOY_BUTTON_X)
    _add_joy_axis("steer_left", JOY_AXIS_LEFT_X, -1.0)
    _add_joy_axis("steer_right", JOY_AXIS_LEFT_X, 1.0)


func _add_key(action: StringName, keycode: Key) -> void:
    if not InputMap.has_action(action):
        InputMap.add_action(action)
    var event := InputEventKey.new()
    event.physical_keycode = keycode
    if not InputMap.action_has_event(action, event):
        InputMap.action_add_event(action, event)


func _add_joy_button(action: StringName, button: JoyButton) -> void:
    if not InputMap.has_action(action):
        InputMap.add_action(action)
    var event := InputEventJoypadButton.new()
    event.button_index = button
    if not InputMap.action_has_event(action, event):
        InputMap.action_add_event(action, event)


func _add_joy_axis(action: StringName, axis: JoyAxis, value: float) -> void:
    if not InputMap.has_action(action):
        InputMap.add_action(action)
    var event := InputEventJoypadMotion.new()
    event.axis = axis
    event.axis_value = value
    if not InputMap.action_has_event(action, event):
        InputMap.action_add_event(action, event)


func _load_json(path: String) -> Variant:
    var file := FileAccess.open(path, FileAccess.READ)
    if file == null:
        push_error("Unable to open " + path)
        return null
    return JSON.parse_string(file.get_as_text())
