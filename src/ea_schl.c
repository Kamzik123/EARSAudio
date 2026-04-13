/* EA SCHl / GSTR container — decode + encode for EA-XA v2 audio.
 *
 * The old EA audio container (pre-SNU) used by Godfather 1 (Xbox), early
 * Need for Speed / FIFA / SSX / etc. Produced by EA Canada's sx.exe.
 *
 * File layout:
 *   [SCHl block] magic "SCHl" + size_LE + "PT\0\0" platform_LE + TLV patches + 0xFF end
 *   [SCCl block] optional "count" chunk
 *   [SCDl block] magic "SCDl" + size_LE + block_samples_LE + audio data
 *   ... more SCDl ...
 *   [SCEl block] end-of-stream marker
 *
 * Patches (TLV): 1 byte type, 1 byte size, N bytes BE value. size=0xFF means
 * a 4-byte BE size follows for a blob (e.g. user data). We only read the ones
 * we need.
 *
 * Codec (EA-XA v2): mono, 28 samples per 15-byte frame. First byte (frame_info)
 * selects a two-tap predictor and shift; the remaining 14 bytes hold 28 signed
 * 4-bit residuals. If frame_info == 0xEE the frame is a literal: 1 byte marker
 * + 2 BE int16 history + 28 BE int16 PCM samples = 61 bytes.
 */

#include "ears.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Byte helpers                                                        */
/* ------------------------------------------------------------------ */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int16_t rd16be(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int16_t clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

#define MAGIC_SCHL 0x5343486C /* "SCHl" */
#define MAGIC_SCCL 0x5343436C /* "SCCl" */
#define MAGIC_SCDL 0x5343446C /* "SCDl" */
#define MAGIC_SCEL 0x5343456C /* "SCEl" */

/* EA-XA predictor tables (same four (c1, c2) pairs as XAS1, stored as int/256). */
static const int EA_XA_COEF1[4] = {   0, 240, 460, 392 };
static const int EA_XA_COEF2[4] = {   0,   0, -208, -220 };

/* ------------------------------------------------------------------ */
/* SCHl header parsing                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int      platform;
    int      version;       /* patch 0x80 */
    int      channels;      /* patch 0x82, default 1 */
    int      sample_rate;   /* patch 0x84 */
    int      num_samples;   /* patch 0x85 */
    int      loop_start;    /* patch 0x86 */
    int      loop_end;      /* patch 0x87 (+1) */
    int      has_loop;
    size_t   header_end;    /* file offset where the SCHl block ends (first SCCl/SCDl) */
} schl_header;

/* Read one TLV patch. Returns 1 if the "end" (0xFF) marker was seen. */
static int read_patch(const uint8_t *buf, size_t size, size_t *off, schl_header *h, int *end) {
    if (*off >= size) return 0;
    uint8_t t = buf[(*off)++];

    /* end marker */
    if (t == 0xFF) { *end = 1; return 1; }
    /* no-op markers */
    if (t == 0xFC || t == 0xFD) return 1;

    if (*off >= size) return 0;
    uint8_t len = buf[(*off)++];

    /* Blob with 32-bit BE size */
    if (len == 0xFF) {
        if (*off + 4 > size) return 0;
        uint32_t blob = rd32be(buf + *off);
        *off += 4 + blob;
        return 1;
    }
    if (len > 4) { *off += len; return 1; }

    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        if (*off >= size) return 0;
        v = (v << 8) | buf[(*off)++];
    }

    switch (t) {
        case 0x80: h->version     = (int)v; break;
        case 0x82: h->channels    = (int)v; break;
        case 0x84: h->sample_rate = (int)v; break;
        case 0x85: h->num_samples = (int)v; break;
        case 0x86: h->loop_start  = (int)v; h->has_loop = 1; break;
        case 0x87: h->loop_end    = (int)v + 1; break;
        default:   /* ignore (priority, codec selectors we don't need, etc.) */    break;
    }
    return 1;
}

