/* EALayer3 V2 decoder (covers EAAC codec 7 = EALAYER3_V2_SPIKE and codec 6 = V2_PCM).
 *
 * EALayer3 is MPEG-1 Layer III with EA's custom framing: the usual 32-bit MPEG
 * sync header is stripped and the side info is repacked more compactly, optionally
 * followed by an inline raw-PCM block used for transients (Spike) or prefetch
 * (PCM). We parse the EA-frames, rebuild standard free-bitrate MPEG-1 L3 frames,
 * and feed them to minimp3 for the actual IMDCT/Huffman/synthesis work.
 *
 * Reference: vgmstream src/coding/mpeg_custom_utils_ealayer3.c, originally
 * reverse-engineered by Zench (https://bitbucket.org/Zenchreal/ealayer3).
 */

#include "ears.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD       /* avoid SIMD header requirements on mingw */
/* EALayer3 is encoded as free-format MPEG with frames up to 144*640*1000/8000 = 11520
 * bytes worst-case for 8kHz. Raise minimp3's cap well above its default 2304 so sync
 * search can find the next frame boundary. */
#define MAX_FREE_FORMAT_FRAME_SIZE 12288
#include "minimp3.h"

/* ------------------------------------------------------------------ */
/* MSB-first bit reader / writer                                       */
/* ------------------------------------------------------------------ */

typedef struct { const uint8_t *buf; size_t len_bytes; size_t pos_bits; } bitr;
typedef struct { uint8_t *buf; size_t cap_bytes; size_t pos_bits; } bitw;

static uint32_t br_get(bitr *r, int n) {
    uint32_t v = 0;
    while (n--) {
        size_t p = r->pos_bits++;
        int bit = 0;
        if ((p >> 3) < r->len_bytes) bit = (r->buf[p >> 3] >> (7 - (p & 7))) & 1;
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}
static size_t br_pos(const bitr *r) { return r->pos_bits; }

static void bw_put(bitw *w, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--) {
        size_t p = w->pos_bits++;
        if ((p >> 3) >= w->cap_bytes) continue;
        int bit = (v >> i) & 1;
        if ((p & 7) == 0) w->buf[p >> 3] = 0;
        w->buf[p >> 3] |= (uint8_t)(bit << (7 - (p & 7)));
    }
}
static size_t bw_pos(const bitw *w) { return w->pos_bits; }
static void bw_align8(bitw *w) { while (w->pos_bits & 7) bw_put(w, 1, 0); }

/* ------------------------------------------------------------------ */
/* EA-frame parsing (V2 — covers codecs 6 and 7)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    /* V2 header */
    uint32_t v2_extended, v2_stereo_flag, v2_reserved, v2_frame_size;
    uint32_t v2_offset_mode, v2_offset_samples, v2_pcm_samples, v2_common_size;

    /* common header */
    uint32_t version_index, sample_rate_index, channel_mode, mode_extension;
    int      version, channels, sample_rate, mpeg1;

    /* side info */
    uint32_t granule_index;
    uint32_t scfsi[2];
    uint32_t main_data_size[2];
    uint32_t others_1[2];
    uint32_t others_2[2];

    /* derived */
    size_t data_offset_bits; /* where MPEG main data starts, relative to EA-frame start (bits) */
    size_t pre_size;         /* header bytes */
    size_t common_size;      /* common (header+side+data+padding) bytes */
    size_t pcm_size;         /* inline PCM bytes */
    size_t eaframe_size;     /* total bytes */
} ea_frame;

