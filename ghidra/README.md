# Reproducible RR206 Ghidra import

The import script verifies the known-good RR206 SHA-256 before invoking Ghidra.
It reads the ROM as a raw binary at address zero and writes only to the ignored
`ghidra-projects/` directory.

```sh
./scripts/import-ghidra-rr206.sh
```

Optional arguments are the ROM path and project directory:

```sh
./scripts/import-ghidra-rr206.sh /absolute/path/to/RR206.gen /tmp/rr2-ghidra
```

The pinned Ghidra 12.0.4 language ID is `68000:BE:32:MC68020`, big-endian with
32-bit addresses. Ghidra does not supply a distinct plain MC68000 language ID;
its `68000:BE:32:default` language describes a 68040. The MC68020 variant is the
least permissive available base specification, but analysts must still reject
instructions unavailable on the Genesis MC68000 if auto-analysis decodes data.

`GenesisImport.java` creates the ROM/RAM/hardware memory map before analysis and
seeds the exception-vector entry points. `ApplyRr206Annotations.java` applies
project annotations supported by the research notes. Re-running the command is
safe for the source ROM, but `-overwrite` replaces the imported program inside
the generated Ghidra project.

For focused follow-up work, `ReconRefs.java` lists references to absolute
addresses, `ReconRange.java` prints a bounded disassembly range, and
`ReconInstructionSearch.java` searches rendered instructions for operand text
such as `(0x12,A1)`.
