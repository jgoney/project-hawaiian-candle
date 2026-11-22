# Headless test of controller input mapping.
# Run: Godot --headless --path godot --script res://tests/test_pad_input.gd
extends SceneTree

const PadInput = preload("res://scripts/pad_input.gd")

var failures := 0


func check(condition: bool, label: String) -> void:
	print(("PASS " if condition else "FAIL ") + label)
	if not condition:
		failures += 1


func release_all() -> void:
	for action in PadInput.ACTIONS:
		for player in 2:
			Input.action_release(PadInput.action_name(player, action))


func _init() -> void:
	PadInput.ensure_actions()
	check(InputMap.has_action("p1_start") and InputMap.has_action("p2_a"), "actions are registered")
	check(PadInput.buttons(0) == 0, "nothing held gives 0")

	Input.action_press("p1_a")
	Input.action_press("p1_start")
	check(PadInput.buttons(0) == PadInput.PAD_A | PadInput.PAD_START, "A and Start map to their bits")
	check(PadInput.buttons(1) == 0, "player 2 is unaffected")
	release_all()

	Input.action_press("p1_b")
	Input.action_press("p1_c")
	check(PadInput.buttons(0) == PadInput.PAD_B | PadInput.PAD_C, "B and C map to their bits")
	release_all()

	Input.action_press("p1_left")
	Input.action_press("p1_right")
	var held := PadInput.buttons(0)
	check(not (held & PadInput.PAD_LEFT and held & PadInput.PAD_RIGHT), "left and right are never both sent")
	release_all()

	Input.action_press("p1_up")
	Input.action_press("p1_down")
	held = PadInput.buttons(0)
	check(not (held & PadInput.PAD_UP and held & PadInput.PAD_DOWN), "up and down are never both sent")
	release_all()

	Input.action_press("p2_up")
	check(PadInput.buttons(1) == PadInput.PAD_UP and PadInput.buttons(0) == 0, "player 2 has its own actions")

	Input.action_press("p1_a")
	PadInput.set_suspended(true)
	check(PadInput.buttons(0) == 0 and PadInput.buttons(1) == 0, "suspended input sends nothing while keys are down")
	PadInput.set_suspended(false)
	check(PadInput.buttons(0) == PadInput.PAD_A, "input resumes")
	release_all()
	check(PadInput.buttons(0) == 0, "releasing everything gives 0")
	quit(1 if failures > 0 else 0)
