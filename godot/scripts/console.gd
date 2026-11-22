# The live console: runs the natively translated game one frame per physics
# tick, shows its picture, and feeds it controller input.
#
# F5 resets. F12 saves the current picture under user://. R switches between
# the original picture and the crisp road: the picture drawn several times as
# large (a size that fits the window evenly, see crisp_scale_for), with the
# road's edges drawn at that size from the game's own road
# points (analysis/race-picture.md, sections 5 and 6). F switches those points
# between fine and whole pixels, to compare. With the crisp road on, riders,
# cars and roadside objects are drawn from their full pictures (section 7); O
# switches that off and on, to compare. 1 to 4 set the CPU
# speed to 100%, 200%, 400% or 800% of the original (the smoothness experiment:
# the video timing the game sees is unchanged, the 68000 just gets more done
# per frame). P captures the moment for a bug report: the picture, the game's
# memory, the settings and the session (everything done since start-up, by
# frame), from which `rr-session-replay` brings the port back to that exact
# frame.
extends Control

const PadInput = preload("res://scripts/pad_input.gd")
const DEFAULT_ROM := "Road Rash II (USA, Europe) (RR206).gen"
const WIDTH := 320
const HEIGHT := 224
const CPU_SPEEDS := [100, 200, 400, 800]  # keys 1 to 4
const MAX_CRISP_SCALE := 5  # larger pictures take too long to draw

## Name of the native class. Changing it is only useful in tests.
@export var console_class_name := "RoadRashConsole"
## ROM file. Empty means the RR2_ROM environment variable, else the default
## file name in the directory above the Godot project.
@export var rom_path := ""
## How long the CPU speed is shown after it is changed.
@export var speed_message_seconds := 2.0
## Where the settings are remembered between launches: the crisp road, the
## fine road points, the full-picture objects, the in-between mode, the soft
## far objects, the CRT screen, the mute and the game speed. Saved whenever
## one is changed, restored when a console starts. Empty: not remembered.
@export var settings_path := "user://console_settings.cfg"
## Where captures (P) are saved. Empty means generated/captures in the
## repository the project is run from, else user://captures.
@export var capture_dir := ""
## The wide view makes a window that is neither fullscreen nor maximised wider
## (and narrower again when it goes off). Set this to leave the window alone.
@export var keep_window_size := false

## The in-between picture modes, as the console numbers them, and the order
## the I key walks them in.
const BETWEEN_OFF := 0
const BETWEEN_PREDICT := 1
const BETWEEN_BLEND := 2
const BETWEEN_LATE := 3
const BETWEEN_ORDER: Array[int] = [BETWEEN_BLEND, BETWEEN_PREDICT, BETWEEN_LATE, BETWEEN_OFF]
const BETWEEN_NAMES := {
	BETWEEN_OFF: "off",
	BETWEEN_PREDICT: "predict (nothing late, may guess)",
	BETWEEN_BLEND: "blend (about one update late)",
	BETWEEN_LATE: "late blend (never guesses)",
}

## The CRT screen (V): the picture redrawn as a tube would show it
## (godot/shaders/crt.gdshader).
const CRT_SHADER := "res://shaders/crt.gdshader"
var _crt: ShaderMaterial = null
var _crt_on := false

var _console = null
var _halted := false
var _crisp := false
var _speed_note_until := -1  # ticks_msec when the speed note is cleared, or -1
var _last_capture := ""
var _width_before_wide := 0  # the window's width before the wide view widened it, or 0
var _restoring := false  # settings are being restored: nothing is saved or shown
var _saved: ConfigFile = null  # the settings file as last read or written
var _image: Image
var _texture: ImageTexture
var _screen: TextureRect
var _label: Label
## Sound (M mutes): what each frame made is pushed into a generator stream.
## The rate asked of the console is nudged, by half a percent at most, to
## keep about SOUND_TARGET_SECONDS waiting, so sound and picture stay in step.
const SOUND_TARGET_SECONDS := 0.04
const SOUND_NUDGE := 0.005
const SOUND_BUFFER_SECONDS := 0.2
var _player: AudioStreamPlayer = null
var _playback: AudioStreamGeneratorPlayback = null
var _sound_rate := 48000.0
var _sound_capacity := 0  # frames the generator holds when empty
var _sound_on := false
var _sound_muted := false
var _sound_asked := 0.0
var _help: ColorRect  # the key list (Tab), over the picture
var _help_label: Label


