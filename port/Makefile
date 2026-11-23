# Native port runtime. Build outputs go to the ignored generated/port/build.
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
BUILD ?= ../generated/port/build
CODE ?= ../generated/port/rr206_code.c
CORE_SRC = core/rr_mem.c core/rr_run.c core/rr_hw_replay.c
CORE_HDR = core/rr_machine.h core/rr_mem.h core/rr_run.h core/rr_ops.h
HW_SRC = hw/rr_vdp.c hw/rr_vdp_render.c hw/rr_genesis.c hw/rr_sound.c
# The video chip's drawing alone, for what only reads its memory
VDP_SRC = hw/rr_vdp_render.c
HW_HDR = hw/rr_vdp.h hw/rr_genesis.h hw/rr_sound.h
RENDER_SRC = render/rr_road.c render/rr_objects.c render/rr_between.c render/rr_dashes.c render/rr_wide.c
RENDER_HDR = render/rr_road.h render/rr_objects.h render/rr_between.h render/rr_dashes.h render/rr_wide.h
HOST_SRC = host/rr_console.c host/rr_resample.c
HOST_HDR = host/rr_console.h host/rr_resample.h
# The two sound cores are other people's code, kept as downloaded
# (third_party/README.md) and built with their own flags.
SOUND_INC = -Ithird_party/z80 -Ithird_party/nuked-opn2
SOUND_OBJ = $(BUILD)/tp_z80.o $(BUILD)/tp_ym3438.o

.PHONY: test code trace-check boot-run race-replay vdp-render objects-draw console-lib session-replay godot-extension clean

test: $(BUILD)/test_core $(BUILD)/test_vdp $(BUILD)/test_road $(BUILD)/test_objects $(BUILD)/test_between $(BUILD)/test_dashes $(BUILD)/test_wide $(BUILD)/test_sound $(BUILD)/test_resample $(BUILD)/test_console
	$(BUILD)/test_core
	$(BUILD)/test_vdp
	$(BUILD)/test_road
	$(BUILD)/test_objects
	$(BUILD)/test_between
	$(BUILD)/test_dashes
	$(BUILD)/test_wide
	$(BUILD)/test_sound
	$(BUILD)/test_resample
	$(BUILD)/test_console


$(BUILD)/tp_z80.o: third_party/z80/z80.c third_party/z80/z80.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -w -c -o $@ third_party/z80/z80.c

$(BUILD)/tp_ym3438.o: third_party/nuked-opn2/ym3438.c third_party/nuked-opn2/ym3438.h
	@mkdir -p $(BUILD)
	$(CC) -std=c11 -O2 -w -c -o $@ third_party/nuked-opn2/ym3438.c

$(BUILD)/test_resample: tests/test_resample.c host/rr_resample.c host/rr_resample.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ihost -o $@ tests/test_resample.c host/rr_resample.c -lm

$(BUILD)/test_sound: tests/test_sound.c hw/rr_sound.c hw/rr_sound.h $(SOUND_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Ihw $(SOUND_INC) -o $@ tests/test_sound.c hw/rr_sound.c $(SOUND_OBJ)

$(BUILD)/test_console: tests/test_console.c $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR) $(HOST_SRC) $(HOST_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -Irender -Ihost -o $@ tests/test_console.c $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ) $(RENDER_SRC) $(HOST_SRC) -lm

$(BUILD)/test_road: tests/test_road.c $(CORE_HDR) $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw -Irender -o $@ tests/test_road.c $(RENDER_SRC) $(VDP_SRC)

$(BUILD)/test_between: tests/test_between.c $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw -Irender -o $@ tests/test_between.c $(RENDER_SRC) $(VDP_SRC)

$(BUILD)/test_dashes: tests/test_dashes.c $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw -Irender -o $@ tests/test_dashes.c $(RENDER_SRC) $(VDP_SRC)

$(BUILD)/test_wide: tests/test_wide.c $(HW_HDR) $(VDP_SRC) $(RENDER_SRC) $(RENDER_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw -Irender -o $@ tests/test_wide.c $(RENDER_SRC) $(VDP_SRC)

$(BUILD)/test_objects: tests/test_objects.c $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -Irender -o $@ tests/test_objects.c $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ) $(RENDER_SRC)

