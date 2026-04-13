#include "ears.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* byte helpers */

static uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t rd_u32le(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static void wr_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int16_t clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* XAS1 tables (rw::audio::core::Xas1Dec) */

static const float XAS_COEFS[4][2] = {
    { 0.0f,      0.0f      },
    { 0.9375f,   0.0f      },
    { 1.796875f,-0.8125f   },
    { 1.53125f, -0.859375f },
};

/*
 * XAS1 block layout (per channel, 0x4c bytes = 128 samples):
 *   4 group headers × 4 bytes  (16 bytes)
 *   15 rows × 4 bytes          (60 bytes) - nibbles for 4 groups interleaved
 * Each group header (LE u32):
 *   bits  0- 3  filter index (0-3)
 *   bits  4-15  hist2 high 12 bits (low 4 zero)
 *   bits 16-19  shift index (0-12)
 *   bits 20-31  hist1 high 12 bits (low 4 zero)
 */
static void xas1_decode_block(const uint8_t *frame, int16_t *out /* 128 samples */) {
    for (int g = 0; g < 4; g++) {
        uint32_t hdr = rd_u32le(frame + g * 4);
        float c1 = XAS_COEFS[hdr & 0x0F][0];
        float c2 = XAS_COEFS[hdr & 0x0F][1];
        int16_t hist2 = (int16_t)((hdr >>  0) & 0xFFF0);
        int16_t hist1 = (int16_t)((hdr >> 16) & 0xFFF0);
        uint8_t shift = (uint8_t)((hdr >> 16) & 0x0F);

        int base = g * 32;
        out[base + 0] = hist2;
        out[base + 1] = hist1;

        for (int row = 0; row < 15; row++) {
            uint8_t byte = frame[16 + row * 4 + g];
            for (int i = 0; i < 2; i++) {
                int n = i == 0 ? (byte >> 4) & 0x0F : byte & 0x0F;
                int s = (int16_t)(n << 12) >> shift;
                int out_s = clamp16((int)(s + hist1 * c1 + hist2 * c2));
                out[base + 2 + row * 2 + i] = (int16_t)out_s;
                hist2 = hist1;
                hist1 = (int16_t)out_s;
            }
        }
    }
}

/* SNU/SNR parsing */

typedef struct {
    int version;
    int codec;
    int channels;
    int sample_rate;
    int type;
    int loop_flag;
    int num_samples;
    size_t stream_offset;
} eaac_hdr;

static ears_status parse_eaac(const uint8_t *snr8, eaac_hdr *h) {
    uint32_t h1 = rd_u32be(snr8);
    uint32_t h2 = rd_u32be(snr8 + 4);
    h->version     = (h1 >> 28) & 0x0F;
    h->codec       = (h1 >> 24) & 0x0F;
    h->channels    = ((h1 >> 18) & 0x3F) + 1;
    h->sample_rate = h1 & 0x3FFFF;
    h->type        = (h2 >> 30) & 0x03;
    h->loop_flag   = (h2 >> 29) & 0x01;
    h->num_samples = h2 & 0x1FFFFFFF;
    if (h->version != 0) return EARS_ERR_UNSUPPORTED;
    if (h->sample_rate <= 0 || h->sample_rate > 200000) return EARS_ERR_FORMAT;
    return EARS_OK;
}

static ears_status parse_snu(const uint8_t *data, size_t size, eaac_hdr *h, size_t *body_off) {
    if (size < 0x18) return EARS_ERR_FORMAT;
    /* SNU header @0x00 (8 bytes), extra (8), then SNR @0x10. Body offset in bytes 8..11 (LE or BE). */
    uint32_t off_le = rd_u32le(data + 8);
    uint32_t off_be = rd_u32be(data + 8);
    uint32_t off = (off_le < size && off_le >= 0x18) ? off_le
                 : (off_be < size && off_be >= 0x18) ? off_be
                 : 0;
    if (!off) return EARS_ERR_FORMAT;

    ears_status s = parse_eaac(data + 0x10, h);
    if (s != EARS_OK) return s;
    h->stream_offset = off;
    *body_off = off;
    return EARS_OK;
}

/* Decode all XAS1 blocks (EA SNS blocked layout) */

static ears_status decode_xas1_stream(const uint8_t *data, size_t size,
                                      size_t body_off, int channels, int num_samples,
                                      int16_t **out_pcm, size_t *out_samples) {
    int16_t *pcm = (int16_t *)malloc((size_t)num_samples * channels * sizeof(int16_t));
    if (!pcm) return EARS_ERR_MEMORY;

    size_t off = body_off;
    int samples_written = 0;
    int16_t chan_buf[128];

    while (off + 8 <= size && samples_written < num_samples) {
        uint32_t bhdr = rd_u32be(data + off);
        uint8_t id = (uint8_t)(bhdr >> 24);
        uint32_t bsize = bhdr & 0x00FFFFFF;
        if (bsize < 8 || off + bsize > size) { free(pcm); return EARS_ERR_FORMAT; }

        if (id != 0x00 && id != 0x80) { /* SNS v0 block ids */
            off += bsize;
            continue;
        }
        uint32_t bsamples = rd_u32be(data + off + 4);
        if (bsamples == 0) { off += bsize; continue; }

        size_t data_off = off + 8;
        size_t need = (size_t)channels * 0x4c;
        int frames_in_block = (int)((bsize - 8) / need);

        for (int f = 0; f < frames_in_block && samples_written < num_samples; f++) {
            for (int c = 0; c < channels; c++) {
                const uint8_t *frame = data + data_off + (size_t)f * need + (size_t)c * 0x4c;
                if (frame + 0x4c > data + size) { free(pcm); return EARS_ERR_FORMAT; }
                xas1_decode_block(frame, chan_buf);

                int to_write = 128;
                if (samples_written + to_write > num_samples)
                    to_write = num_samples - samples_written;
                for (int i = 0; i < to_write; i++)
                    pcm[((size_t)samples_written + i) * channels + c] = chan_buf[i];
            }
            samples_written += 128;
            if (samples_written > num_samples) samples_written = num_samples;
        }

        off += bsize;
        if (id == 0x80) break;
    }

    *out_pcm = pcm;
    *out_samples = (size_t)num_samples;
    return EARS_OK;
}

/* Public API */

ears_status ears_probe_memory(const void *data, size_t size, ears_info *out) {
    if (!data || !out) return EARS_ERR_ARG;
    eaac_hdr h; size_t body;
    ears_status s = parse_snu((const uint8_t *)data, size, &h, &body);
    if (s != EARS_OK) return s;
    memset(out, 0, sizeof(*out));
    out->codec = h.codec;
    out->channels = h.channels;
    out->sample_rate = h.sample_rate;
    out->num_samples = h.num_samples;
    out->loop_flag = h.loop_flag;
    out->loop_start = 0;
    out->loop_end = h.num_samples;
    return EARS_OK;
}

ears_status ears_decode_memory(const void *data, size_t size, ears_info *out_info,
                               int16_t **out_pcm, size_t *out_samples) {
    if (!data || !out_pcm || !out_samples) return EARS_ERR_ARG;
    eaac_hdr h; size_t body;
    ears_status s = parse_snu((const uint8_t *)data, size, &h, &body);
    if (s != EARS_OK) return s;
    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->codec = h.codec;
        out_info->channels = h.channels;
        out_info->sample_rate = h.sample_rate;
        out_info->num_samples = h.num_samples;
        out_info->loop_flag = h.loop_flag;
        out_info->loop_end = h.num_samples;
    }
    if (h.codec != 4 /* XAS1 */) return EARS_ERR_UNSUPPORTED;
    return decode_xas1_stream((const uint8_t *)data, size, body,
                              h.channels, h.num_samples, out_pcm, out_samples);
}