func _ready() -> void:
	PadInput.ensure_actions()
	_image = Image.create(WIDTH, HEIGHT, false, Image.FORMAT_RGBA8)
	_image.fill(Color.BLACK)
	_texture = ImageTexture.create_from_image(_image)
	_screen = TextureRect.new()
	_screen.texture = _texture
	_screen.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	# Fitted to the view whatever the texture's size: without this a larger
	# texture makes the control itself larger than the window.
	_screen.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_screen.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	_screen.set_anchors_preset(Control.PRESET_FULL_RECT)
	if ResourceLoader.exists(CRT_SHADER):
		_crt = ShaderMaterial.new()
		_crt.shader = load(CRT_SHADER)
	add_child(_screen)
	_label = Label.new()
	_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_label.set_anchors_preset(Control.PRESET_FULL_RECT)
	_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_label.add_theme_font_size_override("font_size", 8)
	_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_label.add_theme_constant_override("outline_size", 4)
	add_child(_label)
	_help = ColorRect.new()
	_help.color = Color(0.0, 0.0, 0.0, 0.72)
	_help.set_anchors_preset(Control.PRESET_FULL_RECT)
	_help.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_help.visible = false
	_help_label = Label.new()
	_help_label.set_anchors_preset(Control.PRESET_FULL_RECT)
	_help_label.offset_left = 12
	_help_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_help_label.add_theme_font_size_override("font_size", 8)
	_help_label.add_theme_constant_override("line_spacing", 1)
	_help.add_child(_help_label)
	add_child(_help)
	get_window().size_changed.connect(_on_window_resized)
	_start()


func _start() -> void:
	if not ClassDB.class_exists(console_class_name):
		_show("Native library not built.\nRun scripts/build-godot-extension.sh")
		return
	var console = ClassDB.instantiate(console_class_name)
	var error: String = console.load_rom(_resolve_rom_path())
	if error != "":
		_show(error + "\nSet RR2_ROM to the RR206 ROM file.")
		return
	set_console(console)


func _resolve_rom_path() -> String:
	if rom_path != "":
		return rom_path
	var from_env := OS.get_environment("RR2_ROM")
	if from_env != "":
		return from_env
	return ProjectSettings.globalize_path("res://").path_join("..").path_join(DEFAULT_ROM).simplify_path()


## Use this console object. Tests pass a stand-in with the same methods.
func set_console(console) -> void:
	_console = console
	_halted = false
	_crisp = false
	if _console != null and _console.has_method("set_scale"):
		_console.set_scale(1)
		_console.set_crisp_road(false)
	if _console != null and _console.has_method("set_between"):
		_console.set_between(BETWEEN_BLEND)
	_start_sound()
	_restore_settings()
	_fit()
	_show("")


## The settings as they are now, by section and name.
func _settings_now() -> Dictionary:
	var now := {"picture/crisp": _crisp, "picture/crt": _crt_on}
	if _sound_on:
		now["sound/muted"] = _sound_muted
	for item in [["picture/fine_road", "get_fine_road"], ["picture/fine_objects", "get_fine_objects"],
			["picture/soft_objects", "get_soft_objects"], ["picture/between", "get_between"],
			["picture/wide", "get_wide"],
			["game/cpu_speed", "get_cpu_speed"]]:
		if _console != null and _console.has_method(item[1]):
			now[item[0]] = _console.call(item[1])
	return now


func _save_settings() -> void:
	if _restoring or settings_path == "" or _console == null:
		return
	# What this console cannot do keeps the value saved before. The file is
	# not read again here: it was read when the console started.
	if _saved == null:
		_saved = ConfigFile.new()
	var now := _settings_now()
	for key in now:
		_saved.set_value(key.get_slice("/", 0), key.get_slice("/", 1), now[key])
	_saved.save(settings_path)


## A saved value, or null when the file has none (a file saved before the
## setting existed): ConfigFile reports an error for a missing key otherwise.
func _saved_value(file: ConfigFile, section: String, key: String) -> Variant:
	return file.get_value(section, key) if file.has_section_key(section, key) else null


