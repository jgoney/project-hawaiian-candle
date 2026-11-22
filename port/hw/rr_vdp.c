#include "rr_vdp.h"

#include <string.h>

#include "rr_mem.h"

#define STATUS_FIXED 0x3400     /* upper bits read back as on MAME's 315-5313 */
#define STATUS_FIFO_EMPTY 0x0200
#define STATUS_FRAME_INTERRUPT 0x0080
#define STATUS_VBLANK 0x0008
#define STATUS_HBLANK 0x0004

#define REG_MODE2 1
#define REG_AUTOINC 15
#define REG_DMA_LEN_LO 19
#define REG_DMA_LEN_HI 20
#define REG_DMA_SRC_LO 21
#define REG_DMA_SRC_MID 22
#define REG_DMA_SRC_HI 23
#define MODE2_DMA_ENABLE 0x10

#define CODE_DMA 0x20
enum { TARGET_VRAM = 1, TARGET_CRAM = 3, TARGET_VSRAM = 5 };
enum { READ_VRAM = 0, READ_VSRAM = 4, READ_CRAM = 8 };

static const char SAVE_MAGIC[8] = {'R', 'R', '2', 'V', 'D', 'P', '0', '1'};

void rr_vdp_reset(RrVdp *v) { memset(v, 0, sizeof *v); }

void rr_vdp_frame_interrupt(RrVdp *v) { v->status |= STATUS_FRAME_INTERRUPT; }

uint32_t rr_vdp_take_dma_words(RrVdp *v) {
    uint32_t words = v->dma_words;
    v->dma_words = 0;
    return words;
}

void rr_vdp_ack_frame_interrupt(RrVdp *v) { v->status &= (uint16_t)~STATUS_FRAME_INTERRUPT; }

/* Store one word at the current address in the memory the code selects. */
static void store_word(RrVdp *v, uint16_t value) {
    switch (v->code & 0x0F) {
    case TARGET_VRAM:
        v->vram[v->addr] = (uint8_t)(value >> 8);
        v->vram[v->addr ^ 1] = (uint8_t)value;
        v->vram_writes++;
        break;
    case TARGET_CRAM:
        v->cram[(v->addr >> 1) & 63] = value & 0x0EEE;
        break;
    case TARGET_VSRAM: {
        unsigned index = (v->addr >> 1) & 63;
        if (index < RR_VDP_VSRAM_ENTRIES) v->vsram[index] = value & 0x07FF;
        break;
    }
    default:
        break; /* writes with a read code are ignored */
    }
    v->addr = (uint16_t)(v->addr + v->reg[REG_AUTOINC]);
}

static uint32_t dma_length(const RrVdp *v) {
    uint32_t length = (uint32_t)v->reg[REG_DMA_LEN_HI] << 8 | v->reg[REG_DMA_LEN_LO];
    return length ? length : 0x10000;
}

static void dma_finish(RrVdp *v, uint32_t source_low16) {
    v->reg[REG_DMA_LEN_LO] = v->reg[REG_DMA_LEN_HI] = 0;
    v->reg[REG_DMA_SRC_LO] = (uint8_t)source_low16;
    v->reg[REG_DMA_SRC_MID] = (uint8_t)(source_low16 >> 8);
}

/* 68000 memory to VDP: `length` words, the source wrapping within its 128 KiB block. */
static void dma_from_68k(RrVdp *v, RrMachine *m) {
    uint32_t words = (uint32_t)v->reg[REG_DMA_SRC_MID] << 8 | v->reg[REG_DMA_SRC_LO];
    uint32_t block = (uint32_t)(v->reg[REG_DMA_SRC_HI] & 0x7F) << 17;
    v->dma_words += dma_length(v);
    for (uint32_t n = dma_length(v); n > 0; n--) {
        store_word(v, rr_read16(m, block | (words << 1)));
        words = (words + 1) & 0xFFFF;
    }
    dma_finish(v, words);
}

/* VRAM fill: the high byte of the data word, `length` times. */
static void dma_fill(RrVdp *v, uint16_t value) {
    uint8_t byte = (uint8_t)(value >> 8);
    v->vram_writes++;
    for (uint32_t n = dma_length(v); n > 0; n--) {
        v->vram[v->addr ^ 1] = byte;
        v->addr = (uint16_t)(v->addr + v->reg[REG_AUTOINC]);
    }
    dma_finish(v, (uint32_t)v->reg[REG_DMA_SRC_MID] << 8 | v->reg[REG_DMA_SRC_LO]);
}

/* VRAM to VRAM copy, byte by byte. */
static void dma_copy(RrVdp *v) {
    uint16_t source = (uint16_t)(v->reg[REG_DMA_SRC_MID] << 8 | v->reg[REG_DMA_SRC_LO]);
    v->vram_writes++;
    for (uint32_t n = dma_length(v); n > 0; n--) {
        v->vram[v->addr ^ 1] = v->vram[source ^ 1];
        source++;
        v->addr = (uint16_t)(v->addr + v->reg[REG_AUTOINC]);
    }
    dma_finish(v, source);
}