void ears_free(void *p) { free(p); }

const char *ears_strerror(ears_status s) {
    switch (s) {
        case EARS_OK: return "ok";
        case EARS_ERR_IO: return "I/O error";
        case EARS_ERR_FORMAT: return "bad format";
        case EARS_ERR_UNSUPPORTED: return "unsupported codec/version";
        case EARS_ERR_MEMORY: return "out of memory";
        case EARS_ERR_ARG: return "bad argument";
    }
    return "unknown";
}

static ears_status read_whole_file(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return EARS_ERR_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return EARS_ERR_IO; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return EARS_ERR_IO; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return EARS_ERR_MEMORY; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return EARS_ERR_IO; }
    fclose(f);
    *out = buf; *out_size = (size_t)n;
    return EARS_OK;
}

ears_status ears_probe_file(const char *in_path, ears_info *out_info) {
    uint8_t *buf; size_t size;
    ears_status s = read_whole_file(in_path, &buf, &size);
    if (s != EARS_OK) return s;
    s = ears_probe_memory(buf, size, out_info);
    free(buf);
    return s;
}

static ears_status write_wav(const char *path, const int16_t *pcm, size_t samples,
                             int channels, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return EARS_ERR_IO;
    uint32_t data_bytes = (uint32_t)(samples * channels * sizeof(int16_t));
    uint32_t byte_rate = (uint32_t)sample_rate * channels * 2;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff_size = 36 + data_bytes;
    hdr[4]=riff_size; hdr[5]=riff_size>>8; hdr[6]=riff_size>>16; hdr[7]=riff_size>>24;
    memcpy(hdr+8, "WAVEfmt ", 8);
    hdr[16]=16; hdr[17]=0; hdr[18]=0; hdr[19]=0;
    hdr[20]=1; hdr[21]=0;             /* PCM */
    hdr[22]=(uint8_t)channels; hdr[23]=0;
    hdr[24]=sample_rate; hdr[25]=sample_rate>>8; hdr[26]=sample_rate>>16; hdr[27]=sample_rate>>24;
    hdr[28]=byte_rate; hdr[29]=byte_rate>>8; hdr[30]=byte_rate>>16; hdr[31]=byte_rate>>24;
    uint16_t block_align = (uint16_t)(channels * 2);
    hdr[32]=(uint8_t)block_align; hdr[33]=(uint8_t)(block_align>>8);
    hdr[34]=16; hdr[35]=0;
    memcpy(hdr+36, "data", 4);
    hdr[40]=data_bytes; hdr[41]=data_bytes>>8; hdr[42]=data_bytes>>16; hdr[43]=data_bytes>>24;
    if (fwrite(hdr, 1, 44, f) != 44) { fclose(f); return EARS_ERR_IO; }
    if (fwrite(pcm, sizeof(int16_t), samples * channels, f) != samples * channels) {
        fclose(f); return EARS_ERR_IO;
    }
    fclose(f);
    return EARS_OK;
}