## Put the saved settings back, quietly. A missing or damaged file, a value of
## the wrong kind or out of range, and anything this console cannot do are
## skipped: the setting then stays as the window starts it.
func _restore_settings() -> void:
	if settings_path == "" or _console == null:
		return
	var file := ConfigFile.new()
	if file.load(settings_path) != OK:
		_saved = null
		return
	_saved = file
	_restoring = true
	var fine_road = _saved_value(file, "picture", "fine_road")
	if fine_road is bool and _console.has_method("set_fine_road"):
		_console.set_fine_road(fine_road)
	var fine_objects = _saved_value(file, "picture", "fine_objects")
	if fine_objects is bool and _console.has_method("set_fine_objects"):
		_console.set_fine_objects(fine_objects)
	var soft = _saved_value(file, "picture", "soft_objects")
	if soft is bool and _console.has_method("set_soft_objects"):
		_console.set_soft_objects(soft)
	var between = _saved_value(file, "picture", "between")
	if between is int and BETWEEN_NAMES.has(between) and _console.has_method("set_between"):
		_console.set_between(between)
	var crisp = _saved_value(file, "picture", "crisp")
	if crisp is bool and crisp:
		set_crisp(true)
	# The wide view needs the crisp road, the fine road and the fine objects: after them.
	var wide_view = _saved_value(file, "picture", "wide")
	if wide_view is bool and wide_view:
		set_wide(true)
	var crt = _saved_value(file, "picture", "crt")
	if crt is bool and crt:
		set_crt(true)
	var speed = _saved_value(file, "game", "cpu_speed")
	if speed is int and CPU_SPEEDS.has(speed):
		set_cpu_speed(speed)
	var muted = _saved_value(file, "sound", "muted")
	if muted is bool and muted and _sound_on:
		set_sound_muted(true)
	_restoring = false
	_speed_note_until = -1


func message() -> String:
	return _label.text


## Run the 68000 at this percentage of its original speed and show the result
## briefly. Does nothing if the console cannot change speed or is halted.
func set_cpu_speed(percent: int) -> void:
	if _console == null or _halted or not _console.has_method("set_cpu_speed"):
		return
	_console.set_cpu_speed(percent)
	_show("CPU speed %d%%" % _console.get_cpu_speed())
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## How many times larger the crisp picture is drawn when the window shows the
## game `factor` times enlarged. It must divide the factor: then every row and
## column of the larger picture covers the same number of screen pixels, and
## a slope's steps are even. If nothing from 2 to MAX_CRISP_SCALE divides it,
## the nearest usable size is taken and the steps are slightly uneven.
## Whether the wide view is asked for (W). It shows only while the crisp
## road, the fine road points and the full-picture objects are all on.
func wide() -> bool:
	return _console != null and _console.has_method("get_wide") and _console.get_wide()


## How many of the game's pixels the picture is across: 320, or 398 while the
## wide view shows.
func picture_width() -> int:
	if _console == null or not _console.has_method("get_wide") or not _console.has_method("get_scale"):
		return WIDTH
	return _console.get_frame_width() / maxi(1, _console.get_scale())


## Whole-number size of the picture in the window for a picture `across` game pixels wide.
func _window_factor(across: int) -> int:
	var size := get_window().size
	return maxi(1, mini(size.x / across, size.y / HEIGHT))


## The window is laid out in the picture's own units, so the picture is always
## shown at a whole-number size. When the picture's width changes (the wide
## view coming into effect or going out of it) a window that is neither
## fullscreen nor maximised and is too narrow for the wider picture at the
## size it has is made wider, and put back to the width it had when the wide
## view goes; a window with room, and any other window, is left alone (a
## fullscreen or maximised one may then show the picture smaller). Then the crisp picture's scale is set again for the window.
func _fit() -> void:
	var across := picture_width()
	var window := get_window()
	if window.content_scale_size.x == across:
		return
	var was := window.content_scale_size.x if window.content_scale_size.x > 0 else WIDTH
	var factor := _window_factor(was)
	window.content_scale_size = Vector2i(across, HEIGHT)
	var filling := window.mode == Window.MODE_MAXIMIZED or window.mode == Window.MODE_FULLSCREEN \
			or window.mode == Window.MODE_EXCLUSIVE_FULLSCREEN
	if not filling and not keep_window_size:
		if across > was and window.size.x < across * factor:
			# Too narrow for the wider picture at this size: widen, and remember what it was.
			_width_before_wide = window.size.x
			window.size = Vector2i(across * factor, window.size.y)
		elif across < was and _width_before_wide > 0:
			window.size = Vector2i(_width_before_wide, window.size.y)
			_width_before_wide = 0
	if across < was:
		_width_before_wide = 0
	if _crisp and _console.has_method("set_scale"):
		_console.set_scale(crisp_scale())
	_tell_crt()


