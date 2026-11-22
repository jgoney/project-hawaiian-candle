extends Control

const BIKES_PATH := "res://data/bikes.json"
const CASES_PATH := "res://data/verified_physics_cases.json"
const INTEGRATOR_CASES_PATH := "res://data/verified_integrator_cases.json"
const PhysicsModel := preload("res://scripts/bike_physics_model.gd")

@onready var status_label: Label = %StatusLabel
@onready var detail_label: Label = %DetailLabel


func _ready() -> void:
    var bikes: Dictionary = _load_json(BIKES_PATH)
    var cases: Array = _load_json(CASES_PATH)
    var integrator_cases: Array = _load_json(INTEGRATOR_CASES_PATH)
    var failures: Array[String] = []

    for case: Dictionary in cases:
        var result: Dictionary = PhysicsModel.calculate_clean_delta(
            bikes[case.bike],
            case.speed,
            case.gear,
            case.accelerating,
            case.braking,
            case.nitro,
        )
        if result.delta != case.delta or result.gear != case.next_gear:
            failures.append(
                "%s: got delta %d/gear %d, expected %d/%d" % [
                    case.name,
                    result.delta,
                    result.gear,
                    case.delta,
                    case.next_gear,
                ]
            )

    for case: Dictionary in integrator_cases:
        var result: Dictionary = PhysicsModel.integrate_speed(
            case.speed,
            case.delta,
            case.timestep,
            case.reduced_delta,
        )
        if result.speed != case.next_speed or result.applied_delta != case.applied_delta:
            failures.append(
                "%s: got speed %d/delta %d, expected %d/%d" % [
                    case.name,
                    result.speed,
                    result.applied_delta,
                    case.next_speed,
                    case.applied_delta,
                ]
            )

    if failures.is_empty():
        var case_count := cases.size() + integrator_cases.size()
        status_label.text = "VERIFIED PHYSICS CASES: %d/%d" % [case_count, case_count]
        status_label.modulate = Color("73f59b")
        detail_label.text = (
            "Clean delta paths match captured RR206 behavior.\n\n"
            + "Implemented:\n"
            + "  gear base acceleration\n"
            + "  coasting drag (-5)\n"
            + "  braking (-64 plus drag)\n"
            + "  top-speed limiting\n"
            + "  nitro delta doubling and +1/8 threshold\n\n"
            + "  speed += delta x timestep, clamped at zero\n\n"
            + "Pending timestep scheduling, slope meaning, collision, and MPH scale."
        )
        print("RR2GODOT: all %d verified physics cases passed" % case_count)
    else:
        status_label.text = "PHYSICS CONFORMANCE FAILURE"
        status_label.modulate = Color("ff6b6b")
        detail_label.text = "\n".join(failures)
        for failure in failures:
            push_error("RR2GODOT: " + failure)


func _load_json(path: String) -> Variant:
    var file := FileAccess.open(path, FileAccess.READ)
    if file == null:
        push_error("Unable to open " + path)
        return null
    return JSON.parse_string(file.get_as_text())