ears_status ears_decode_file_to_wav(const char *in_path, const char *out_wav_path) {
    uint8_t *buf; size_t size;
    ears_status s = read_whole_file(in_path, &buf, &size);
    if (s != EARS_OK) return s;
    ears_info info;
    int16_t *pcm = NULL; size_t samples = 0;
    s = ears_decode_memory(buf, size, &info, &pcm, &samples);
    free(buf);
    if (s != EARS_OK) return s;
    s = write_wav(out_wav_path, pcm, samples, info.channels, info.sample_rate);
    free(pcm);
    return s;
}

/* XAS1 encoder
 *
 * Mirrors rw::audio::core::Xas1Enc::EncodeBlock. Processes 128 samples per
 * channel per call as 4 groups of 32 samples. Each group:
 *  - 2 raw samples quantized to 12-bit signed (rounded to nearest, clamped to
 *    +/-32752 so that the 4-bit-masked low nibble can't turn MAX into -MAX),
 *  - 30 residual nibbles produced by subtracting the XAS coef prediction,
 *    scaled by 2^shift and truncated to 4-bit signed.
 *
 * Filter selection: pick filter with the smallest max |residual| across the 30
 * predicted samples (prediction uses the actual input samples as history to
 * match the game's heuristic). Special case: if filter 0 (no prediction) gives
 * max |residual| <= 7, keep filter 0.
 *
 * Shift selection: smallest shift in [0..12] such that the rounded
 * ((max_r << shift) + 2048) fits in a 4-bit signed nibble.
 *
 * Reconstruction uses the decoder formula so encoder/decoder stay bit-locked.
 */

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int quantize12(int s16) {
    int q = (s16 + 8) & ~0xF;
    if (q > 32752) q = 32752;
    if (q < -32768) q = -32768;
    return q;
}