## Turn the wide view on or off (W) and say which, with the size the picture
## is shown at. It needs the crisp road, the fine road points and the
## full-picture objects; the note says so when one is off, and nothing is
## changed. Does nothing if the console cannot do it or is halted.
func set_wide(on: bool) -> void:
	if _console == null or _halted or not _console.has_method("set_wide"):
		return
	var ready: bool = _crisp and _console.get_fine_road() and _console.get_fine_objects()
	if on and not ready:
		_show("Wide view — needs R, F and O on")
		_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
		return
	_console.set_wide(on)
	_fit()
	_show("Wide view on (%d×)" % _console.get_scale() if on else "Wide view off")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


func crisp_scale_for(factor: int) -> int:
	if factor < 2:
		return 2  # a window no larger than the game: the smallest crisp picture
	for scale in range(MAX_CRISP_SCALE, 1, -1):
		if factor % scale == 0:
			return scale
	return clampi(factor, 2, 4)


## The crisp picture's scale for the window as it is now.
func crisp_scale() -> int:
	return crisp_scale_for(_window_factor(picture_width()))


## Switch between the original picture and the crisp road, and say which
## briefly. Does nothing if the console cannot scale or is halted.
func set_crisp(on: bool) -> void:
	if _console == null or _halted or not _console.has_method("set_scale"):
		return
	_crisp = on
	_console.set_scale(crisp_scale() if on else 1)
	_console.set_crisp_road(on)
	_fit()  # the wide view comes with the crisp road and goes with it
	_show("Crisp road on (%d×)" % crisp_scale() if on else "Original picture")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Switch the crisp road's points between fine (placed with fractions of a
## pixel) and whole pixels (as the game places them), and say which briefly.
## Does nothing if the console cannot do it or is halted.
func toggle_fine_road() -> void:
	if _console == null or _halted or not _console.has_method("set_fine_road"):
		return
	var on: bool = not _console.get_fine_road()
	_console.set_fine_road(on)
	_fit()
	_show("Fine road points" if on else "Road points on whole pixels")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Switch the objects (riders, cars, roadside things) between their full
## pictures drawn at the enlarged size and the original's sprites enlarged,
## and say which briefly. It shows while the crisp road is on. Does nothing if
## the console cannot do it or is halted.
func toggle_fine_objects() -> void:
	if _console == null or _halted or not _console.has_method("set_fine_objects"):
		return
	var on: bool = not _console.get_fine_objects()
	_console.set_fine_objects(on)
	_fit()
	_show("Objects from the full pictures" if on else "Objects enlarged as the original draws them")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Soften the far objects (those drawn smaller than their art) or make them