objects-draw: $(BUILD)/rr-objects-draw

$(BUILD)/rr-objects-draw: tools/rr_objects_draw_tool.c render/rr_objects.c $(RENDER_HDR) $(HW_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw -Irender -o $@ tools/rr_objects_draw_tool.c render/rr_objects.c

$(BUILD)/test_vdp: tests/test_vdp.c $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -o $@ tests/test_vdp.c $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ)

$(BUILD)/test_core: tests/test_core.c $(CORE_SRC) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -o $@ tests/test_core.c $(CORE_SRC)

code: $(BUILD)/rr206_code.o

$(BUILD)/rr206_code.o: $(CODE) $(CORE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -Icore -c -o $@ $(CODE)

trace-check: $(BUILD)/rr-trace-check

$(BUILD)/rr-trace-check: tools/rr_trace_check.c $(BUILD)/rr206_code.o $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -o $@ tools/rr_trace_check.c $(BUILD)/rr206_code.o $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ)

boot-run: $(BUILD)/rr-boot-run

$(BUILD)/rr-boot-run: tools/rr_boot_run.c $(BUILD)/rr206_code.o $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -o $@ tools/rr_boot_run.c $(BUILD)/rr206_code.o $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ)

race-replay: $(BUILD)/rr-race-replay

$(BUILD)/rr-race-replay: tools/rr_race_replay.c $(BUILD)/rr206_code.o $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR) $(RENDER_SRC) $(RENDER_HDR)
	$(CC) $(CFLAGS) -Icore -Ihw $(SOUND_INC) -Irender -o $@ tools/rr_race_replay.c $(BUILD)/rr206_code.o $(CORE_SRC) $(HW_SRC) $(SOUND_OBJ) $(RENDER_SRC)

vdp-render: $(BUILD)/rr-vdp-render

$(BUILD)/rr-vdp-render: tools/rr_vdp_render_tool.c $(CORE_SRC) $(CORE_HDR) $(HW_SRC) $(SOUND_OBJ) $(HW_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DRR_NO_GENERATED_CODE -Icore -Ihw -o $@ tools/rr_vdp_render_tool.c core/rr_mem.c core/rr_run.c hw/rr_vdp.c hw/rr_vdp_render.c

# The console (core, hardware, host API and the translated game) as a static library.
LIB_SRC = $(CORE_SRC) $(HW_SRC) $(RENDER_SRC) $(HOST_SRC)
LIB_OBJ = $(patsubst %.c,$(BUILD)/obj/%.o,$(LIB_SRC))

$(BUILD)/obj/%.o: %.c $(CORE_HDR) $(HW_HDR) $(RENDER_HDR) $(HOST_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -Icore -Ihw $(SOUND_INC) -Irender -Ihost -c -o $@ $<

console-lib: $(BUILD)/librr2core.a

$(BUILD)/librr2core.a: $(LIB_OBJ) $(SOUND_OBJ) $(BUILD)/rr206_code.o
	rm -f $@
	ar rcs $@ $(LIB_OBJ) $(SOUND_OBJ) $(BUILD)/rr206_code.o

# Do a session saved by the Godot window again (tools/rr_session_replay.c).
session-replay: $(BUILD)/rr-session-replay

$(BUILD)/rr-session-replay: tools/rr_session_replay.c $(BUILD)/librr2core.a $(HOST_HDR)
	$(CC) $(CFLAGS) -Ihost -o $@ tools/rr_session_replay.c $(BUILD)/librr2core.a -lm

# The Godot native extension. gdextension_interface.h is dumped from the
# installed Godot by scripts/build-godot-extension.sh. Godot hands out every
# interface function as one generic function pointer type that has to be cast,
# so that one warning is turned off for this file.
GODOT_HDR_DIR = ../generated/port/godot
GODOT_LIB = ../godot/bin/librr2.dylib

godot-extension: $(GODOT_LIB)

$(GODOT_LIB): host/rr_godot.c $(BUILD)/librr2core.a $(GODOT_HDR_DIR)/gdextension_interface.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-cast-function-type -fPIC -shared -Icore -Ihw -Ihost -I$(GODOT_HDR_DIR) -o $@ host/rr_godot.c $(BUILD)/librr2core.a -lm

clean:
	rm -rf $(BUILD)
