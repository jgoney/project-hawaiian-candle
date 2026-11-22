# Headless test of the live console scene.
# Run: Godot --headless --path godot --script res://tests/test_console_scene.gd
# Environment: RR2_ROM (optional) path of the RR206 ROM.
extends SceneTree

const SCENE := preload("res://scenes/console.tscn")
const PadInput = preload("res://scripts/pad_input.gd")

var failures := 0


func check(condition: bool, label: String) -> void:
	print(("PASS " if condition else "FAIL ") + label)
	if not condition:
		failures += 1


class HaltedStub:
	extends RefCounted
	var calls := 0
	var buttons_seen := []

	func run_frame() -> bool:
		calls += 1
		return false

	func get_halt_message() -> String:
		return "RR2FATAL pc=001234 insn=5: jump to untranslated address 00ABCD"

	func get_frame() -> PackedByteArray:
		return PackedByteArray()

	func set_buttons(port: int, held: int) -> void:
		buttons_seen.append([port, held])

	func reset() -> void:
		pass

	func get_frame_count() -> int:
		return 0


class RunningStub:
	extends HaltedStub

	func run_frame() -> bool:
		calls += 1
		return true


class SoundStub:
	extends RunningStub
	var sound := false
	var rates := []

	func set_sound(on: bool) -> void:
		sound = on

	func get_sound() -> bool:
		return sound

	func take_sound(rate: int) -> PackedByteArray:
		rates.append(rate)
		var bytes := PackedByteArray()
		bytes.resize(800 * 8)  # 800 silent left/right pairs of 32-bit floats
		return bytes


class DyingSoundStub:
	extends SoundStub
	var dead := false

	func run_frame() -> bool:
		calls += 1
		return not dead


class CaptureStub:
	extends RunningStub
	var session := "rr2-session 1\n0 speed 200\nend 7\n"

	func get_session() -> String:
		return session

	func get_ram() -> PackedByteArray:
		var bytes := PackedByteArray()
		bytes.resize(65536)
		bytes[0x1234] = 0x5A
		return bytes

	func get_cpu_speed() -> int:
		return 200


class SpeedStub:
	extends RunningStub
	var speed := 100

	func set_cpu_speed(percent: int) -> void:
		speed = percent

	func get_cpu_speed() -> int:
		return speed


class ScaleStub:
	extends RunningStub
	var scale := 1
	var crisp := false

	func set_scale(value: int) -> void:
		scale = value

	func get_scale() -> int:
		return scale

	func set_crisp_road(on: bool) -> void:
		crisp = on

	var fine := true

	func set_fine_road(on: bool) -> void:
		fine = on

	func get_fine_road() -> bool:
		return fine

	var soft := false

	func set_soft_objects(on: bool) -> void:
		soft = on

	func get_soft_objects() -> bool:
		return soft

	var between := 0

	func set_between(mode: int) -> void:
		between = mode

	func get_between() -> int:
		return between

	var fine_objects := true

	func set_fine_objects(on: bool) -> void:
		fine_objects = on

	func get_fine_objects() -> bool:
		return fine_objects

	var wide := false

	func set_wide(on: bool) -> void:
		wide = on

	func get_wide() -> bool:
		return wide

	# As the console: the wide view is in effect with the crisp road, the fine road,
	# the fine objects and an enlarged picture.
	func get_frame_width() -> int:
		return (398 if wide and crisp and fine and fine_objects and scale > 1 else 320) * scale

	func get_frame_height() -> int:
		return 224 * scale

	func get_frame() -> PackedByteArray:
		var frame := PackedByteArray()
		frame.resize(get_frame_width() * get_frame_height() * 4)
		frame.fill(scale)
		return frame