static int pick_shift(int max_r) {
    int ar = max_r < 0 ? -max_r : max_r;
    int mask = 0x4000;
    int shift = 0;
    while (shift < 12) {
        if (((mask >> 3) + ar) & mask) break;
        ++shift;
        mask >>= 1;
    }
    return shift;
}

/* Encode one channel's 128-sample block in-place into dst (0x4c bytes). */
static void xas1_encode_block(const int16_t *src, uint8_t *dst) {
    uint8_t mini[4][19]; /* per-group uninterleaved: 4 hdr bytes + 15 nibble bytes */

    for (int g = 0; g < 4; g++) {
        const int16_t *gs = src + g * 32;
        int hist2_q = quantize12(gs[0]);
        int hist1_q = quantize12(gs[1]);

        /* filter selection: track max |r| per filter using actual-sample history */
        int best_filter = 0;
        float best_max = 1e30f;
        float filt_max[4] = {0};
        for (int f = 0; f < 4; f++) {
            float c1 = XAS_COEFS[f][0], c2 = XAS_COEFS[f][1];
            float h1 = (float)hist1_q, h2 = (float)hist2_q;
            float maxr = 0.0f;
            for (int n = 0; n < 30; n++) {
                float s = (float)gs[2 + n];
                float r = s - (h1 * c1 + h2 * c2);
                float ar = r < 0 ? -r : r;
                if (ar > maxr) maxr = ar;
                h2 = h1;
                h1 = s;
            }
            filt_max[f] = maxr;
            if (maxr < best_max) { best_max = maxr; best_filter = f; }
            if (f == 0 && maxr <= 7.0f) { best_filter = 0; break; }
        }

        float c1 = XAS_COEFS[best_filter][0];
        float c2 = XAS_COEFS[best_filter][1];
        int shift = pick_shift((int)filt_max[best_filter]);

        /* write group header: bits 0-3 filter, 4-15 hist2, 16-19 shift, 20-31 hist1 */
        uint32_t hdr = (uint32_t)((hist2_q & 0xFFF0) | (best_filter & 0x0F))
                     | ((uint32_t)((hist1_q & 0xFFF0) | (shift & 0x0F)) << 16);
        wr_u32le(mini[g], hdr);

        /* encode 30 nibbles using reconstructed history (must match decoder) */
        int h1 = hist1_q, h2 = hist2_q;
        for (int n = 0; n < 30; n++) {
            int s = gs[2 + n];
            float pred = (float)h1 * c1 + (float)h2 * c2;
            int residual = (int)(s - pred);
            int scaled = (residual << shift) + 2048;
            int nibble = (scaled >> 12);
            nibble = clampi(nibble, -8, 7);

            int rec = clamp16((int)((int16_t)(nibble << 12) >> shift) + (int)(h1 * c1 + h2 * c2));
            (void)pred;
            h2 = h1;
            h1 = rec;

            int byte_idx = 4 + (n >> 1);
            uint8_t u = (uint8_t)(nibble & 0x0F);
            if ((n & 1) == 0)
                mini[g][byte_idx] = (uint8_t)(u << 4);
            else
                mini[g][byte_idx] |= u;
        }
    }

    /* interleave (matches Xas1Enc::CustomInterleaveXAS): 4 headers, then 15 rows × 4 groups */
    for (int g = 0; g < 4; g++) {
        memcpy(dst + g * 4, mini[g], 4);
    }
    for (int row = 0; row < 15; row++) {
        for (int g = 0; g < 4; g++) {
            dst[16 + row * 4 + g] = mini[g][4 + row];
        }
    }
}