static int parse_schl(const uint8_t *buf, size_t size, schl_header *h) {
    memset(h, 0, sizeof(*h));
    h->channels = 1;

    if (size < 0x10 || rd32be(buf) != MAGIC_SCHL) return 0;
    size_t block_size = rd32le(buf + 4);
    if (block_size < 0x10 || block_size > size) return 0;

    /* Platform tag: "PT\0\0" then platform LE */
    if (rd32be(buf + 8) != 0x50540000) return 0;
    h->platform = (int)((uint16_t)buf[0x0A] | ((uint16_t)buf[0x0B] << 8));

    size_t off = 0x0C;
    int end = 0;
    while (!end && off < block_size) {
        if (!read_patch(buf, block_size, &off, h, &end)) return 0;
    }
    h->header_end = block_size;

    /* Defaults only kick in if the patch was absent. num_samples must be set. */
    if (h->num_samples <= 0) return 0;
    if (h->sample_rate <= 0) h->sample_rate = 24000; /* Xbox default */
    if (h->version <= 0) h->version = 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Decode EA-XA v2 frames                                              */
/* ------------------------------------------------------------------ */

/* Decode up to `want` samples from the frame at *p. Returns samples produced;
 * advances *p past the consumed frame. State h1/h2 carries ADPCM history. */
static int decode_xa_v2_frame(const uint8_t **p, const uint8_t *end,
                              int16_t *out, int want, int *h1, int *h2) {
    if (*p >= end) return 0;
    uint8_t fi = **p;

    if (fi == 0xEE) {
        /* PCM literal: 1 marker + 2 BE hist + 28 BE int16 samples = 61 bytes */
        if (*p + 61 > end) return 0;
        *h1 = rd16be(*p + 1);
        *h2 = rd16be(*p + 3);
        int n = want < 28 ? want : 28;
        for (int i = 0; i < n; i++) out[i] = rd16be(*p + 5 + i * 2);
        *p += 61;
        /* keep running history as the latest two decoded samples for the next frame */
        if (n >= 2) { *h2 = out[n - 2]; *h1 = out[n - 1]; }
        else if (n == 1) { *h2 = *h1; *h1 = out[0]; }
        return n;
    }

    /* ADPCM: frame_info byte + 14 residual bytes = 15 bytes */
    if (*p + 15 > end) return 0;
    int c1 = EA_XA_COEF1[(fi >> 4) & 3];  /* top nibble selects predictor; v2 ignores bits 6-7 extras */
    int c2 = EA_XA_COEF2[(fi >> 4) & 3];
    int shift = (fi & 0x0F) + 8;

    int hh1 = *h1, hh2 = *h2;
    int n = want < 28 ? want : 28;
    for (int i = 0; i < n; i++) {
        uint8_t b = (*p)[1 + (i >> 1)];
        int nib = ((i & 1) == 0) ? (b >> 4) : (b & 0x0F);
        int s = ((int32_t)(nib << 28)) >> shift;                 /* sign-extend to 32b then scale */
        int y = clamp16((s + c1 * hh1 + c2 * hh2) >> 8);
        out[i] = (int16_t)y;
        hh2 = hh1;
        hh1 = y;
    }
    *h1 = hh1; *h2 = hh2;
    *p += 15;
    return n;
}

/* Decode the body: iterate SCDl blocks, decoding frames until num_samples reached.
 * Multi-channel: each block has a per-channel offset table at 0x0C, then each
 * channel's frame stream laid out independently. Channels carry their own h1/h2
 * history across blocks. Output PCM is channel-interleaved. */
static int decode_schl_body(const uint8_t *buf, size_t size, const schl_header *h,
                            int16_t **out_pcm, size_t *out_samples) {
    int N = h->channels;
    if (N < 1 || N > 8) return EARS_ERR_UNSUPPORTED;

    int16_t *pcm = (int16_t *)calloc((size_t)h->num_samples * N, sizeof(int16_t));
    if (!pcm) return EARS_ERR_MEMORY;

    int hist1[8] = {0};
    int hist2[8] = {0};

    size_t off = h->header_end;
    int filled_per_ch = 0;

    while (off + 8 <= size && filled_per_ch < h->num_samples) {
        uint32_t id   = rd32be(buf + off);
        uint32_t bsz  = rd32le(buf + off + 4);
        if (bsz < 8 || off + bsz > size) break;

        if (id == MAGIC_SCEL) break;
        if (id != MAGIC_SCDL) { off += bsz; continue; }

        if (bsz < 12) { off += bsz; continue; }
        int block_samples = (int)rd32le(buf + off + 8);

        size_t off_table = (size_t)N * 4;
        if (0x0C + off_table > bsz) { off += bsz; continue; }
        const uint8_t *block_end = buf + off + bsz;
        const uint8_t *table_end = buf + off + 0x0C + off_table;

        int decoded_this_block = block_samples;
        if (filled_per_ch + decoded_this_block > h->num_samples)
            decoded_this_block = h->num_samples - filled_per_ch;

        for (int c = 0; c < N; c++) {
            uint32_t ch_start = rd32le(buf + off + 0x0C + 4 * c);
            const uint8_t *p = table_end + ch_start;
            if (p > block_end) continue;

            int written = 0;
            while (written < decoded_this_block && p < block_end) {
                int16_t tmp[28];
                int want = decoded_this_block - written;
                if (want > 28) want = 28;
                int got = decode_xa_v2_frame(&p, block_end, tmp, want, &hist1[c], &hist2[c]);
                if (got == 0) break;
                for (int i = 0; i < got; i++) {
                    pcm[((size_t)filled_per_ch + (size_t)written + (size_t)i) * (size_t)N + (size_t)c] = tmp[i];
                }
                written += got;
            }
        }
        filled_per_ch += decoded_this_block;
        off += bsz;
    }

    *out_pcm = pcm;
    *out_samples = (size_t)filled_per_ch;
    return EARS_OK;
}

/* ------------------------------------------------------------------ */
/* Public decode entry                                                 */
/* ------------------------------------------------------------------ */

int ears_decode_schl(const uint8_t *data, size_t size, ears_info *info,
                     int16_t **out_pcm, size_t *out_samples) {
    schl_header h;
    if (!parse_schl(data, size, &h)) return EARS_ERR_FORMAT;
    if (h.channels < 1 || h.channels > 8) return EARS_ERR_UNSUPPORTED;

    if (info) {
        memset(info, 0, sizeof(*info));
        info->codec = 100;             /* synthetic id: EA-XA via SCHl */
        info->channels = h.channels;
        info->sample_rate = h.sample_rate;
        info->num_samples = h.num_samples;
        info->loop_flag = h.has_loop;
        info->loop_start = h.loop_start;
        info->loop_end = h.loop_end ? h.loop_end : h.num_samples;
    }
    return decode_schl_body(data, size, &h, out_pcm, out_samples);
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Encode 28 samples into a 15-byte ADPCM frame with the given predictor/shift.
 * Writes the 15-byte frame to out. Updates h1/h2 to the reconstructed history.
 * Sets *out_total_err to sum |err| and *out_worst to max |err|. */
static void encode_xa_frame_one(const int16_t *src, int predictor, int shift_raw,
                                int *h1, int *h2, uint8_t *out,
                                int *out_total_err, int *out_worst) {
    int c1 = EA_XA_COEF1[predictor];
    int c2 = EA_XA_COEF2[predictor];
    int hh1 = *h1, hh2 = *h2;
    int worst = 0, total_err = 0;

    out[0] = (uint8_t)((predictor << 4) | (shift_raw - 8));

    /* Decoder does (nibble << 28) >> shift_raw, then adds c1*h1+c2*h2 (Q8), then >>8.
     * So the nibble's contribution to the int16 output is nibble << (20 - shift_raw). */
    int divisor_shift = 20 - shift_raw;
    int divisor = 1 << divisor_shift;
    int half = divisor >> 1;

    for (int i = 0; i < 28; i++) {
        int pred_i16 = (c1 * hh1 + c2 * hh2) >> 8;
        int r = (int)src[i] - pred_i16;
        /* round half away from zero via division, so negatives don't round toward -inf */
        int nibble = (r >= 0) ? (r + half) / divisor : -(((-r) + half) / divisor);
        nibble = clampi(nibble, -8, 7);

        /* Reconstruct exactly how the decoder does (unsigned shift to avoid UB on
         * negative signed shift; then arithmetic >> to sign-extend). */
        uint32_t unib = (uint32_t)(nibble & 0x0F);
        int32_t resid_q8 = (int32_t)(unib << 28) >> shift_raw;
        int reconstructed = clampi((resid_q8 + c1 * hh1 + c2 * hh2) >> 8, -32768, 32767);

        int err = (int)src[i] - reconstructed;
        if (err < 0) err = -err;
        if (err > worst) worst = err;
        total_err += err;

        uint8_t u = (uint8_t)(nibble & 0x0F);
        int byte_idx = 1 + (i >> 1);
        if ((i & 1) == 0) out[byte_idx] = (uint8_t)(u << 4);
        else              out[byte_idx] |= u;

        hh2 = hh1;
        hh1 = reconstructed;
    }
    *h1 = hh1; *h2 = hh2;
    *out_total_err = total_err;
    *out_worst     = worst;
}

/* PCM-literal frame (0xEE): 1 marker + 2 BE h1 + 2 BE h2 + 28 BE int16 samples = 61 bytes.
 * The decoder sets its h1/h2 state from the marker's embedded values *before* outputting
 * the samples, so they must be the last two output samples from this frame to give the
 * next ADPCM frame a sane predictor input. */
static void encode_pcm_literal(const int16_t *src, int *h1, int *h2, uint8_t *out) {
    out[0] = 0xEE;
    int16_t new_h1 = src[27];
    int16_t new_h2 = src[26];
    out[1] = (uint8_t)((uint16_t)new_h1 >> 8);
    out[2] = (uint8_t)new_h1;
    out[3] = (uint8_t)((uint16_t)new_h2 >> 8);
    out[4] = (uint8_t)new_h2;
    for (int i = 0; i < 28; i++) {
        out[5 + i * 2]     = (uint8_t)((uint16_t)src[i] >> 8);
        out[5 + i * 2 + 1] = (uint8_t)src[i];
    }
    *h1 = new_h1;
    *h2 = new_h2;
}

/* Encode one frame. Picks the best ADPCM pred/shift; if the worst sample error
 * exceeds PCM_FALLBACK_THRESHOLD, emit a PCM-literal (61-byte) frame instead.
 * Returns the number of bytes written (15 or 61). */
#define PCM_FALLBACK_THRESHOLD 256   /* ~-42 dB worst-sample error */

static size_t encode_xa_frame(const int16_t *src, int *h1, int *h2, uint8_t *out) {
    uint8_t best_frame[15];
    int best_score = INT32_MAX;
    int best_worst = INT32_MAX;
    int best_h1 = *h1, best_h2 = *h2;

    /* Shift range 8..19 (field 0..11 in the 4-bit slot — the decoder accepts
     * up to 15, but shift_raw > 19 gives sub-unit steps which are useless for
     * int16). Higher shift_raw = smaller quantization step = finer quality, at
     * the cost of reduced residual range — the minimum-error search naturally
     * picks the finest shift whose 8-nibble range still covers the residual. */
    for (int pred = 0; pred < 4; pred++) {
        for (int shift_raw = 8; shift_raw <= 19; shift_raw++) {
            uint8_t tmp[15];
            int th1 = *h1, th2 = *h2;
            int total = 0, worst = 0;
            encode_xa_frame_one(src, pred, shift_raw, &th1, &th2, tmp, &total, &worst);
            if (total < best_score) {
                best_score = total;
                best_worst = worst;
                memcpy(best_frame, tmp, 15);
                best_h1 = th1; best_h2 = th2;
            }
        }
    }

    if (best_worst > PCM_FALLBACK_THRESHOLD) {
        encode_pcm_literal(src, h1, h2, out);
        return 61;
    }
    memcpy(out, best_frame, 15);
    *h1 = best_h1; *h2 = best_h2;
    return 15;
}

/* Write the SCHl header with just the minimum patches the runtime needs. */
static size_t write_schl_header(uint8_t *buf, int sample_rate, int num_samples, int channels) {
    size_t p = 0;
    memcpy(buf + p, "SCHl", 4); p += 4;
    p += 4; /* size, patched below */
    buf[p++] = 'P'; buf[p++] = 'T'; buf[p++] = 0; buf[p++] = 0;
    buf[p++] = 0; buf[p++] = 0;                                       /* platform LE = PC */
    buf[p++] = 0xFD;                                                   /* info section marker */
    buf[p++] = 0x80; buf[p++] = 0x01; buf[p++] = 0x02;                /* version=2 */
    buf[p++] = 0x85; buf[p++] = 0x04;                                  /* sample count (4 bytes BE) */
    buf[p++] = (uint8_t)(num_samples >> 24);
    buf[p++] = (uint8_t)(num_samples >> 16);
    buf[p++] = (uint8_t)(num_samples >> 8);
    buf[p++] = (uint8_t)num_samples;
    if (channels > 1) {
        buf[p++] = 0x82; buf[p++] = 0x01; buf[p++] = (uint8_t)channels;
    }
    buf[p++] = 0x84; buf[p++] = 0x02;                                  /* sample rate (2 bytes BE) */
    buf[p++] = (uint8_t)(sample_rate >> 8);
    buf[p++] = (uint8_t)sample_rate;
    buf[p++] = 0xFF;                                                   /* end */
    while (p & 3) buf[p++] = 0x00;                                     /* 4-byte align */
    wr32le(buf + 4, (uint32_t)p);
    return p;
}

int ears_encode_schl_memory_multi(const int16_t *pcm, size_t samples, int channels,
                                  int sample_rate, void **out_data, size_t *out_size) {
    if (!pcm || !out_data || !out_size) return EARS_ERR_ARG;
    if (samples == 0 || samples > 0x7FFFFFFF) return EARS_ERR_ARG;
    if (sample_rate <= 0) return EARS_ERR_ARG;
    if (channels < 1 || channels > 8) return EARS_ERR_UNSUPPORTED;

    /* Worst case every frame on every channel is a 61-byte PCM literal. */
    size_t max_frame_bytes = ((samples + 27) / 28) * 61 * (size_t)channels;
    size_t cap = 4096 + max_frame_bytes;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return EARS_ERR_MEMORY;

    /* SCHl block */
    size_t p = write_schl_header(buf, sample_rate, (int)samples, channels);

    /* SCCl block (stream count) */
    memcpy(buf + p, "SCCl", 4); p += 4;
    wr32le(buf + p, 0x0C); p += 4;
    wr32le(buf + p, 1);    p += 4;

    /* SCDl blocks. Target ~1596 samples (57 frames) per block per channel.
     * Each channel is encoded independently with its own h1/h2 history; the
     * block's offset table at 0x0C records where each channel's frame stream
     * starts (relative to the end of the offsets array). */
    const int frames_per_block = 57;
    size_t total_frames = (samples + 27) / 28;
    int hist1[8] = {0};
    int hist2[8] = {0};
    size_t sample_cursor = 0;

    /* Per-channel scratch: up to 57 * 61 bytes */
    uint8_t per_ch_scratch[8][57 * 61];
    size_t per_ch_used[8];

    for (size_t fstart = 0; fstart < total_frames; fstart += frames_per_block) {
        size_t fend = fstart + frames_per_block;
        if (fend > total_frames) fend = total_frames;
        int nframes = (int)(fend - fstart);
        int block_samples = nframes * 28;
        if (sample_cursor + (size_t)block_samples > samples)
            block_samples = (int)(samples - sample_cursor);

        for (int c = 0; c < channels; c++) {
            size_t used = 0;
            for (int f = 0; f < nframes; f++) {
                int16_t frame_pcm[28] = {0};
                size_t src_off = sample_cursor + (size_t)f * 28;
                size_t copy = 28;
                if (src_off + copy > samples) copy = samples - src_off;
                for (size_t i = 0; i < copy; i++)
                    frame_pcm[i] = pcm[(src_off + i) * (size_t)channels + (size_t)c];
                used += encode_xa_frame(frame_pcm, &hist1[c], &hist2[c],
                                        per_ch_scratch[c] + used);
            }
            per_ch_used[c] = used;
        }

        /* Block layout: header(12) + offset_table(4*N) + concatenated channel streams + pad-to-4. */
        size_t off_table = (size_t)channels * 4;
        size_t total_chan = 0;
        for (int c = 0; c < channels; c++) total_chan += per_ch_used[c];
        size_t block_data_size = off_table + total_chan;
        while (block_data_size & 3) block_data_size++;
        size_t block_size = 12 + block_data_size;

        if (p + block_size > cap) {
            size_t nc = cap * 2 + block_size;
            uint8_t *nb = (uint8_t *)realloc(buf, nc);
            if (!nb) { free(buf); return EARS_ERR_MEMORY; }
            buf = nb; cap = nc;
        }

        memcpy(buf + p, "SCDl", 4);
        wr32le(buf + p + 4, (uint32_t)block_size);
        wr32le(buf + p + 8, (uint32_t)block_samples);

        /* Write offset table; channel c starts at sum of previous channels' used bytes */
        size_t running = 0;
        for (int c = 0; c < channels; c++) {
            wr32le(buf + p + 0x0C + 4 * c, (uint32_t)running);
            running += per_ch_used[c];
        }
        /* Copy per-channel data after the table */
        size_t write_off = 0x0C + off_table;
        for (int c = 0; c < channels; c++) {
            memcpy(buf + p + write_off, per_ch_scratch[c], per_ch_used[c]);
            write_off += per_ch_used[c];
        }
        while (write_off < block_size) buf[p + write_off++] = 0x00;

        p += block_size;
        sample_cursor += (size_t)(nframes * 28);
        if (sample_cursor > samples) sample_cursor = samples;
    }

    /* SCEl end block */
    if (p + 8 > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, cap + 8);
        if (!nb) { free(buf); return EARS_ERR_MEMORY; }
        buf = nb; cap += 8;
    }
    memcpy(buf + p, "SCEl", 4);
    wr32le(buf + p + 4, 0x08);
    p += 8;

    *out_data = buf;
    *out_size = p;
    return EARS_OK;
}

int ears_encode_schl_memory(const int16_t *pcm, size_t samples, int sample_rate,
                            void **out_data, size_t *out_size) {
    return ears_encode_schl_memory_multi(pcm, samples, 1, sample_rate, out_data, out_size);
}

/* Minimal WAV reader (PCM int16, 1-8 channels). Self-contained so the SCHl
 * encoder doesn't depend on read_wav from ears.c. */
static int read_wav_any(const char *path, int16_t **out_pcm, size_t *out_samples,
                        int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return EARS_ERR_IO;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return EARS_ERR_FORMAT;
    }
    int have_fmt = 0;
    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t csz = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (!memcmp(chunk, "fmt ", 4)) {
            uint8_t fmt[40]; if (csz > sizeof(fmt)) { fclose(f); return EARS_ERR_FORMAT; }
            if (fread(fmt, 1, csz, f) != csz) { fclose(f); return EARS_ERR_IO; }
            fmt_tag  = (uint16_t)(fmt[0] | (fmt[1] << 8));
            channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            rate     = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits     = (uint16_t)(fmt[14] | (fmt[15] << 8));
            have_fmt = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_fmt || (fmt_tag != 1 && fmt_tag != 0xFFFE) || bits != 16
                    || channels < 1 || channels > 8) {
                fclose(f); return EARS_ERR_UNSUPPORTED;
            }
            size_t nsamp = csz / (size_t)(channels * 2);
            int16_t *buf = (int16_t *)malloc(nsamp * channels * sizeof(int16_t));
            if (!buf) { fclose(f); return EARS_ERR_MEMORY; }
            if (fread(buf, 1, csz, f) != csz) { free(buf); fclose(f); return EARS_ERR_IO; }
            fclose(f);
            *out_pcm = buf; *out_samples = nsamp;
            *out_channels = channels; *out_rate = (int)rate;
            return EARS_OK;
        } else {
            if (fseek(f, (long)csz, SEEK_CUR)) { fclose(f); return EARS_ERR_IO; }
        }
    }
    fclose(f);
    return EARS_ERR_FORMAT;
}