static int parse_common(bitr *r, ea_frame *f) {
    static const int version_tbl[4]  = { 3, -1, 2, 1 };
    static const int srate_tbl[4][4] = {
        { 11025, 12000,  8000, -1 },
        {    -1,    -1,    -1, -1 },
        { 22050, 24000, 16000, -1 },
        { 44100, 48000, 32000, -1 },
    };
    static const int chan_tbl[4] = { 2, 2, 2, 1 };

    size_t start = br_pos(r);

    f->version_index     = br_get(r, 2);
    f->sample_rate_index = br_get(r, 2);
    f->channel_mode      = br_get(r, 2);
    f->mode_extension    = br_get(r, 2);

    if (!f->version_index && !f->sample_rate_index && !f->channel_mode && !f->mode_extension) return 0;

    f->version     = version_tbl[f->version_index];
    f->channels    = chan_tbl[f->channel_mode];
    f->sample_rate = srate_tbl[f->version_index][f->sample_rate_index];
    f->mpeg1       = (f->version == 1);
    if (f->version < 0 || f->sample_rate < 0) return 0;

    int others_2_bits = f->mpeg1 ? (47 - 32) : (51 - 32);

    f->granule_index = br_get(r, 1);

    if (f->mpeg1 && f->granule_index == 1) {
        for (int i = 0; i < f->channels; i++) f->scfsi[i] = br_get(r, 4);
    }
    for (int i = 0; i < f->channels; i++) {
        f->main_data_size[i] = br_get(r, 12);
        f->others_1[i]       = br_get(r, 32);
        f->others_2[i]       = br_get(r, others_2_bits);
    }

    f->data_offset_bits = br_pos(r);
    size_t base_bits = br_pos(r) - start;
    size_t data_bits = 0;
    for (int i = 0; i < f->channels; i++) data_bits += f->main_data_size[i];
    size_t pad_bits = ((base_bits + data_bits) & 7) ? (8 - ((base_bits + data_bits) & 7)) : 0;

    /* skip the main data + padding so the caller's read cursor ends at end of common section */
    r->pos_bits += data_bits + pad_bits;

    f->common_size = (base_bits + data_bits + pad_bits) / 8;
    return 1;
}

/* Parse one EA V2 frame starting at flat[off]. Returns bytes consumed, or 0 on failure. */
static size_t parse_v2_frame(const uint8_t *flat, size_t flat_size, size_t off, ea_frame *f) {
    if (off + 2 > flat_size) return 0;
    memset(f, 0, sizeof(*f));

    bitr r = { flat + off, flat_size - off, 0 };

    f->v2_extended    = br_get(&r, 1);
    f->v2_stereo_flag = br_get(&r, 1);
    f->v2_reserved    = br_get(&r, 2);
    f->v2_frame_size  = br_get(&r, 12);
    f->pre_size = 2;

    if (f->v2_extended) {
        f->v2_offset_mode    = br_get(&r, 2);
        f->v2_offset_samples = br_get(&r, 10);
        f->v2_pcm_samples    = br_get(&r, 10);
        f->v2_common_size    = br_get(&r, 10);
        f->pre_size += 4;
    }

    if (!f->v2_extended || f->v2_common_size) {
        if (!parse_common(&r, f)) return 0;
        /* data_offset_bits was relative to start of EA-frame bits — adjust for pre_size */
    } else {
        f->channels = (int)(f->v2_stereo_flag + 1);
    }

    f->pcm_size = f->v2_pcm_samples * 2u * (uint32_t)f->channels;
    f->eaframe_size = f->pre_size + f->common_size + f->pcm_size;

    if (f->v2_frame_size != f->eaframe_size) return 0;
    if (off + f->eaframe_size > flat_size) return 0;
    return f->eaframe_size;
}

/* ------------------------------------------------------------------ */
/* Rebuild a standard free-bitrate MPEG frame from one or two EA-granules */
/* ------------------------------------------------------------------ */