/* SNU/SNS writer */

typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   cap;
} bytebuf;

static int bb_reserve(bytebuf *b, size_t need) {
    if (b->size + need <= b->cap) return 1;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->size + need) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->buf, nc);
    if (!p) return 0;
    b->buf = p; b->cap = nc;
    return 1;
}

ears_status ears_encode_memory(const int16_t *pcm, size_t samples, int channels, int sample_rate,
                               void **out_data, size_t *out_size) {
    if (!pcm || !out_data || !out_size) return EARS_ERR_ARG;
    if (channels < 1 || channels > 8) return EARS_ERR_UNSUPPORTED;
    if (sample_rate <= 0 || sample_rate > 0x3FFFF) return EARS_ERR_ARG;
    if (samples == 0 || samples > 0x1FFFFFFF) return EARS_ERR_ARG;

    const size_t body_offset = 0x20;
    bytebuf bb = {0};
    if (!bb_reserve(&bb, body_offset)) return EARS_ERR_MEMORY;
    memset(bb.buf, 0, body_offset);
    bb.size = body_offset;

    /* SNU header */
    uint32_t groups = (uint32_t)(samples / 32);
    bb.buf[0] = 0x03;
    bb.buf[1] = 0x00;
    bb.buf[2] = 0x00;
    bb.buf[3] = (uint8_t)channels;
    wr_u32le(bb.buf + 4, groups);
    wr_u32le(bb.buf + 8, (uint32_t)body_offset);
    /* bytes 12..15 zero */

    /* SNR header (BE packed): version=0, codec=4 (XAS1), channels-1, sample_rate, type=1 (stream),
     * loop=0, num_samples. All .exa.snu files shipped by the game use type=1: the SNU container
     * embeds the SNS body and the game streams it from that offset (see PlaySnr / the
     * PLAY1PARAM_STREAMFILEOFFSET doc string). Writing type=0 produces files the runtime
     * mis-parses. */
    uint32_t h1 = ((uint32_t)0 << 28)
                | ((uint32_t)4 << 24)
                | (((uint32_t)channels - 1) << 18)
                | ((uint32_t)sample_rate & 0x3FFFF);
    uint32_t h2 = ((uint32_t)1 << 30) | ((uint32_t)0 << 29) | ((uint32_t)samples & 0x1FFFFFFF);
    wr_u32be(bb.buf + 0x10, h1);
    wr_u32be(bb.buf + 0x14, h2);

    /* Encode audio into 128-sample frames, then pack into SNS blocks. */
    size_t total_frames = (samples + 127) / 128;
    size_t bytes_per_frame = (size_t)channels * 0x4c;

    /* Scratch: padded input buffer per frame (interleaved int16), sized for any channel count */
    int16_t *frame_pcm = (int16_t *)malloc((size_t)128 * channels * sizeof(int16_t));
    if (!frame_pcm) return EARS_ERR_MEMORY;
    int16_t chan_buf[128];

    /* Emit blocks of up to `frames_per_block` frames. Game's streamed files use
     * 26-27 frames per block; RAM (type=0) files commonly use a single block. */
    const size_t frames_per_block = 256; /* safe upper bound; RAM is fine in one block too */

    size_t frames_remaining = total_frames;
    size_t sample_cursor = 0;
    size_t frame_cursor = 0;

    while (frames_remaining > 0) {
        size_t n_frames = frames_remaining < frames_per_block ? frames_remaining : frames_per_block;
        frames_remaining -= n_frames;
        int is_last = (frames_remaining == 0);

        size_t block_data_size = n_frames * bytes_per_frame;
        size_t block_size = 8 + block_data_size;
        if (block_size > 0x00FFFFFF) { free(frame_pcm); return EARS_ERR_FORMAT; }

        /* block header */
        if (!bb_reserve(&bb, block_size)) { free(frame_pcm); return EARS_ERR_MEMORY; }
        uint32_t bhdr = ((uint32_t)(is_last ? 0x80 : 0x00) << 24) | (uint32_t)block_size;
        wr_u32be(bb.buf + bb.size, bhdr);

        /* block samples count: total samples covered by this block's frames */
        size_t block_samples_covered = n_frames * 128;
        if (sample_cursor + block_samples_covered > samples) {
            /* last block may not fill its final frame fully */
            block_samples_covered = samples - sample_cursor;
        }
        wr_u32be(bb.buf + bb.size + 4, (uint32_t)block_samples_covered);
        bb.size += 8;

        for (size_t f = 0; f < n_frames; f++) {
            size_t src_start = (frame_cursor + f) * 128;
            memset(frame_pcm, 0, (size_t)128 * channels * sizeof(int16_t));
            size_t to_copy = 128;
            if (src_start + to_copy > samples) to_copy = samples - src_start;
            memcpy(frame_pcm, pcm + src_start * channels,
                   to_copy * channels * sizeof(int16_t));

            for (int c = 0; c < channels; c++) {
                for (int i = 0; i < 128; i++) chan_buf[i] = frame_pcm[i * channels + c];
                xas1_encode_block(chan_buf, bb.buf + bb.size + (size_t)c * 0x4c);
            }
            bb.size += bytes_per_frame;
        }
        frame_cursor += n_frames;
        sample_cursor += block_samples_covered;
    }

    free(frame_pcm);
    *out_data = bb.buf;
    *out_size = bb.size;
    return EARS_OK;
}

