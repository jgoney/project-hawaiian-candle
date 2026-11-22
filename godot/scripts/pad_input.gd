# Controller input for the console: keyboard and gamepad to Genesis pad bits.
#
# Player 1: arrow keys or d-pad / left stick; Z, X, C for A, B, C; Enter for Start.
# Player 2: gamepad 2 only, plus I J K L and T Y U, Backspace on the keyboard.
extends RefCounted

const PAD_UP := 1
const PAD_DOWN := 2
const PAD_LEFT := 4
const PAD_RIGHT := 8
const PAD_B := 16
const PAD_C := 32
const PAD_A := 64
const PAD_START := 128

const ACTIONS := ["up", "down", "left", "right", "a", "b", "c", "start"]
const BITS := {
	"up": PAD_UP, "down": PAD_DOWN, "left": PAD_LEFT, "right": PAD_RIGHT,
	"a": PAD_A, "b": PAD_B, "c": PAD_C, "start": PAD_START,
}
const KEYS := [
	{
		"up": KEY_UP, "down": KEY_DOWN, "left": KEY_LEFT, "right": KEY_RIGHT,
		"a": KEY_Z, "b": KEY_X, "c": KEY_C, "start": KEY_ENTER,
	},
	{
		"up": KEY_I, "down": KEY_K, "left": KEY_J, "right": KEY_L,
		"a": KEY_T, "b": KEY_Y, "c": KEY_U, "start": KEY_BACKSPACE,
	},
]
const JOY_BUTTONS := {
	"up": JOY_BUTTON_DPAD_UP, "down": JOY_BUTTON_DPAD_DOWN,
	"left": JOY_BUTTON_DPAD_LEFT, "right": JOY_BUTTON_DPAD_RIGHT,
	"a": JOY_BUTTON_X, "b": JOY_BUTTON_A, "c": JOY_BUTTON_B, "start": JOY_BUTTON_START,
}
# Left stick: [axis, direction]
const JOY_AXES := {
	"up": [JOY_AXIS_LEFT_Y, -1.0], "down": [JOY_AXIS_LEFT_Y, 1.0],
	"left": [JOY_AXIS_LEFT_X, -1.0], "right": [JOY_AXIS_LEFT_X, 1.0],
}

# While suspended (the window has lost focus) nothing is reported, so a key
# that was down when focus went away cannot stay stuck.
static var _suspended := false


static func action_name(player: int, action: String) -> String:
	return "p%d_%s" % [player + 1, action]


static func ensure_actions() -> void:
	for player in 2:
		for action in ACTIONS:
			var name := action_name(player, action)
			if InputMap.has_action(name):
				continue
			InputMap.add_action(name, 0.5)
			var key := InputEventKey.new()
			key.physical_keycode = KEYS[player][action]
			InputMap.action_add_event(name, key)
			var button := InputEventJoypadButton.new()
			button.device = player
			button.button_index = JOY_BUTTONS[action]
			InputMap.action_add_event(name, button)
			if JOY_AXES.has(action):
				var motion := InputEventJoypadMotion.new()
				motion.device = player
				motion.axis = JOY_AXES[action][0]
				motion.axis_value = JOY_AXES[action][1]
				InputMap.action_add_event(name, motion)


static func set_suspended(suspended: bool) -> void:
	_suspended = suspended


# The pad bits held by a player (0 or 1). Opposite directions cancel.
static func buttons(player: int) -> int:
	if _suspended:
		return 0
	var held := 0
	for action in ACTIONS:
		if Input.is_action_pressed(action_name(player, action)):
			held |= BITS[action]
	if held & PAD_LEFT and held & PAD_RIGHT:
		held &= ~(PAD_LEFT | PAD_RIGHT)
	if held & PAD_UP and held & PAD_DOWN:
		held &= ~(PAD_UP | PAD_DOWN)
	return held