## sharp again, and say which briefly. It shows while the full-picture objects
## do. Does nothing if the console cannot do it or is halted.
func toggle_soft_objects() -> void:
	if _console == null or _halted or not _console.has_method("set_soft_objects"):
		return
	var on: bool = not _console.get_soft_objects()
	_console.set_soft_objects(on)
	_show("Far objects softened" if on else "Far objects sharp")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Step the in-between pictures to their next mode (blend, predict, late
## blend, off) and say which briefly. They show while the crisp road, the
## fine road points and the full-picture objects are all on; the note says so
## when one is not. Does nothing if the console cannot do it or is halted.
func cycle_between() -> void:
	if _console == null or _halted or not _console.has_method("set_between"):
		return
	var at := BETWEEN_ORDER.find(_console.get_between())
	var mode: int = BETWEEN_ORDER[(at + 1) % BETWEEN_ORDER.size()]
	_console.set_between(mode)
	var note: String = "In-between pictures: " + BETWEEN_NAMES[mode]
	var usable: bool = _crisp and _console.get_fine_objects() and _console.get_fine_road()
	if mode != BETWEEN_OFF and not usable:
		note += " — needs R, F and O on"
	_show(note)
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Show the picture on a CRT tube, or plainly, and say which briefly.
func set_crt(on: bool) -> void:
	if _crt == null:
		return
	_crt_on = on
	_screen.material = _crt if on else null
	_tell_crt()
	_show("CRT screen on" if on else "CRT screen off")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


func crt_on() -> bool:
	return _crt_on


## The picture size the tube was last told, for tests.
func crt_frame_size() -> Vector2i:
	if _crt == null:
		return Vector2i.ZERO
	return Vector2i(_crt.get_shader_parameter("frame_size"))


# The tube samples the picture itself (smoothly, which the screen's own
# nearest-pixel setting would not allow) and needs its size.
func _tell_crt() -> void:
	if _crt == null:
		return
	_crt.set_shader_parameter("frame", _texture)
	_crt.set_shader_parameter("frame_size", Vector2(_texture.get_width(), _texture.get_height()))
	_crt.set_shader_parameter("game_size", Vector2(picture_width(), HEIGHT))


func _on_window_resized() -> void:
	if _crisp and _console != null and _console.has_method("set_scale"):
		_console.set_scale(crisp_scale())


func frame_count() -> int:
	return _console.get_frame_count() if _console != null else 0


## The size, in the game's own 320x224 units, of the area the picture is shown in.
func screen_size() -> Vector2:
	return _screen.size


func picture() -> Image:
	return _image


func _show(text: String) -> void:
	_label.text = text


func _capture_root() -> String:
	if capture_dir != "":
		return capture_dir
	var generated := ProjectSettings.globalize_path("res://").path_join("../generated").simplify_path()
	if DirAccess.dir_exists_absolute(generated):
		return generated.path_join("captures")
	return ProjectSettings.globalize_path("user://captures")


func _on_off(on: bool) -> String:
	return "on" if on else "off"


func _capture_settings() -> String:
	var between := "not available"
	if _console.has_method("get_between"):
		between = BETWEEN_NAMES.get(_console.get_between(), "?")
	var lines := [
		"Time: " + Time.get_datetime_string_from_system(),
		"Frames since reset: %d" % frame_count(),
		"CPU speed: %s" % (str(_console.get_cpu_speed()) if _console.has_method("get_cpu_speed") else "not available"),
		"Crisp road: " + _on_off(_crisp),
		"Scale: %s" % (str(_console.get_scale()) if _console.has_method("get_scale") else "1"),
		"Fine road points: " + _state("get_fine_road"),
		"Objects from full pictures: " + _state("get_fine_objects"),
		"In-between pictures: " + between,
		"Far objects softened: " + _state("get_soft_objects"),
		"Wide view: " + _state("get_wide"),
		"CRT screen: " + _on_off(_crt_on),
		"Halted: " + (_console.get_halt_message() if _halted else "no"),
	]
	return "\n".join(lines) + "\n"


## Save this moment for a bug report (P): the picture as drawn (before the CRT
## screen), the game's work RAM, the settings, and the session, from which
## rr-session-replay brings the port back to this exact frame. Returns the
## folder, or "" if nothing was saved. Works on a halted console too.
func capture() -> String:
	if _console == null or not _console.has_method("get_session") or not _console.has_method("get_ram"):
		_note("Capture not available")
		return ""
	var session: String = _console.get_session()
	if session == "":
		_note("Capture failed: the console has no session to save")
		return ""
	var stamp := Time.get_datetime_string_from_system().replace("-", "").replace(":", "").replace("T", "-")
	var folder := _capture_root().path_join(stamp)
	var number := 2
	while DirAccess.dir_exists_absolute(folder):
		folder = _capture_root().path_join("%s-%d" % [stamp, number])
		number += 1
	var failed := DirAccess.make_dir_recursive_absolute(folder) != OK
	failed = failed or _image.save_png(folder.path_join("picture.png")) != OK
	for item in [["ram.bin", _console.get_ram()], ["session.txt", session.to_utf8_buffer()],
			["settings.txt", _capture_settings().to_utf8_buffer()]]:
		var file := FileAccess.open(folder.path_join(item[0]), FileAccess.WRITE)
		if file == null:
			failed = true
			continue
		file.store_buffer(item[1])
		file.close()
	if failed:
		_note("Capture failed: could not write to " + folder)
		return ""
	_last_capture = folder
	print("captured ", folder)
	_note("Captured: " + folder)
	return folder


func last_capture() -> String:
	return _last_capture


## Show a note for a moment; under the halt message if the console is halted.
func _note(text: String) -> void:
	if _halted and _console != null:
		_show(_console.get_halt_message() + "\nPress F5 to reset.\n" + text)
		return
	_show(text)
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)
	_save_settings()


