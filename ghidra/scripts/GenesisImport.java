// Seed a raw Sega Genesis image before Ghidra auto-analysis.
// @category RoadRash

import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class GenesisImport extends GhidraScript {
    private static final long ROM_END = 0x000fffffL;

    @Override
    public void run() throws Exception {
        Memory memory = currentProgram.getMemory();
        MemoryBlock imported = memory.getBlock(toAddr(0));
        if (imported != null) {
            imported.setName("ROM");
            imported.setRead(true);
            imported.setWrite(false);
            imported.setExecute(true);
        }

        createBlock("Z80_RAM", 0x00a00000L, 0x2000L, true, true, false);
        createBlock("IO_REGISTERS", 0x00a10000L, 0x20L, true, true, false);
        createBlock("Z80_BUS_REQUEST", 0x00a11100L, 2L, true, true, false);
        createBlock("Z80_RESET", 0x00a11200L, 2L, true, true, false);
        createBlock("TMSS", 0x00a14000L, 4L, true, true, false);
        createBlock("VDP_PORTS", 0x00c00000L, 0x20L, true, true, false);
        createBlock("WORK_RAM", 0x00ff0000L, 0x10000L, true, true, true);

        label(0x00a10003L, "IO_PORT1_DATA");
        label(0x00a10005L, "IO_PORT2_DATA");
        label(0x00a10009L, "IO_PORT1_CONTROL");
        label(0x00a1000bL, "IO_PORT2_CONTROL");
        label(0x00a11100L, "Z80_BUS_REQUEST_REG");
        label(0x00a11200L, "Z80_RESET_REG");
        label(0x00a14000L, "TMSS_SECURITY_REG");
        label(0x00c00000L, "VDP_DATA_PORT");
        label(0x00c00004L, "VDP_CONTROL_PORT");

        Set<Long> seeded = new HashSet<>();
        for (int index = 1; index < 64; index++) {
            long target = Integer.toUnsignedLong(memory.getInt(toAddr(index * 4L)));
            if ((target & 1L) == 0 && target >= 0x200L && target <= ROM_END && seeded.add(target)) {
                Address entry = toAddr(target);
                disassemble(entry);
                createFunction(entry, null);
                currentProgram.getSymbolTable().addExternalEntryPoint(entry);
            }
        }

        nameFunction(0x00000200L, "reset_entry");
        nameFunction(0x000011b8L, "vblank_handler");
        nameFunction(0x000014beL, "hblank_handler");
        setPlateComment(toAddr(0x200), "VERIFIED: reset vector target (vector 1)");
        setPlateComment(toAddr(0x11b8), "VERIFIED: level-6/VBlank vector target (vector 30)");
        setPlateComment(toAddr(0x14be), "VERIFIED: level-4/HBlank vector target (vector 28)");
    }

    private void createBlock(
            String name, long start, long length, boolean read, boolean write, boolean execute)
            throws Exception {
        Address address = toAddr(start);
        if (currentProgram.getMemory().getBlock(address) != null) {
            return;
        }
        MemoryBlock block = currentProgram.getMemory().createUninitializedBlock(
            name, address, length, false);
        block.setRead(read);
        block.setWrite(write);
        block.setExecute(execute);
    }

    private void label(long value, String name) throws Exception {
        createLabel(toAddr(value), name, true, SourceType.USER_DEFINED);
    }

    private void nameFunction(long value, String name) throws Exception {
        Address address = toAddr(value);
        if (getFunctionAt(address) == null) {
            disassemble(address);
            createFunction(address, name);
        } else {
            getFunctionAt(address).setName(name, SourceType.USER_DEFINED);
        }
    }
}
