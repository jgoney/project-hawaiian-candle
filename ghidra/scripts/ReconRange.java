// Print a requested disassembly range: ReconRange.java START_HEX END_HEX.
// @category RoadRash

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ReconRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 2) {
            throw new IllegalArgumentException("usage: ReconRange.java START_HEX END_HEX");
        }
        long start = Long.parseUnsignedLong(arguments[0].replaceFirst("^0[xX]", ""), 16);
        long end = Long.parseUnsignedLong(arguments[1].replaceFirst("^0[xX]", ""), 16);
        InstructionIterator instructions = currentProgram.getListing().getInstructions(
            toAddr(start), true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            if (instruction.getAddress().getOffset() >= end) {
                break;
            }
            Function function = getFunctionContaining(instruction.getAddress());
            String owner = function == null ? "-" : function.getName();
            println(instruction.getAddress() + " [" + owner + "] " + instruction);
        }
    }
}