## Turn the console's sound on, if it can make any, and start playing it.
func _start_sound() -> void:
	_sound_on = _console != null and _console.has_method("set_sound")
	if not _sound_on:
		return
	_console.set_sound(true)
	if _player == null:
		_sound_rate = AudioServer.get_mix_rate()
		var generator := AudioStreamGenerator.new()
		generator.mix_rate = _sound_rate
		generator.buffer_length = SOUND_BUFFER_SECONDS
		_player = AudioStreamPlayer.new()
		_player.stream = generator
		add_child(_player)
		_player.play()
		_playback = _player.get_stream_playback()
		_sound_capacity = _playback.get_frames_available() if _playback != null else 0
	_drop_waiting_sound()


## Forget sound not yet played (a reset, a halt): nothing old comes out later.
func _drop_waiting_sound() -> void:
	if _player == null or sound_buffered_seconds() <= 0.0:
		return
	# A playing generator cannot be emptied; starting it again gives a new, empty one.
	_player.stop()
	_player.play()
	_playback = _player.get_stream_playback()


## Push what the frame just run made. Asking for a little more or less than
## the card's rate holds the amount waiting near the target.
func _pump_sound() -> void:
	if not _sound_on or _playback == null:
		return
	_sound_asked = sound_rate_for(sound_buffered_seconds())
	var bytes: PackedByteArray = _console.take_sound(int(round(_sound_asked)))
	var samples := bytes.to_float32_array()
	var count: int = mini(samples.size() / 2, _playback.get_frames_available())
	if count <= 0:
		return
	var frames := PackedVector2Array()
	frames.resize(count)
	for i in count:
		frames[i] = Vector2(samples[2 * i], samples[2 * i + 1])
	_playback.push_buffer(frames)


## The rate to ask the console for when this much sound is waiting: more
## than the card's rate when too little waits, less when too much. The full
## nudge is reached when the amount waiting is half the target off.
func sound_rate_for(buffered_seconds: float) -> float:
	var off := (SOUND_TARGET_SECONDS - buffered_seconds) / SOUND_TARGET_SECONDS
	return _sound_rate * (1.0 + clampf(off * 2.0 * SOUND_NUDGE, -SOUND_NUDGE, SOUND_NUDGE))


func set_sound_muted(muted: bool) -> void:
	if _halted:
		return  # the halt message stays up
	if not _sound_on:
		_show("Sound not available")
	else:
		_sound_muted = muted
		_player.volume_db = -80.0 if muted else 0.0
		_show("Sound off" if muted else "Sound on")
	_speed_note_until = Time.get_ticks_msec() + int(speed_message_seconds * 1000.0)


func sound_on() -> bool:
	return _sound_on


func sound_muted() -> bool:
	return _sound_muted


## Seconds of sound waiting to be played.
func sound_buffered_seconds() -> float:
	if _playback == null or _sound_capacity <= 0:
		return 0.0
	return (_sound_capacity - _playback.get_frames_available()) / _sound_rate


## The rate last asked of the console, in samples a second.
func sound_asked_rate() -> float:
	return _sound_asked


## The key list (Tab): every key and what it is set to now.
func set_help(on: bool) -> void:
	_help.visible = on
	if on:
		_help_label.text = _help_lines()


func help_showing() -> bool:
	return _help.visible


func help_text() -> String:
	return _help_label.text


