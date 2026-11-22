// Search disassembled instructions for one or more case-insensitive text fragments.
// Usage: ReconInstructionSearch.java FRAGMENT [FRAGMENT ...]
// @category RoadRash

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ReconInstructionSearch extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) {
            throw new IllegalArgumentException(
                "usage: ReconInstructionSearch.java FRAGMENT [FRAGMENT ...]");
        }

        InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            String rendered = instruction.toString().toLowerCase();
            boolean matches = true;
            for (String argument : arguments) {
                if (!rendered.contains(argument.toLowerCase())) {
                    matches = false;
                    break;
                }
            }
            if (!matches) {
                continue;
            }

            Function function = getFunctionContaining(instruction.getAddress());
            String owner = function == null ? "-" : function.getName();
            println(instruction.getAddress() + " [" + owner + "] " + instruction);
        }
    }
}
