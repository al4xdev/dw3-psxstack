/* The SPU core's internals (runtime/spu.c, spu_dsp.c): the pure DSP pieces and a read-only view of the voices, for
 * the core itself and for its host tests and tools (tests/spu/). The game's side uses include/psxstack/spu.h only.
 * Everything here follows psx-spx "Sound Processing Unit (SPU)"; docs/SOUND.md section 6 lists what is modelled, what is
 * assumed where psx-spx is silent, and how each piece is checked. */
#ifndef PORT_SPU_INTERNAL_H
#define PORT_SPU_INTERNAL_H

#include <stdint.h>

#include "spu.h"

#define SPU_VOICES 24
#define SPU_BLOCK_SAMPLES 28 /* one 16-byte ADPCM block */
#define SPU_FIR_TAPS 39      /* the reverb's resampling filter */

/* psx-spx's tables: the 512-entry interpolation table ("gaussian"), the ADPCM filter coefficients (SPU-ADPCM's five,
 * the CD-ROM page's pos/neg tables), the reverb's 39-tap resampling filter. */
extern const int16_t spu_gauss[512];
extern const int8_t spu_adpcm_pos[5], spu_adpcm_neg[5];
extern const int16_t spu_fir[SPU_FIR_TAPS];

static inline int32_t spu_clamp16(int32_t v) {
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v;
}

/* One ADPCM block (16 bytes: shift/filter, flags, 28 nibbles) into 28 samples. hist[0] is the last sample decoded
 * before the block, hist[1] the one before it; both are updated. */
void spu_adpcm_decode(const uint8_t block[16], int32_t hist[2], int16_t out[SPU_BLOCK_SAMPLES]);

/* The 4-point interpolation: s[0] oldest .. s[3] newest, `i` the pitch counter's bits 4-11. */
int32_t spu_gauss_interp(const int16_t s[4], unsigned i);

/* An envelope (ADSR or volume sweep): its level and its step counter. One tick is one 44,100 Hz sample. `rate` is the
 * 7-bit shift/step pair (shift << 2 | step) as the registers hold it. */
typedef struct SpuEnvelope {
    int32_t level;
    uint32_t counter;
} SpuEnvelope;
/* The envelope's tick, here and not in spu_dsp.c: the mix calls it three times per voice per sample (63 times a
 * frame), and across translation units it was a call every time - 3.2 ms of the SPU's 4.9. */
/* psx-spx "Envelope Operation depending on Shift/Step/Mode/Direction", one 44,100 Hz tick. The step and the counter
 * increment come from the 7-bit rate (shift = rate >> 2, step value = rate & 3); exponential increase above 6000h is
 * slowed (step / 4, or counter increment / 4, or both / 2 at shift 10); exponential decrease scales the step by the
 * level (>> 15: floor, so it reaches 0); the counter increment is at least 1 unless the rate is 7Fh (never steps); a
 * step happens when the counter's bit 15 sets, and takes 8000h off the counter (psx-spx leaves the counter after a step
 * open; with increments that divide 8000h this is the documented "1 SHL (shift-11) cycles" per step, and it makes
 * shifts 26..31 alike, as psx-spx reports rate 76h behaving like 6Ah). Saturation: increase to -8000h..7FFFh,
 * decrease to >= 0, decrease with the negative phase to -8000h..0. */
static inline void spu_envelope_tick(SpuEnvelope *e, unsigned rate, int exponential, int decrease, int negative) {
    int shift = (int)((rate >> 2) & 0x1F);
    int32_t step = 7 - (int32_t)(rate & 3);
    uint32_t inc;

    if (decrease != negative) {
        step = ~step; /* +7..+4 -> -8..-5 */
    }
    step = shift < 11 ? step * (1 << (11 - shift)) : step;
    inc = shift > 11 ? 0x8000u >> (shift - 11) : 0x8000u;
    if (exponential && !decrease && e->level > 0x6000) {
        if (shift < 10) {
            step >>= 2;
        } else if (shift >= 11) {
            inc >>= 2;
        } else {
            step >>= 1;
            inc >>= 1;
        }
    } else if (exponential && decrease) {
        step = (step * e->level) >> 15;
    }
    if ((rate & 0x7F) != 0x7F && inc == 0) {
        inc = 1;
    }
    e->counter += inc;
    if (!(e->counter & 0x8000)) {
        return;
    }
    e->counter -= 0x8000;
    e->level += step;
    if (!decrease) {
        e->level = spu_clamp16(e->level);
    } else if (negative) {
        e->level = e->level < -0x8000 ? -0x8000 : e->level > 0 ? 0 : e->level;
    } else if (e->level < 0) {
        e->level = 0;
    }
}

/* The ADSR phases (spu_voice_info's phase). */
enum { SPU_ATTACK, SPU_DECAY, SPU_SUSTAIN, SPU_RELEASE };

/* A voice's state as the tests and tools see it (read-only). */
typedef struct SpuVoiceInfo {
    uint32_t addr;          /* the current block's byte address in SPU RAM */
    uint32_t repeat;        /* the repeat address (bytes) */
    uint32_t counter;       /* the pitch counter: bits 12+ the sample in the block, 4-11 the interpolation index */
    int32_t level;          /* the ADSR level (ENVX) */
    int phase;              /* SPU_ATTACK .. SPU_RELEASE */
    int32_t out;            /* the last sample after the envelope (what PMON and the capture see) */
    int16_t vol_l, vol_r;   /* the current volumes */
    uint32_t keyons;        /* counters since the reset: key-ons, end blocks with repeat (a loop), end blocks */
    uint32_t loops;         /*   without repeat (the voice muted) */
    uint32_t mutes;
} SpuVoiceInfo;
void spu_voice_info(int voice, SpuVoiceInfo *info);

/* SPU RAM, read-only (SPU_RAM_SIZE bytes), and the number of samples rendered since the reset. */
const uint8_t *spu_ram(void);
uint64_t spu_samples(void);

/* The reverb's address arithmetic: the byte address of a work-area offset (bytes, relative to the current buffer
 * address, may be negative), wrapped into ESA..0x7FFFE as psx-spx describes; and the current buffer address. */
uint32_t spu_reverb_address(int32_t offset);
uint32_t spu_reverb_current(void);

#endif /* PORT_SPU_INTERNAL_H */
