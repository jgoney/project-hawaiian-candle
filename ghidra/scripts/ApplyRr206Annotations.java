// Apply evidence-backed RR206 names after auto-analysis.
// @category RoadRash

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyRr206Annotations extends GhidraScript {
    @Override
    public void run() throws Exception {
        annotateFunction(0x00000200L, "reset_entry",
            "VERIFIED: reset vector target; begins hardware and RAM initialization.");
        annotateFunction(0x000011b8L, "vblank_handler",
            "VERIFIED: vector 30 target. Saves all registers, reads the frame callback from " +
            "0xFF0300, calls it when nonzero, restores registers, and returns from interrupt.");
        annotateFunction(0x000014beL, "hblank_handler",
            "VERIFIED: vector 28 target (level 4 / HBlank interrupt).");
        annotateFunction(0x000010f4L, "read_controller_port_1",
            "VERIFIED: reads a three-button pad from port 1 through 0xA10003/0xA10009; " +
            "returns active-high button bits in D0.b.");
        annotateFunction(0x00001156L, "read_controller_port_2",
            "VERIFIED: reads a three-button pad from port 2 through 0xA10005/0xA1000B; " +
            "returns active-high button bits in D0.b.");
        annotateFunction(0x0000e684L, "decode_selected_controller_input",
            "VERIFIED: selects a controller port, stores D0.b at 0xFF062C, and converts " +
            "direction/button bits into gameplay control state. Field meanings remain hypotheses.");
        annotateFunction(0x0000e8e2L, "read_both_controllers",
            "VERIFIED: reads ports 1 and 2 and stores raw active-high bytes at " +
            "0xFF062A and 0xFF062B before calling per-port decoders.");
        annotateFunction(0x000cbcc8L, "hyp_frontend_state_loop",
            "HYPOTHESIS: post-reset front-end/state loop. VERIFIED mechanics: schedules frame " +
            "callbacks through 0xFF0300 and jumps through the table at 0xCC274 using 0xFFF59E.");
        annotateFunction(0x0001a730L, "vehicle_acceleration_update",
            "VERIFIED: computes signed vehicle motion delta. Consumes control state 0xFF056C, " +
            "compares A1+0x12 against vehicle/gear tables, and writes A1+0x14.");
        annotateFunction(0x000175c4L, "integrate_vehicle_motion",
            "VERIFIED: iterates active vehicles. Ordinary path multiplies signed A1+0x14 by " +
            "the timestep at 0xFF1C08, adds it to speed at A1+0x12, and clamps below zero.");
        annotateFunction(0x0000dcdeL, "load_selected_bike_tuning",
            "VERIFIED: indexes the 16-entry pointer table at 0x278BA using the selected bike, " +
            "copies a 38-byte tuning record to 0xFF0546/0xFF0570, and loads secondary parameters.");

        label(0x00ff0300L, "frame_callback");
        label(0x00ff056cL, "hyp_primary_throttle_state");
        label(0x00ff0546L, "player_1_bike_tuning");
        label(0x00ff0570L, "player_2_bike_tuning");
        label(0x00ff062aL, "controller_1_raw");
        label(0x00ff062bL, "controller_2_raw");
        label(0x00ff062cL, "selected_controller_raw");
        label(0x00ff1c08L, "physics_timestep");
        label(0x00ff27aaL, "hyp_primary_steering_state");
        label(0x00fff59eL, "hyp_frontend_state_index");
        label(0x000cc274L, "hyp_frontend_state_jump_table");
        label(0x000cbdd0L, "hyp_frontend_state_dispatch");
        label(0x0002744aL, "runtime_vehicle_tuning_pointer_table");
        label(0x000277aeL, "bike_secondary_parameter_table");
        label(0x000278baL, "bike_tuning_pointer_table");
    }

    private void annotateFunction(long value, String name, String comment) throws Exception {
        Address address = toAddr(value);
        Function function = getFunctionAt(address);
        if (function == null) {
            disassemble(address);
            function = createFunction(address, name);
        }
        if (function != null) {
            function.setName(name, SourceType.USER_DEFINED);
        }
        setPlateComment(address, comment);
    }

    private void label(long value, String name) throws Exception {
        createLabel(toAddr(value), name, true, SourceType.USER_DEFINED);
    }
}
