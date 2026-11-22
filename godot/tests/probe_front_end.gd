# Drive the front end without a person: press Start for 6 frames every 90
# frames from frame 600, and report whether anything halts.
# Run: RR2_ROM=<rom> Godot --headless --path godot --script res://tests/probe_front_end.gd
# Environment: RR2_ROM (required); PROBE_FRAMES (default 5400); PROBE_OUT
# (optional directory: a picture is saved there every 300 frames).
# This is an exploration tool, not a test: it has no expected result.
extends SceneTree


func _init() -> void:
	var console = ClassDB.instantiate("RoadRashConsole") if ClassDB.class_exists("RoadRashConsole") else null
	if console == null:
		print("native library not built; run scripts/build-godot-extension.sh")
		quit(1)
		return
	var error: String = console.load_rom(OS.get_environment("RR2_ROM"))
	if error != "":
		print(error)
		quit(1)
		return
	var total := int(OS.get_environment("PROBE_FRAMES")) if OS.get_environment("PROBE_FRAMES") != "" else 5400
	var out := OS.get_environment("PROBE_OUT")
	var halted := false
	for frame in total:
		var held := 0
		if frame >= 600 and frame % 90 < 6:
			held = console.PAD_START
		console.set_buttons(0, held)
		if not console.run_frame():
			print("HALT at frame %d: %s" % [frame, console.get_halt_message()])
			halted = true
			break
		if out != "" and frame % 300 == 299:
			var image := Image.create_from_data(320, 224, false, Image.FORMAT_RGBA8, console.get_frame())
			image.save_png("%s/frame-%05d.png" % [out, frame + 1])
	print("frames run: %d" % console.get_frame_count())
	console = null
	quit(1 if halted else 0)
