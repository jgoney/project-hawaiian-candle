/* Test driver: runs translated snippet code from a script on stdin.
 *
 * Commands: "d N HEX", "a N HEX", "sr HEX", "pc HEX", "ram ADDR HEXBYTES",
 * "steps N". After running, prints registers and writes RAM to argv[2].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_mem.h"
#include "rr_run.h"

int main(int argc, char **argv) {
    static RrMachine m;
    static uint8_t rom[0x1000];
    char line[4096], bytes[4000];
    unsigned n, v, steps = 1;
    if (argc != 3) return 3;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) return 3;
    size_t rom_len = fread(rom, 1, sizeof rom, f);
    fclose(f);
    (void)rom_len;
    m.rom = rom;
    m.rom_size = sizeof rom;
    m.rom_crc32 = rr_code_rom_crc32;
    m.cpu.sr = 0x2700;
    while (fgets(line, sizeof line, stdin) != NULL) {
        if (sscanf(line, "d %u %x", &n, &v) == 2) m.cpu.d[n & 7] = v;
        else if (sscanf(line, "a %u %x", &n, &v) == 2) m.cpu.a[n & 7] = v;
        else if (sscanf(line, "sr %x", &v) == 1) m.cpu.sr = (uint16_t)v;
        else if (sscanf(line, "pc %x", &v) == 1) m.cpu.pc = v;
        else if (sscanf(line, "steps %u", &v) == 1) steps = v;
        else if (sscanf(line, "ram %x %3999s", &v, bytes) == 2) {
            for (size_t i = 0; i + 1 < strlen(bytes); i += 2) {
                unsigned b;
                sscanf(bytes + i, "%2x", &b);
                rr_write8(&m, v + (unsigned)(i / 2), (uint8_t)b);
            }
        }
    }
    for (unsigned i = 0; i < steps; i++) rr_step(&m);
    for (int i = 0; i < 8; i++) printf("d %d %08X\n", i, m.cpu.d[i]);
    for (int i = 0; i < 8; i++) printf("a %d %08X\n", i, m.cpu.a[i]);
    printf("pc %08X\nsr %04X\ncycles %llu\n", m.cpu.pc, m.cpu.sr, (unsigned long long)m.cpu.cycles);
    f = fopen(argv[2], "wb");
    if (f == NULL) return 3;
    fwrite(m.ram, 1, sizeof m.ram, f);
    fclose(f);
    return 0;
}