static size_t rebuild_mpeg_frame(const uint8_t *flat,
                                 size_t off0, const ea_frame *f0,
                                 size_t off1, const ea_frame *f1,
                                 uint8_t *out, size_t out_cap) {
    if (!f0->common_size) return 0;
    if (f0->mpeg1 && (!f1 || !f1->common_size || f0->granule_index == f1->granule_index)) return 0;

    int expected;
    if (f0->mpeg1) expected = 144l * (320 * 2) * 1000l / f0->sample_rate;
    else           expected =  72l * (160 * 2) * 1000l / f0->sample_rate;
    if ((size_t)expected > out_cap) return 0;

    bitw w = { out, out_cap, 0 };
    memset(out, 0, (size_t)expected);

    /* MPEG frame header, free bitrate */
    bw_put(&w, 11, 0x7FF);
    bw_put(&w,  2, f0->version_index);
    bw_put(&w,  2, 0x01);              /* layer III */
    bw_put(&w,  1, 1);                  /* no CRC */
    bw_put(&w,  4, 0);                  /* bitrate index = free */
    bw_put(&w,  2, f0->sample_rate_index);
    bw_put(&w,  1, 0);                  /* padding */
    bw_put(&w,  1, 0);                  /* private */
    bw_put(&w,  2, f0->channel_mode);
    bw_put(&w,  2, f0->mode_extension);
    bw_put(&w,  1, 1);                  /* copyright */
    bw_put(&w,  1, 1);                  /* original */
    bw_put(&w,  2, 0);                  /* emphasis */

    if (f0->mpeg1) {
        int private_bits = (f0->channels == 1) ? 5 : 3;
        bw_put(&w, 9, 0);                /* main_data_begin */
        bw_put(&w, private_bits, 0);
        for (int i = 0; i < f1->channels; i++) bw_put(&w, 4, f1->scfsi[i]);
        for (int i = 0; i < f0->channels; i++) {
            bw_put(&w, 12,       f0->main_data_size[i]);
            bw_put(&w, 32,       f0->others_1[i]);
            bw_put(&w, 47 - 32,  f0->others_2[i]);
        }
        for (int i = 0; i < f1->channels; i++) {
            bw_put(&w, 12,       f1->main_data_size[i]);
            bw_put(&w, 32,       f1->others_1[i]);
            bw_put(&w, 47 - 32,  f1->others_2[i]);
        }
        /* main data: granule0 then granule1, bit-by-bit */
        bitr r0 = { flat + off0, 0x10000, f0->data_offset_bits };
        for (int i = 0; i < f0->channels; i++)
            for (uint32_t j = 0; j < f0->main_data_size[i]; j++) bw_put(&w, 1, br_get(&r0, 1));
        bitr r1 = { flat + off1, 0x10000, f1->data_offset_bits };
        for (int i = 0; i < f1->channels; i++)
            for (uint32_t j = 0; j < f1->main_data_size[i]; j++) bw_put(&w, 1, br_get(&r1, 1));
    } else {
        int private_bits = (f0->channels == 1) ? 1 : 2;
        bw_put(&w, 8, 0);
        bw_put(&w, private_bits, 0);
        for (int i = 0; i < f0->channels; i++) {
            bw_put(&w, 12,       f0->main_data_size[i]);
            bw_put(&w, 32,       f0->others_1[i]);
            bw_put(&w, 51 - 32,  f0->others_2[i]);
        }
        bitr r0 = { flat + off0, 0x10000, f0->data_offset_bits };
        for (int i = 0; i < f0->channels; i++)
            for (uint32_t j = 0; j < f0->main_data_size[i]; j++) bw_put(&w, 1, br_get(&r0, 1));
    }

    bw_align8(&w);
    size_t written = bw_pos(&w) / 8;
    if (written > (size_t)expected) return 0;
    /* remaining bytes stay zero-filled (ancillary), helps minimp3's free-bitrate sync */
    return (size_t)expected;
}