void rr_vdp_write_control(RrVdp *v, RrMachine *m, uint16_t value) {
    if (!v->pending) {
        if ((value & 0xC000) == 0x8000) {
            unsigned reg = (value >> 8) & 0x1F;
            if (reg < RR_VDP_REGISTERS) v->reg[reg] = (uint8_t)value;
            return;
        }
        v->pending = 1;
        v->first_word = value;
        v->addr = (uint16_t)((v->addr & 0xC000) | (value & 0x3FFF));
        v->code = (uint8_t)((v->code & 0x3C) | (value >> 14));
        return;
    }
    v->pending = 0;
    v->addr = (uint16_t)((v->addr & 0x3FFF) | (value & 3) << 14);
    v->code = (uint8_t)((v->code & 0x03) | ((value >> 2) & 0x3C));
    v->fill_pending = 0;
    if (!(v->code & CODE_DMA) || !(v->reg[REG_MODE2] & MODE2_DMA_ENABLE)) return;
    switch (v->reg[REG_DMA_SRC_HI] >> 6) {
    case 2:
        v->fill_pending = 1; /* runs when the fill value is written to the data port */
        break;
    case 3:
        dma_copy(v);
        break;
    default:
        dma_from_68k(v, m);
        break;
    }
}

void rr_vdp_write_data(RrVdp *v, uint16_t value) {
    v->pending = 0;
    store_word(v, value);
    if (v->fill_pending) {
        v->fill_pending = 0;
        dma_fill(v, value);
    }
}

uint16_t rr_vdp_read_data(RrVdp *v) {
    uint16_t value = 0;
    v->pending = 0;
    switch (v->code & 0x0F) {
    case READ_VRAM:
        value = (uint16_t)(v->vram[v->addr & 0xFFFE] << 8 | v->vram[v->addr | 1]);
        break;
    case READ_CRAM:
        value = v->cram[(v->addr >> 1) & 63];
        break;
    case READ_VSRAM: {
        unsigned index = (v->addr >> 1) & 63;
        value = index < RR_VDP_VSRAM_ENTRIES ? v->vsram[index] : 0;
        break;
    }
    default:
        break;
    }
    v->addr = (uint16_t)(v->addr + v->reg[REG_AUTOINC]);
    return value;
}

uint16_t rr_vdp_read_status(RrVdp *v, int in_vblank, int in_hblank) {
    uint16_t value = (uint16_t)(STATUS_FIXED | STATUS_FIFO_EMPTY | v->status);
    if (in_vblank) value |= STATUS_VBLANK;
    if (in_hblank) value |= STATUS_HBLANK;
    v->pending = 0;
    return value;
}

static void put16(FILE *f, uint16_t value) {
    fputc(value & 0xFF, f);
    fputc(value >> 8, f);
}

static uint16_t get16(FILE *f) {
    int low = fgetc(f), high = fgetc(f);
    return (uint16_t)((low & 0xFF) | (high & 0xFF) << 8);
}

int rr_vdp_save(const RrVdp *v, FILE *f) {
    fwrite(SAVE_MAGIC, 1, sizeof SAVE_MAGIC, f);
    fwrite(v->reg, 1, sizeof v->reg, f);
    fwrite(v->vram, 1, sizeof v->vram, f);
    for (int i = 0; i < 64; i++) put16(f, v->cram[i]);
    for (int i = 0; i < RR_VDP_VSRAM_ENTRIES; i++) put16(f, v->vsram[i]);
    put16(f, v->addr);
    put16(f, v->first_word);
    put16(f, v->status);
    fputc(v->code, f);
    fputc(v->pending, f);
    fputc(v->fill_pending, f);
    return ferror(f) ? 1 : 0;
}

int rr_vdp_load(RrVdp *v, FILE *f) {
    char magic[sizeof SAVE_MAGIC];
    if (fread(magic, 1, sizeof magic, f) != sizeof magic ||
        memcmp(magic, SAVE_MAGIC, sizeof magic) != 0) {
        fprintf(stderr, "rr_vdp_load: not an RR2VDP01 block\n");
        return 1;
    }
    if (fread(v->reg, 1, sizeof v->reg, f) != sizeof v->reg ||
        fread(v->vram, 1, sizeof v->vram, f) != sizeof v->vram) {
        fprintf(stderr, "rr_vdp_load: truncated\n");
        return 1;
    }
    for (int i = 0; i < 64; i++) v->cram[i] = get16(f);
    for (int i = 0; i < RR_VDP_VSRAM_ENTRIES; i++) v->vsram[i] = get16(f);
    v->addr = get16(f);
    v->first_word = get16(f);
    v->status = get16(f);
    v->code = (uint8_t)fgetc(f);
    v->pending = (uint8_t)fgetc(f);
    int last = fgetc(f);
    if (last == EOF) {
        fprintf(stderr, "rr_vdp_load: truncated\n");
        return 1;
    }
    v->fill_pending = (uint8_t)last;
    return 0;
}
