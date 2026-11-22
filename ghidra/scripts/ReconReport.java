// Print compact evidence useful during initial reconnaissance.
// @category RoadRash

import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ReconReport extends GhidraScript {
    private static final long[] TARGETS = {
        0x00a10000L, 0x00a10003L, 0x00a10005L, 0x00a10009L, 0x00a1000bL,
        0x00a11100L, 0x00a11200L
    };

    @Override
    public void run() throws Exception {
        println("=== hardware references and scalar constants ===");
        Set<String> emitted = new HashSet<>();
        for (long target : TARGETS) {
            Address address = toAddr(target);
            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(address);
            while (references.hasNext()) {
                Reference reference = references.next();
                emit(reference.getFromAddress(), target, "xref", emitted);
            }
        }

        InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            for (int operand = 0; operand < instruction.getNumOperands(); operand++) {
                for (Object object : instruction.getOpObjects(operand)) {
                    if (object instanceof Scalar) {
                        long value = ((Scalar) object).getUnsignedValue();
                        for (long target : TARGETS) {
                            if (value == target) {
                                emit(instruction.getAddress(), target, "scalar", emitted);
                            }
                        }
                    }
                }
            }
        }

        listing("reset_entry", 0x200L, 0x380L);
        listing("controller_port_1", 0x10f4L, 0x1156L);
        listing("controller_port_2", 0x1156L, 0x11b8L);
        listing("vblank_handler", 0x11b8L, 0x14beL);
        listing("hblank_handler", 0x14beL, 0x15c0L);
        listing("post_reset_entry", 0xcbcc8L, 0xcbe80L);
        listing("input_update_both_ports", 0xe64aL, 0xe684L);
        listing("input_update_variant", 0xe684L, 0xe730L);
        listing("input_update_third_caller", 0xe8e2L, 0xe930L);
        referencesTo("controller_port_1 callers", 0x10f4L);
        referencesTo("controller_port_2 callers", 0x1156L);
        referencesTo("post_reset_entry callers", 0xcbcc8L);
    }

    private void referencesTo(String name, long target) {
        println("=== " + name + " ===");
        ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(
            toAddr(target));
        while (references.hasNext()) {
            Reference reference = references.next();
            Address from = reference.getFromAddress();
            Function function = getFunctionContaining(from);
            String owner = function == null ? "<no function>" : function.getName();
            println(from + " [" + owner + "] " + reference.getReferenceType());
        }
    }

    private void emit(Address from, long target, String kind, Set<String> emitted) {
        Function function = getFunctionContaining(from);
        String owner = function == null
            ? "<no function>"
            : function.getName() + "@" + function.getEntryPoint();
        String line = String.format("%s %s -> %08x in %s", kind, from, target, owner);
        if (emitted.add(line)) {
            println(line);
        }
    }

    private void listing(String name, long start, long end) {
        println("=== " + name + " ===");
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