/* ------------------------------------------------------------------ */
/* Flatten SNS blocks → single continuous EA-frame byte stream          */
/* ------------------------------------------------------------------ */

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int flatten_blocks(const uint8_t *snu, size_t snu_size, size_t body_off,
                          uint8_t **out_flat, size_t *out_size) {
    size_t total = 0;
    for (size_t off = body_off; off + 8 <= snu_size;) {
        uint32_t bhdr  = rd32be(snu + off);
        uint8_t  id    = (uint8_t)(bhdr >> 24);
        uint32_t bsize = bhdr & 0x00FFFFFF;
        if (bsize < 8 || off + bsize > snu_size) return 0;
        if (id == 0x00 || id == 0x80) total += bsize - 8;
        off += bsize;
        if (id == 0x80) break;
    }
    uint8_t *flat = (uint8_t *)malloc(total ? total : 1);
    if (!flat) return 0;
    size_t fp = 0;
    for (size_t off = body_off; off + 8 <= snu_size;) {
        uint32_t bhdr  = rd32be(snu + off);
        uint8_t  id    = (uint8_t)(bhdr >> 24);
        uint32_t bsize = bhdr & 0x00FFFFFF;
        if (id == 0x00 || id == 0x80) {
            memcpy(flat + fp, snu + off + 8, bsize - 8);
            fp += bsize - 8;
        }
        off += bsize;
        if (id == 0x80) break;
    }
    *out_flat = flat;
    *out_size = total;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public decode entry                                                  */
/* ------------------------------------------------------------------ */

int ears_decode_ealayer3_v2(const uint8_t *snu, size_t snu_size, size_t body_off,
                            int channels, int num_samples,
                            int16_t **out_pcm, size_t *out_samples) {
    uint8_t *flat = NULL; size_t flat_size = 0;
    if (!flatten_blocks(snu, snu_size, body_off, &flat, &flat_size)) return EARS_ERR_MEMORY;

    int16_t *pcm = (int16_t *)calloc((size_t)num_samples * channels, sizeof(int16_t));
    if (!pcm) { free(flat); return EARS_ERR_MEMORY; }

    mp3dec_t dec;
    mp3dec_init(&dec);

    /* Rolling buffer: holds 3 frames. minimp3's free-format lock-on wants THREE
     * consecutive sync words visible; once locked only two are needed, but we keep
     * three to be safe. */
    uint8_t mpg_buf[MAX_FREE_FORMAT_FRAME_SIZE * 3 + 16];
    size_t  mpg_len = 0;        /* bytes currently in mpg_buf */
    size_t  pending_size = 0;   /* size of the frame at mpg_buf[0] awaiting decode */
    mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    size_t filled = 0; /* samples per channel written */
    int first_mpeg_frame = 1;

    size_t off = 0;
    while (off < flat_size && filled < (size_t)num_samples) {
        ea_frame f0;
        size_t eaf0_off = off;
        size_t n0 = parse_v2_frame(flat, flat_size, off, &f0);
        if (!n0) break;
        off += n0;

        /* copy granule0's inline PCM block (16-bit BE, channel-interleaved) */
        if (f0.v2_extended && f0.v2_pcm_samples) {
            const uint8_t *p = flat + eaf0_off + f0.pre_size + f0.common_size;
            for (uint32_t i = 0; i < f0.v2_pcm_samples && filled + i < (size_t)num_samples; i++) {
                for (int c = 0; c < f0.channels && c < channels; c++) {
                    int16_t s = (int16_t)((p[0] << 8) | p[1]);
                    pcm[(filled + i) * channels + c] = s;
                    p += 2;
                }
            }
            filled += f0.v2_pcm_samples;
            if (filled > (size_t)num_samples) filled = (size_t)num_samples;
        }

        if (!f0.common_size) continue; /* PCM-only frame */

        ea_frame f1; size_t eaf1_off = 0;
        int have_f1 = 0;
        if (f0.mpeg1) {
            if (off >= flat_size) break;
            eaf1_off = off;
            size_t n1 = parse_v2_frame(flat, flat_size, off, &f1);
            if (!n1) break;
            off += n1;
            have_f1 = 1;

            if (f1.v2_extended && f1.v2_pcm_samples) {
                const uint8_t *p = flat + eaf1_off + f1.pre_size + f1.common_size;
                for (uint32_t i = 0; i < f1.v2_pcm_samples && filled + i < (size_t)num_samples; i++) {
                    for (int c = 0; c < f1.channels && c < channels; c++) {
                        int16_t s = (int16_t)((p[0] << 8) | p[1]);
                        pcm[(filled + i) * channels + c] = s;
                        p += 2;
                    }
                }
                filled += f1.v2_pcm_samples;
                if (filled > (size_t)num_samples) filled = (size_t)num_samples;
            }
            if (!f1.common_size) continue;
        }

        /* Rebuild into a scratch buffer, then append to the rolling mpg_buf. */
        uint8_t scratch[MAX_FREE_FORMAT_FRAME_SIZE];
        size_t new_size = rebuild_mpeg_frame(flat, eaf0_off, &f0,
                                             have_f1 ? eaf1_off : 0, have_f1 ? &f1 : NULL,
                                             scratch, sizeof(scratch));
        if (!new_size) continue;
        if (mpg_len + new_size > sizeof(mpg_buf)) {
            /* Shouldn't happen, but reset rather than overflow. */
            mpg_len = 0; pending_size = 0;
        }
        if (pending_size == 0) pending_size = new_size;
        memcpy(mpg_buf + mpg_len, scratch, new_size);
        mpg_len += new_size;

        /* Only decode once we have 3 frames visible (minimp3's free-format lock-on
         * checks that sync words align at f0, f0+k, and f0+k+nextfb). */
        if (mpg_len < pending_size * 3) continue;

        mp3dec_frame_info_t info = {0};
        int got = mp3dec_decode_frame(&dec, mpg_buf, (int)mpg_len, frame_pcm, &info);
        int consumed = info.frame_bytes > 0 ? info.frame_bytes : (int)pending_size;
        if (consumed <= 0 || consumed > (int)mpg_len) {
            /* Desync — drop and continue */
            mpg_len = 0; pending_size = 0;
            continue;
        }
        /* Shift remaining (the just-appended next frame) down to the front */
        memmove(mpg_buf, mpg_buf + consumed, mpg_len - consumed);
        mpg_len -= consumed;
        pending_size = (size_t)consumed; /* next frame is the same free-format size */

        if (first_mpeg_frame) { first_mpeg_frame = 0; if (got == 0) continue; }
        if (got == 0) continue;

        int want_ch = info.channels < channels ? info.channels : channels;
        for (int i = 0; i < got && filled + (size_t)i < (size_t)num_samples; i++) {
            for (int c = 0; c < want_ch; c++) {
                pcm[(filled + i) * channels + c] = frame_pcm[i * info.channels + c];
            }
        }
        filled += (size_t)got;
        if (filled > (size_t)num_samples) filled = (size_t)num_samples;
    }

    /* Flush any remaining buffered frames (free-format no longer needs 3-frame
     * lookahead once locked, so 1 remaining frame is enough). */
    while (mpg_len >= pending_size && pending_size > 0 && filled < (size_t)num_samples) {
        mp3dec_frame_info_t info = {0};
        int got = mp3dec_decode_frame(&dec, mpg_buf, (int)mpg_len, frame_pcm, &info);
        int consumed = info.frame_bytes > 0 ? info.frame_bytes : (int)pending_size;
        if (consumed <= 0 || consumed > (int)mpg_len) break;
        memmove(mpg_buf, mpg_buf + consumed, mpg_len - consumed);
        mpg_len -= consumed;
        if (got == 0) continue;
        int want_ch = info.channels < channels ? info.channels : channels;
        for (int i = 0; i < got && filled + (size_t)i < (size_t)num_samples; i++) {
            for (int c = 0; c < want_ch; c++) {
                pcm[(filled + i) * channels + c] = frame_pcm[i * info.channels + c];
            }
        }
        filled += (size_t)got;
        if (filled > (size_t)num_samples) filled = (size_t)num_samples;
    }

    free(flat);
    *out_pcm = pcm;
    *out_samples = filled;
    return EARS_OK;
}
