# Headless test of the native RoadRashConsole class.
# Run: Godot --headless --path godot --script res://tests/test_console.gd
# Environment: RR2_ROM (optional) path of the RR206 ROM; RR2_FRAME_OUT (optional)
# where to save the picture after 600 frames.
extends SceneTree

var failures := 0


func check(condition: bool, label: String) -> void:
	if condition:
		print("PASS ", label)
	else:
		print("FAIL ", label)
		failures += 1


func _init() -> void:
	check(ClassDB.class_exists("RoadRashConsole"), "class RoadRashConsole exists")
	if not ClassDB.class_exists("RoadRashConsole"):
		quit(1)
		return
	var console = ClassDB.instantiate("RoadRashConsole")
	check(console != null, "instantiate")
	check(console.get_reference_count() == 1, "one reference, so it is freed when dropped")
	check(console.WIDTH == 320 and console.HEIGHT == 224, "size constants")
	check(console.PAD_START == 128 and console.PAD_A == 64, "pad constants")
	check(not console.run_frame(), "run_frame is false before a ROM is loaded")
	check(console.get_frame().size() == 0, "no picture before a ROM is loaded")
	check(console.get_ram().size() == 0 and console.get_session() == "", "no RAM and no session before a ROM is loaded")
	check(console.get_cpu_speed() == 100, "CPU speed reads 100 before a ROM is loaded")
	console.set_cpu_speed(400)
	check(console.get_cpu_speed() == 100, "set_cpu_speed does nothing before a ROM is loaded")
	check(console.has_method("set_scale") and console.has_method("set_crisp_road"), "scale methods exist")
	check(console.has_method("set_fine_road") and console.get_fine_road(), "fine road points are on by default")
	check(console.has_method("set_fine_objects") and console.get_fine_objects(), "fine objects are on by default")
	check(console.has_method("set_between") and console.get_between() == 0, "in-between pictures are off before a ROM is loaded")
	check(console.has_method("set_soft_objects") and not console.get_soft_objects(), "far objects are sharp by default")
	check(console.has_method("set_wide") and not console.get_wide(), "the wide view is off before a ROM is loaded")
	check(console.get_scale() == 1 and console.get_frame_width() == 320 and console.get_frame_height() == 224, "scale reads 1 before a ROM is loaded")

	var error: String = console.load_rom("/nonexistent.gen")
	check(error.contains("/nonexistent.gen"), "load_rom names a missing file: " + error)
	check(not console.run_frame(), "run_frame is false after a failed load")

	var rom := OS.get_environment("RR2_ROM")
	if rom == "":
		print("SKIP ROM checks (RR2_ROM is not set)")
	else:
		error = console.load_rom(rom)
		check(error == "", "load_rom succeeds: " + error)
		var ran := 0
		for i in 600:
			if console.run_frame():
				ran += 1
		check(ran == 600, "600 frames ran (%d)" % ran)
		check(console.get_frame_count() == 600, "frame count")
		check(console.has_method("get_ram") and console.get_ram().size() == 65536, "the work RAM is 65,536 bytes")
		var session: String = console.get_session()
		check(session == "rr2-session 1\nend 600\n", "the session of 600 untouched frames: " + session)
		check(console.get_halt_message() == "", "not halted: " + console.get_halt_message())
		var frame: PackedByteArray = console.get_frame()
		check(frame.size() == 320 * 224 * 4, "picture is 320x224 RGBA")
		check(frame[3] == 255, "alpha is opaque")
		console.set_buttons(0, console.PAD_START)
		check(console.run_frame(), "runs with a button held")
		console.reset()
		check(console.get_frame_count() == 0, "reset clears the frame count")
		check(console.get_cpu_speed() == 100, "the CPU starts at the original speed")
		console.set_cpu_speed(800)
		check(console.get_cpu_speed() == 800, "set_cpu_speed round-trips")
		check(console.run_frame(), "runs at 800%")
		console.set_cpu_speed(5)
		check(console.get_cpu_speed() == 100, "a speed below 100 is clamped")
		console.set_cpu_speed(1000000)
		check(console.get_cpu_speed() == 6400, "a speed above 6400 is clamped")
		console.set_cpu_speed(100)
		console.set_scale(4)
		console.set_crisp_road(true)
		check(console.get_scale() == 4 and console.get_frame_width() == 1280 and console.get_frame_height() == 896, "scale 4 is 1280x896")
		check(console.run_frame(), "runs at scale 4")
		check(console.get_frame().size() == 1280 * 896 * 4, "the picture is 1280x896 RGBA")
		console.set_fine_road(false)
		check(not console.get_fine_road() and console.run_frame(), "runs with whole-pixel road points")
		console.set_fine_road(true)
		check(console.get_fine_road(), "fine road points round-trip")
		console.set_fine_objects(false)
		check(not console.get_fine_objects() and console.run_frame(), "runs with the original's objects")
		console.set_fine_objects(true)
		check(console.get_fine_objects(), "fine objects round-trip")
		check(console.get_between() == 0, "in-between pictures are off by default in the library")
		console.set_between(2)
		check(console.get_between() == 2 and console.run_frame(), "the in-between mode round-trips")
		console.set_between(9)
		check(console.get_between() == 2, "a mode that does not exist is ignored")
		console.set_between(0)
		check(console.has_method("set_sound") and not console.get_sound(), "sound is off by default in the library")
		check(console.take_sound(48000).size() == 0, "and none is handed out")
		console.set_sound(true)
		check(console.get_sound() and console.run_frame() and console.run_frame(), "sound turns on")
		var sound: PackedByteArray = console.take_sound(48000)
		check(sound.size() > 0 and sound.size() % 8 == 0, "sound comes as pairs of 32-bit floats: %d bytes" % sound.size())
		check(absf(sound.size() / 8.0 - 2 * 801.0) < 80, "about two frames' worth at 48,000")
		check(console.take_sound(0).size() == 0 and console.take_sound(-5).size() == 0, "a rate that makes no sense gives nothing")
		console.set_sound(false)
		check(not console.get_sound(), "and off again")
		console.set_soft_objects(true)
		check(console.get_soft_objects() and console.run_frame(), "softened far objects round-trip")
		console.set_soft_objects(false)
		# The wide view: 398 game pixels across while the crisp road, fine road and fine objects are on.
		check(not console.get_wide(), "the wide view is off after loading")
		console.set_scale(3)
		console.set_crisp_road(true)
		console.set_fine_road(true)
		console.set_fine_objects(true)
		console.set_wide(true)
		check(console.get_wide() and console.get_frame_width() == 1194 and console.get_frame_height() == 672, "wide at scale 3 is 1194x672")
		check(console.run_frame() and console.get_frame().size() == 1194 * 672 * 4, "the wide picture is 1194x672 RGBA")
		check(console.get_session().contains(" wide 1\n"), "the session lists the wide view")
		console.set_crisp_road(false)
		check(console.get_wide() and console.get_frame_width() == 960, "without the crisp road the picture is 960 across again")
		console.set_wide(false)
		check(not console.get_wide(), "wide off again")
		console.set_scale(99)
		check(console.get_scale() == 8, "a scale above 8 is clamped")
		console.set_scale(1)
		check(console.run_frame() and console.get_frame().size() == 320 * 224 * 4, "back to the original size")
		var out := OS.get_environment("RR2_FRAME_OUT")
		if out != "":
			var image := Image.create_from_data(320, 224, false, Image.FORMAT_RGBA8, frame)
			check(image.save_png(out) == OK, "saved " + out)
	console = null
	quit(1 if failures > 0 else 0)
