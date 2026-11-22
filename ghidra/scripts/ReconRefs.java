// Print references to one or more hexadecimal addresses.
// @category RoadRash

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ReconRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String argument : getScriptArgs()) {
            long value = Long.parseUnsignedLong(argument.replaceFirst("^0[xX]", ""), 16);
            println("=== references to " + String.format("%08x", value) + " ===");
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(
                toAddr(value));
            while (references.hasNext()) {
                Reference reference = references.next();
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                String owner = function == null
                    ? "<no function>"
                    : function.getName() + "@" + function.getEntryPoint();
                println(from + " [" + owner + "] " + reference.getReferenceType());
            }
        }
    }
}