func _state(method: String) -> String:
	if _console == null or not _console.has_method(method):
		return "not available"
	return "on" if _console.call(method) else "off"


func _help_lines() -> String:
	var between := "not available"
	if _console != null and _console.has_method("get_between"):
		between = BETWEEN_NAMES.get(_console.get_between(), "?")
	return "\n".join([
		"R  Crisp road: " + ("on" if _crisp else "off"),
		"F  Fine road points: " + _state("get_fine_road"),
		"O  Objects from full pictures: " + _state("get_fine_objects"),
		"I  In-between pictures: " + between,
		"B  Far objects softened: " + _state("get_soft_objects"),
		"W  Wide view: " + _state("get_wide"),
		"V  CRT screen: " + ("on" if _crt_on else "off"),
		"M  Sound: " + (("off" if _sound_muted else "on") if _sound_on else "not available"),
		"     (F, O, I, B and W only show with the crisp road on)",
		"",
		"1 to 4  Game speed 100, 200, 400, 800%",
		"P  Capture this moment (picture, memory, session) for a bug report",
		"F5  Reset",
		"F12  Save a picture",
		"Tab  This list",
		"",
		"Player 1: arrows; Z X C = A B C; Enter = Start",
		"Player 2: I J K L; T Y U = A B C; Backspace = Start",
		"Gamepads: d-pad or stick; X A B = A B C; Start",
	])


func _physics_process(_delta: float) -> void:
	if _console == null or _halted:
		return
	if _speed_note_until >= 0 and Time.get_ticks_msec() >= _speed_note_until:
		_speed_note_until = -1
		_show("")
	_console.set_buttons(0, PadInput.buttons(0))
	_console.set_buttons(1, PadInput.buttons(1))
	if not _console.run_frame():
		_halted = true
		_drop_waiting_sound()
		_show(_console.get_halt_message() + "\nPress F5 to reset.")
		return
	_pump_sound()
	var frame: PackedByteArray = _console.get_frame()
	var width := WIDTH
	var height := HEIGHT
	if _console.has_method("get_frame_width"):
		width = _console.get_frame_width()
		height = _console.get_frame_height()
	if frame.size() != width * height * 4:
		return
	_image.set_data(width, height, false, Image.FORMAT_RGBA8, frame)
	if _texture.get_width() == width and _texture.get_height() == height:
		_texture.update(_image)
	else:
		# The picture changed size: a texture cannot, so it is replaced.
		_texture = ImageTexture.create_from_image(_image)
		_screen.texture = _texture
		_tell_crt()


func _notification(what: int) -> void:
	if what == NOTIFICATION_APPLICATION_FOCUS_OUT:
		PadInput.set_suspended(true)
	elif what == NOTIFICATION_APPLICATION_FOCUS_IN:
		PadInput.set_suspended(false)


func _unhandled_key_input(event: InputEvent) -> void:
	if not event.is_pressed() or event.is_echo():
		return
	if event.keycode == KEY_TAB or event.keycode == KEY_F1:
		set_help(not _help.visible)
		return
	if event.keycode == KEY_F5 and _console != null:
		_console.reset()
		_drop_waiting_sound()
		_halted = false
		_show("")
	elif event.keycode == KEY_R:
		set_crisp(not _crisp)
	elif event.keycode == KEY_F:
		toggle_fine_road()
	elif event.keycode == KEY_O:
		toggle_fine_objects()
	elif event.keycode == KEY_I:
		cycle_between()
	elif event.keycode == KEY_B:
		toggle_soft_objects()
	elif event.keycode == KEY_W:
		set_wide(not wide())
	elif event.keycode == KEY_V:
		set_crt(not _crt_on)
	elif event.keycode == KEY_M:
		set_sound_muted(not _sound_muted)
	elif event.keycode >= KEY_1 and event.keycode <= KEY_4:
		set_cpu_speed(CPU_SPEEDS[event.keycode - KEY_1])
	elif event.keycode == KEY_P:
		capture()
	elif event.keycode == KEY_F12:
		var path := "user://frame-%d.png" % frame_count()
		_image.save_png(path)
		print("saved ", ProjectSettings.globalize_path(path))
	if _help.visible:
		_help_label.text = _help_lines()