class EverythingStub:
	extends ScaleStub
	var speed := 100
	var sound := false

	func set_cpu_speed(percent: int) -> void:
		speed = percent

	func get_cpu_speed() -> int:
		return speed

	func set_sound(on: bool) -> void:
		sound = on

	func get_sound() -> bool:
		return sound

	func take_sound(_rate: int) -> PackedByteArray:
		return PackedByteArray()


func press(scene: Node, keycode: Key) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.pressed = true
	scene._unhandled_key_input(event)


func ticks(count: int) -> void:
	for i in count:
		await physics_frame


func make_scene(properties: Dictionary) -> Node:
	var scene := SCENE.instantiate()
	scene.settings_path = ""  # a test never reads or writes the owner's saved settings
	for key in properties:
		scene.set(key, properties[key])
	root.add_child(scene)
	return scene


func _initialize() -> void:
	run()


func run() -> void:
	var scene := make_scene({"rom_path": "/nonexistent.gen"})
	await ticks(2)
	check(scene.message().contains("/nonexistent.gen"), "a missing ROM is reported: " + scene.message())
	scene.queue_free()

	scene = make_scene({"console_class_name": "NoSuchConsoleClass"})
	await ticks(2)
	check(scene.message().contains("build-godot-extension.sh"), "a missing library shows the build hint")
	scene.queue_free()

	scene = make_scene({"rom_path": "/nonexistent.gen"})
	await ticks(1)
	var stub := HaltedStub.new()
	scene.set_console(stub)
	await ticks(3)
	check(scene.message().contains("00ABCD"), "a halt shows the error message")
	check(scene.message().contains("F5"), "a halt says how to reset")
	check(stub.calls == 1, "run_frame is not called again after a halt (%d)" % stub.calls)
	scene.queue_free()

	scene = make_scene({"rom_path": "/nonexistent.gen"})
	await ticks(1)
	stub = HaltedStub.new()
	Input.action_press("p1_start")
	scene.notification(NOTIFICATION_APPLICATION_FOCUS_OUT)
	check(PadInput.buttons(0) == 0, "losing focus releases the pads")
	scene.notification(NOTIFICATION_APPLICATION_FOCUS_IN)
	check(PadInput.buttons(0) == PadInput.PAD_START, "regaining focus restores input")
	Input.action_release("p1_start")
	scene.queue_free()

	# Keys 1 to 4 set the CPU speed; the new speed is shown for a moment.
	scene = make_scene({"rom_path": "/nonexistent.gen", "speed_message_seconds": 0.05})
	await ticks(1)
	var speedy := SpeedStub.new()
	scene.set_console(speedy)
	press(scene, KEY_3)
	check(speedy.speed == 400, "3 selects 400%% (%d)" % speedy.speed)
	check(scene.message().contains("400%"), "the new speed is shown: " + scene.message())
	press(scene, KEY_4)
	check(speedy.speed == 800, "4 selects 800%")
	press(scene, KEY_2)
	check(speedy.speed == 200, "2 selects 200%")
	press(scene, KEY_1)
	check(speedy.speed == 100 and scene.message().contains("100%"), "1 selects the original speed")
	OS.delay_msec(80)
	await ticks(2)
	check(scene.message() == "", "the speed note goes away: " + scene.message())
	scene.set_console(RunningStub.new())  # a console without the speed methods is left alone
	press(scene, KEY_4)
	check(scene.message() == "", "no speed note without speed support")
	scene.queue_free()

	# R switches between the original picture and the crisp road at 4 times the size.
	scene = make_scene({"rom_path": "/nonexistent.gen", "speed_message_seconds": 0.05})
	await ticks(1)
	var scaled := ScaleStub.new()
	scene.set_console(scaled)
	await ticks(1)
	check(scene.picture().get_size() == Vector2i(320, 224), "starts as the original picture")
	# The scale is one that divides the window's own enlargement, so every row and column
	# of the larger picture gets the same number of screen pixels.
	var expected := {0: 2, 1: 2, 2: 2, 3: 3, 4: 4, 5: 5, 6: 3, 7: 4, 8: 4, 9: 3, 10: 5, 12: 4}
	for factor in expected:
		check(scene.crisp_scale_for(factor) == expected[factor], "window %d times: scale %d (%d)" % [factor, expected[factor], scene.crisp_scale_for(factor)])
	var want: int = scene.crisp_scale()
	check(want == scene.crisp_scale_for(maxi(1, mini(scene.get_window().size.x / 320, scene.get_window().size.y / 224))), "the scale follows the window (%d)" % want)
	press(scene, KEY_R)
	check(scaled.scale == want and scaled.crisp, "R turns the crisp road on at the window's scale")
	check(scene.message().contains("Crisp road on (%d×)" % want), "and says so: " + scene.message())
	await ticks(1)
	check(scene.picture().get_size() == Vector2i(320 * want, 224 * want), "the picture is resized: %s" % scene.picture().get_size())
	check(scene.picture().get_pixel(320 * want - 1, 224 * want - 1).r8 == want, "and holds the new frame")
	await ticks(1)
	check(scene.screen_size() == Vector2(320, 224), "the larger picture is still fitted to the view, not grown past it: %s" % scene.screen_size())
	# F switches the road's points between fine and whole pixels, to compare the two.
	press(scene, KEY_F)
	check(not scaled.fine and scene.message().contains("whole pixels"), "F turns fine road points off: " + scene.message())
	press(scene, KEY_F)
	check(scaled.fine and scene.message().contains("Fine road points"), "F again turns them on: " + scene.message())
	# O switches the objects between their full pictures and the original's sprites.
	press(scene, KEY_O)
	check(not scaled.fine_objects and scene.message().contains("as the original draws them"), "O turns fine objects off: " + scene.message())
	press(scene, KEY_O)
	check(scaled.fine_objects and scene.message().contains("full pictures"), "O again turns them on: " + scene.message())
	# V shows the picture on a CRT tube and takes it off again.
	check(not scene.crt_on(), "the CRT screen is off to begin with")
	press(scene, KEY_V)
	check(scene.crt_on() and scene.message() == "CRT screen on", "V: CRT on: " + scene.message())
	await ticks(1)
	check(scene.crt_frame_size() == scene.picture().get_size(), "the tube is told the picture's size")
	press(scene, KEY_V)
	check(not scene.crt_on() and scene.message() == "CRT screen off", "V again: CRT off: " + scene.message())
	# Tab shows the list of keys, with what each is set to now, and hides it.
	check(not scene.help_showing(), "the key list is hidden to begin with")
	press(scene, KEY_TAB)
	check(scene.help_showing(), "Tab shows the key list")
	for key_name in ["R", "F", "O", "I", "B", "V", "1 to 4", "F5", "F12", "Tab"]:
		check(scene.help_text().contains("\n" + key_name + "  ") or scene.help_text().begins_with(key_name + "  "), "the key list names " + key_name)
	check(scene.help_text().contains("CRT screen: off"), "the list shows the CRT is off: " + scene.help_text())
	press(scene, KEY_V)
	check(scene.help_text().contains("CRT screen: on"), "the list follows a key pressed while it shows")
	press(scene, KEY_V)
	press(scene, KEY_TAB)
	check(not scene.help_showing(), "Tab again hides the key list")
	# B softens the far objects and makes them sharp again.
	press(scene, KEY_B)
	check(scaled.soft and scene.message() == "Far objects softened", "B softens: " + scene.message())
	press(scene, KEY_B)
	check(not scaled.soft and scene.message() == "Far objects sharp", "B again sharpens: " + scene.message())
	# In-between pictures start in blend; I walks predict, late blend, off and back to blend.
	check(scaled.between == 2, "the scene starts in blend (%d)" % scaled.between)
	press(scene, KEY_I)
	check(scaled.between == 1 and scene.message() == "In-between pictures: predict (nothing late, may guess)", "I: predict: " + scene.message())
	press(scene, KEY_I)
	check(scaled.between == 3 and scene.message() == "In-between pictures: late blend (never guesses)", "I: late blend: " + scene.message())
	press(scene, KEY_I)
	check(scaled.between == 0 and scene.message() == "In-between pictures: off", "I: off: " + scene.message())
	press(scene, KEY_I)
	check(scaled.between == 2 and scene.message() == "In-between pictures: blend (about one update late)", "I: blend: " + scene.message())
	press(scene, KEY_O)  # without the full-picture objects it has nothing to draw with, and says so
	press(scene, KEY_I)
	check(scaled.between == 1 and scene.message().ends_with("needs R, F and O on"), "I says what it needs: " + scene.message())
	press(scene, KEY_O)
	scaled.scale = 2  # as if the window had been resized: the scale is set again
	scene.get_window().size_changed.emit()
	check(scaled.scale == want, "a resized window sets the scale again")
	press(scene, KEY_R)
	check(scaled.scale == 1 and not scaled.crisp, "R again turns it off")
	check(scene.message().contains("Original picture"), "and says so: " + scene.message())
	await ticks(1)
	check(scene.picture().get_size() == Vector2i(320, 224), "the picture is the original size again")
	check(scene.picture().get_pixel(319, 223).r8 == 1, "and holds the new frame")
	# W: the wide view. It needs the crisp road (and F and O), and says so.
	check(not scene.wide() and scene.picture_width() == 320, "the wide view is off to begin with")
	press(scene, KEY_W)
	check(not scaled.wide and scene.message() == "Wide view — needs R, F and O on", "W says what it needs: " + scene.message())
	scene.get_window().size = Vector2i(960, 672)
	press(scene, KEY_R)
	check(scaled.scale == 3 and scene.get_window().size == Vector2i(960, 672), "a 960x672 window: scale 3")
	press(scene, KEY_W)
	check(scaled.wide and scene.wide() and scene.picture_width() == 398, "W turns the wide view on")
	check(scene.get_window().size == Vector2i(1194, 672), "the window grows to 1194x672: %s" % scene.get_window().size)
	check(scaled.scale == 3 and scene.message() == "Wide view on (3×)", "the picture keeps its size, and the note says it: " + scene.message())
	check(scene.get_window().content_scale_size == Vector2i(398, 224), "the window is laid out 398 across")
	await ticks(2)
	check(scene.picture().get_size() == Vector2i(1194, 672), "the texture is 398 x scale across: %s" % scene.picture().get_size())
	check(scene.screen_size() == Vector2(398, 224), "and fills the wider view: %s" % scene.screen_size())
	press(scene, KEY_TAB)
	check(scene.help_text().contains("\nW  Wide view: on"), "the key list shows the wide view is on")
	press(scene, KEY_TAB)
	# Taking away something it needs puts the window back; giving it back widens it again.
	press(scene, KEY_O)
	check(scaled.wide and scene.picture_width() == 320 and scene.get_window().size == Vector2i(960, 672), "without O: 320 across in a 960x672 window again")
	press(scene, KEY_O)
	check(scene.picture_width() == 398 and scene.get_window().size == Vector2i(1194, 672), "with O back: wide again")
	press(scene, KEY_W)
	check(not scaled.wide and scene.message() == "Wide view off" and scene.get_window().size == Vector2i(960, 672), "W again: off, and the window is 960x672 again: " + scene.message())
	check(scene.get_window().content_scale_size == Vector2i(320, 224) and scaled.scale == 3, "laid out 320 across at scale 3")
	# A window with room for the wide picture at the same size is not touched at all.
	scene.get_window().size = Vector2i(1920, 1080)
	scene.get_window().size_changed.emit()
	check(scaled.scale == 4, "a 1920x1080 window shows the picture at 4")
	press(scene, KEY_W)
	check(scene.picture_width() == 398 and scene.get_window().size == Vector2i(1920, 1080) and scaled.scale == 4, "W in a window with room: the window is left as it is: %s" % scene.get_window().size)
	press(scene, KEY_W)
	check(scene.get_window().size == Vector2i(1920, 1080) and scaled.scale == 4, "and W off leaves it too: %s" % scene.get_window().size)
	scene.get_window().size = Vector2i(960, 672)
	scene.get_window().size_changed.emit()
	# A window that is not to be resized (fullscreen, maximised) shows the wide picture smaller.
	scene.keep_window_size = true
	press(scene, KEY_W)
	check(scene.get_window().size == Vector2i(960, 672) and scaled.scale == 2, "held at 960x672 the wide picture is at scale 2 (%d)" % scaled.scale)
	check(scene.message() == "Wide view on (2×)", "and the note says so: " + scene.message())
	scene.get_window().size = Vector2i(2388, 1344)
	check(scene.crisp_scale() == 3, "a 2388x1344 window shows the wide picture at 6: scale 3 (%d)" % scene.crisp_scale())
	scene.get_window().size = Vector2i(960, 672)
	press(scene, KEY_W)
	scene.keep_window_size = false
	check(scene.picture_width() == 320 and scene.get_window().content_scale_size == Vector2i(320, 224), "off again")
	await ticks(2)
	check(scene.screen_size() == Vector2(320, 224), "with the wide view off the view is 320x224 as before: %s" % scene.screen_size())
	press(scene, KEY_R)
	scene.set_console(RunningStub.new())  # a console that cannot scale is left alone
	press(scene, KEY_F)
	check(scene.message() == "", "no note without fine-point support")
	press(scene, KEY_O)
	check(scene.message() == "", "no note without fine-object support")
	press(scene, KEY_I)
	check(scene.message() == "", "no note without in-between support")
	press(scene, KEY_B)
	check(scene.message() == "", "no note without softening support")
	press(scene, KEY_R)
	check(scene.message() == "", "no note without scale support")
	scene.queue_free()

	var rom := OS.get_environment("RR2_ROM")
	if rom == "":
		print("SKIP ROM checks (RR2_ROM is not set)")
	else:
		scene = make_scene({"rom_path": rom})
		await ticks(1)
		check(scene.message() == "", "the ROM loads: " + scene.message())
		var start: int = scene.frame_count()
		await ticks(5)
		check(scene.frame_count() - start == 5, "one console frame per physics tick (%d)" % (scene.frame_count() - start))
		# Up to four console frames may run per drawn frame, so the game keeps its
		# speed on a display slower than 60 Hz; a long stall is dropped, not replayed.
		check(ProjectSettings.get_setting("physics/common/max_physics_steps_per_frame") == 4, "at most 4 physics steps per drawn frame")
		start = scene.frame_count()
		OS.delay_msec(500)
		await process_frame
		await process_frame
		var after_stall: int = scene.frame_count() - start
		check(after_stall >= 1 and after_stall <= 8, "a 500 ms stall (30 frames) is not caught up (%d frames ran)" % after_stall)
		check(scene.picture().get_width() == 320 and scene.picture().get_height() == 224, "picture size")
		scene.queue_free()
	await ticks(1)
	# Sound: turned on with the console, pulled once a frame at about the card's rate.
	scene = make_scene({"rom_path": "/nonexistent.gen"})
	await ticks(2)
	var loud := SoundStub.new()
	scene.set_console(loud)
	check(loud.sound and scene.sound_on(), "sound is turned on when the console can make it")
	await ticks(5)
	check(loud.rates.size() == loud.calls and loud.calls >= 4, "sound is taken once for every frame run")
	var mix := AudioServer.get_mix_rate()
	var close := true
	for rate in loud.rates:
		close = close and absf(rate - mix) <= mix * 0.005 + 1.0
	check(close, "the rate asked for stays within half a percent of the card's: %s" % str(loud.rates))
	# Too little waiting asks for more than the card's rate, too much for less.
	check(scene.sound_rate_for(0.04) == mix, "at the target the card's own rate is asked for")
	check(scene.sound_rate_for(0.03) > mix and scene.sound_rate_for(0.05) < mix, "the nudge goes the right way")
	check(is_equal_approx(scene.sound_rate_for(0.0), mix * 1.005), "it stops at half a percent up")
	check(is_equal_approx(scene.sound_rate_for(0.5), mix * 0.995), "and at half a percent down")
	check(scene.sound_rate_for(0.03) < scene.sound_rate_for(0.02), "and grows with the shortfall")
	# What is waiting is dropped by a reset.
	scene._playback.push_frame(Vector2.ZERO)
	check(scene.sound_buffered_seconds() > 0.0, "sound is waiting to be played")
	press(scene, KEY_F5)
	check(scene.sound_buffered_seconds() == 0.0, "F5 drops what was waiting")
	check(not scene.sound_muted(), "sound is not muted to begin with")
	press(scene, KEY_M)
	check(scene.sound_muted() and scene.message() == "Sound off", "M mutes: " + scene.message())
	var before_mute: int = loud.rates.size()
	await ticks(2)
	check(loud.rates.size() > before_mute, "muted sound keeps flowing, so unmuting is instant")
	press(scene, KEY_TAB)
	check(scene.help_text().contains("\nM  Sound: off"), "the key list shows sound is off: " + scene.help_text())
	press(scene, KEY_M)
	check(not scene.sound_muted() and scene.message() == "Sound on", "M again unmutes: " + scene.message())
	check(scene.help_text().contains("\nM  Sound: on"), "the key list follows")
	press(scene, KEY_TAB)
	# A console that cannot make sound is left alone, and M says so.
	scene.set_console(RunningStub.new())
	check(not scene.sound_on(), "no sound from a console without it")
	press(scene, KEY_M)
	check(scene.message() == "Sound not available", "M without sound: " + scene.message())
	press(scene, KEY_TAB)
	check(scene.help_text().contains("\nM  Sound: not available"), "the key list says so too")
	press(scene, KEY_TAB)
	# A console that halts: what was waiting is dropped, and M leaves the message up.
	var dying := DyingSoundStub.new()
	scene.set_console(dying)
	await ticks(2)
	scene._playback.push_frame(Vector2.ZERO)
	dying.dead = true
	await ticks(2)
	check(scene.message().contains("Press F5 to reset"), "the halt is shown: " + scene.message())
	check(scene.sound_buffered_seconds() == 0.0, "a halt drops what was waiting")
	var asked: int = dying.rates.size()
	await ticks(3)
	check(dying.rates.size() == asked, "a halted console is not asked for sound")
	press(scene, KEY_M)
	check(scene.message().contains("Press F5 to reset"), "M leaves the halt message up: " + scene.message())
	scene.queue_free()
	await ticks(1)
	# The settings are remembered from one launch to the next.
	var settings_file := OS.get_user_data_dir().path_join("test-settings-%d.cfg" % Time.get_ticks_usec())
	scene = make_scene({"rom_path": "/nonexistent.gen", "settings_path": settings_file})
	await ticks(2)
	var first_run := EverythingStub.new()
	scene.set_console(first_run)
	check(not first_run.crisp and first_run.speed == 100 and first_run.between == scene.BETWEEN_BLEND, "with nothing saved the window starts as it always did")
	press(scene, KEY_R)
	press(scene, KEY_B)
	press(scene, KEY_2)
	press(scene, KEY_V)
	press(scene, KEY_M)
	press(scene, KEY_I)  # blend -> predict
	press(scene, KEY_O)
	press(scene, KEY_F)
	check(FileAccess.file_exists(settings_file), "changing a setting saves the settings")
	scene.queue_free()
	await ticks(1)
	scene = make_scene({"rom_path": "/nonexistent.gen", "settings_path": settings_file})
	await ticks(2)
	var second_run := EverythingStub.new()
	scene.set_console(second_run)
	check(second_run.crisp and second_run.scale == scene.crisp_scale(), "the crisp road comes back (scale %d)" % second_run.scale)
	check(second_run.soft, "the softened far objects come back")
	check(second_run.speed == 200, "the game speed comes back: %d" % second_run.speed)
	check(scene.crt_on(), "the CRT screen comes back")
	check(scene.sound_muted(), "the mute comes back")
	check(second_run.between == scene.BETWEEN_PREDICT, "the in-between mode comes back: %d" % second_run.between)
	check(not second_run.fine_objects and not second_run.fine, "the two comparison switches come back")
	check(scene.message() == "", "restoring says nothing on screen: " + scene.message())
	press(scene, KEY_TAB)
	check(scene.help_text().contains("R  Crisp road: on") and scene.help_text().contains("M  Sound: off"), "the key list shows what was restored")
	press(scene, KEY_TAB)
	# A reset keeps them; a console that cannot do something is left alone.
	press(scene, KEY_F5)
	check(second_run.crisp and second_run.speed == 200, "F5 keeps the settings")
	scene.set_console(RunningStub.new())
	check(scene.message() == "", "a console without the settings is not asked for them")
	scene.queue_free()
	await ticks(1)
	# A file that is not settings is ignored.
	var junk := FileAccess.open(settings_file, FileAccess.WRITE)
	junk.store_string("this is not a settings file [[[")
	junk.close()
	scene = make_scene({"rom_path": "/nonexistent.gen", "settings_path": settings_file})
	await ticks(2)
	var third_run := EverythingStub.new()
	scene.set_console(third_run)
	check(not third_run.crisp and third_run.speed == 100, "a damaged settings file is ignored")
	# Values out of range are ignored too.
	var odd := ConfigFile.new()
	odd.set_value("picture", "between", 99)
	odd.set_value("game", "cpu_speed", 12345)
	odd.set_value("picture", "crisp", "yes please")
	odd.save(settings_file)
	scene.set_console(third_run)
	check(third_run.between == scene.BETWEEN_BLEND and third_run.speed == 100 and not third_run.crisp, "settings that make no sense are ignored: between %d speed %d" % [third_run.between, third_run.speed])
	DirAccess.remove_absolute(settings_file)
	scene.queue_free()
	await ticks(1)
	# The wide view is remembered, and comes back after the crisp road it needs.
	scene = make_scene({"rom_path": "/nonexistent.gen", "settings_path": settings_file})
	await ticks(2)
	scene.get_window().size = Vector2i(960, 672)
	var wide_first := EverythingStub.new()
	scene.set_console(wide_first)
	press(scene, KEY_R)
	press(scene, KEY_W)
	check(wide_first.wide and scene.picture_width() == 398, "R then W: the wide view is on")
	scene.queue_free()
	await ticks(1)
	root.size = Vector2i(960, 672)
	root.content_scale_size = Vector2i(320, 224)
	scene = make_scene({"rom_path": "/nonexistent.gen", "settings_path": settings_file})
	await ticks(2)
	var wide_second := EverythingStub.new()
	scene.set_console(wide_second)
	check(wide_second.crisp and wide_second.wide and scene.wide(), "the wide view comes back with the crisp road")
	check(scene.picture_width() == 398 and scene.get_window().size == Vector2i(1194, 672), "and the window is made wide for it: %s" % scene.get_window().size)
	check(scene.message() == "", "restoring it says nothing on screen: " + scene.message())
	# The capture's settings say so.
	press(scene, KEY_W)
	scene.get_window().content_scale_size = Vector2i(320, 224)
	DirAccess.remove_absolute(settings_file)
	scene.queue_free()
	await ticks(1)
	# P captures the moment: the picture, the memory, the session and the settings.
	var capture_root := OS.get_user_data_dir().path_join("test-captures-%d" % Time.get_ticks_usec())
	scene = make_scene({"rom_path": "/nonexistent.gen", "capture_dir": capture_root})
	await ticks(2)
	var recorder := CaptureStub.new()
	scene.set_console(recorder)
	await ticks(2)
	press(scene, KEY_P)
	check(scene.message().begins_with("Captured: " + capture_root), "P says where the capture went: " + scene.message())
	var made := DirAccess.get_directories_at(capture_root)
	check(made.size() == 1, "one capture folder: %s" % str(made))
	if made.size() == 1:
		var folder := capture_root.path_join(made[0])
		check(FileAccess.get_file_as_string(folder.path_join("session.txt")) == recorder.session, "the session is saved as the console gave it")
		var ram := FileAccess.get_file_as_bytes(folder.path_join("ram.bin"))
		check(ram.size() == 65536 and ram[0x1234] == 0x5A, "the memory is saved whole (%d bytes)" % ram.size())
		var saved := Image.load_from_file(folder.path_join("picture.png"))
		check(saved != null and saved.get_width() == scene.picture().get_width(), "the picture is saved")
		var settings := FileAccess.get_file_as_string(folder.path_join("settings.txt"))
		check(settings.contains("CPU speed: 200") and settings.contains("Crisp road: off"), "the settings are saved: " + settings)
		check(settings.contains("\nWide view: not available\n"), "the settings name the wide view")
		check(scene.last_capture() == folder, "the scene remembers the folder")
	press(scene, KEY_P)
	check(DirAccess.get_directories_at(capture_root).size() == 2, "a second capture gets its own folder")
	press(scene, KEY_TAB)
	check(scene.help_text().contains("\nP  Capture this moment"), "the key list has P: " + scene.help_text())
	press(scene, KEY_TAB)
	# A console that cannot give a session says so and saves nothing.
	scene.set_console(RunningStub.new())
	press(scene, KEY_P)
	check(scene.message() == "Capture not available", "P without a session: " + scene.message())
	check(DirAccess.get_directories_at(capture_root).size() == 2, "and nothing is saved")
	# A session the console could not keep is not saved as if it were whole.
	recorder.session = ""
	scene.set_console(recorder)
	press(scene, KEY_P)
	check(scene.message() == "Capture failed: the console has no session to save", "an empty session: " + scene.message())
	check(DirAccess.get_directories_at(capture_root).size() == 2, "and nothing is saved then either")
	for name in DirAccess.get_directories_at(capture_root):
		for file in DirAccess.get_files_at(capture_root.path_join(name)):
			DirAccess.remove_absolute(capture_root.path_join(name).path_join(file))
		DirAccess.remove_absolute(capture_root.path_join(name))
	DirAccess.remove_absolute(capture_root)
	scene.queue_free()
	await ticks(1)
	# A halted console can still be captured, and the halt message stays up.
	capture_root += "-halted"
	scene = make_scene({"rom_path": "/nonexistent.gen", "capture_dir": capture_root})
	await ticks(2)
	var stopped := CaptureStub.new()
	scene.set_console(stopped)
	await ticks(2)
	scene._halted = true
	press(scene, KEY_P)
	check(scene.message().begins_with(stopped.get_halt_message() + "\nPress F5 to reset.\nCaptured: " + capture_root), "a halted console is captured under its message: " + scene.message())
	for name in DirAccess.get_directories_at(capture_root):
		for file in DirAccess.get_files_at(capture_root.path_join(name)):
			DirAccess.remove_absolute(capture_root.path_join(name).path_join(file))
		DirAccess.remove_absolute(capture_root.path_join(name))
	DirAccess.remove_absolute(capture_root)
	scene.queue_free()

	quit(1 if failures > 0 else 0)