/* Tiny WAV reader (PCM int16, mono/stereo). */
static ears_status read_wav(const char *path, int16_t **out_pcm, size_t *out_samples,
                            int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return EARS_ERR_IO;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f); return EARS_ERR_FORMAT;
    }
    int have_fmt = 0;
    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    uint8_t chunk[8];
    while (fread(chunk, 1, 8, f) == 8) {
        uint32_t csz = rd_u32le(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[40]; if (csz > sizeof(fmt)) { fclose(f); return EARS_ERR_FORMAT; }
            if (fread(fmt, 1, csz, f) != csz) { fclose(f); return EARS_ERR_IO; }
            fmt_tag = (uint16_t)(fmt[0] | (fmt[1] << 8));
            channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            rate = rd_u32le(fmt + 4);
            bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            have_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt || (fmt_tag != 1 && fmt_tag != 0xFFFE) ||
                bits != 16 || channels < 1 || channels > 8) {
                fclose(f); return EARS_ERR_UNSUPPORTED;
            }
            size_t nsamp = csz / (size_t)(channels * 2);
            int16_t *pcm = (int16_t *)malloc(nsamp * channels * sizeof(int16_t));
            if (!pcm) { fclose(f); return EARS_ERR_MEMORY; }
            if (fread(pcm, 1, csz, f) != csz) { free(pcm); fclose(f); return EARS_ERR_IO; }
            fclose(f);
            *out_pcm = pcm; *out_samples = nsamp;
            *out_channels = channels; *out_rate = (int)rate;
            return EARS_OK;
        } else {
            if (fseek(f, (long)csz, SEEK_CUR) != 0) { fclose(f); return EARS_ERR_IO; }
        }
    }
    fclose(f);
    return EARS_ERR_FORMAT;
}

ears_status ears_encode_wav_to_file(const char *in_wav_path, const char *out_snu_path) {
    int16_t *pcm = NULL; size_t samples = 0; int channels = 0, rate = 0;
    ears_status s = read_wav(in_wav_path, &pcm, &samples, &channels, &rate);
    if (s != EARS_OK) return s;

    void *snu = NULL; size_t snu_size = 0;
    s = ears_encode_memory(pcm, samples, channels, rate, &snu, &snu_size);
    free(pcm);
    if (s != EARS_OK) return s;

    FILE *f = fopen(out_snu_path, "wb");
    if (!f) { free(snu); return EARS_ERR_IO; }
    size_t w = fwrite(snu, 1, snu_size, f);
    fclose(f);
    free(snu);
    return w == snu_size ? EARS_OK : EARS_ERR_IO;
}