int ears_encode_schl_wav_to_file(const char *in_wav_path, const char *out_exa_path) {
    int16_t *pcm = NULL; size_t samples = 0; int channels = 0, rate = 0;
    int s = read_wav_any(in_wav_path, &pcm, &samples, &channels, &rate);
    if (s != EARS_OK) return s;
    void *data = NULL; size_t size = 0;
    s = ears_encode_schl_memory_multi(pcm, samples, channels, rate, &data, &size);
    free(pcm);
    if (s != EARS_OK) return s;

    FILE *f = fopen(out_exa_path, "wb");
    if (!f) { free(data); return EARS_ERR_IO; }
    size_t w = fwrite(data, 1, size, f);
    fclose(f); free(data);
    return w == size ? EARS_OK : EARS_ERR_IO;
}

int ears_probe_schl(const uint8_t *data, size_t size, ears_info *info) {
    schl_header h;
    if (!parse_schl(data, size, &h)) return EARS_ERR_FORMAT;
    memset(info, 0, sizeof(*info));
    info->codec = 100;
    info->channels = h.channels;
    info->sample_rate = h.sample_rate;
    info->num_samples = h.num_samples;
    info->loop_flag = h.has_loop;
    info->loop_start = h.loop_start;
    info->loop_end = h.loop_end ? h.loop_end : h.num_samples;
    return EARS_OK;
}
