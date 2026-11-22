class_name BikePhysicsModel
extends RefCounted

const COAST_DRAG := -5
const BRAKE_ADJUSTMENT := -64


static func calculate_clean_delta(
        tuning: Dictionary,
        speed: int,
        gear: int,
        accelerating: bool,
        braking: bool,
        nitro_active: bool
) -> Dictionary:
    var next_gear := gear
    var delta := 0

    if accelerating:
        var threshold: int = tuning.upper_threshold[next_gear]
        if nitro_active and tuning.nitro_parameter != 0:
            threshold += threshold >> 3
        if speed >= threshold:
            if next_gear < tuning.max_gear:
                next_gear += 1
            else:
                delta = COAST_DRAG
        if delta != COAST_DRAG:
            delta = tuning.base_delta[next_gear]
            if nitro_active and tuning.nitro_parameter != 0:
                delta *= 2

    if braking:
        delta += BRAKE_ADJUSTMENT
    if not accelerating:
        delta += COAST_DRAG

    return {"delta": delta, "gear": next_gear}


static func integrate_speed(
        speed: int,
        delta: int,
        timestep: int,
        reduced_delta: bool = false
) -> Dictionary:
    var applied_delta := delta
    if reduced_delta:
        applied_delta -= applied_delta >> 2

    var next_speed := speed + applied_delta * timestep
    if next_speed < 0:
        next_speed = 0

    return {
        "speed": next_speed,
        "applied_delta": applied_delta,
        "scaled_motion": next_speed * timestep,
    }
