#include "ears.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s info   <input.exa.snu | input.exa>\n"
        "  %s decode <input.exa.snu | input.exa> <output.wav>\n"
        "  %s encode <input.wav> <output.exa.snu | output.exa> [--frames-per-block N]\n"
        "\n"
        "  encode output extension picks the container:\n"
        "    .exa.snu -> SNU / XAS1    .exa -> SCHl / EA-XA v2 (mono)\n",
        prog, prog, prog);
}

static const char *codec_name(int c) {
    switch (c) {
        case 0: return "NONE";
        case 2: return "PCM16BE";
        case 3: return "EAXMA";
        case 4: return "XAS1";
        case 5: return "EALAYER3_V1";
        case 6: return "EALAYER3_V2_PCM";
        case 7: return "EALAYER3_V2_SPIKE";
        case 8: return "GCADPCM";
        case 100: return "EA-XA v2 (SCHl)";
        default: return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "info") == 0) {
        ears_info info;
        ears_status s = ears_probe_file(argv[2], &info);
        if (s != EARS_OK) {
            fprintf(stderr, "probe: %s\n", ears_strerror(s));
            return 2;
        }
        printf("codec       : %d (%s)\n", info.codec, codec_name(info.codec));
        printf("channels    : %d\n", info.channels);
        printf("sample rate : %d Hz\n", info.sample_rate);
        printf("samples     : %d (%.2f s)\n", info.num_samples,
               (double)info.num_samples / info.sample_rate);
        printf("loop        : %d (%d..%d)\n", info.loop_flag, info.loop_start, info.loop_end);
        return 0;
    }

    if (strcmp(argv[1], "decode") == 0 && argc >= 4) {
        ears_status s = ears_decode_file_to_wav(argv[2], argv[3]);
        if (s != EARS_OK) {
            fprintf(stderr, "decode: %s\n", ears_strerror(s));
            return 2;
        }
        printf("wrote %s\n", argv[3]);
        return 0;
    }

    if (strcmp(argv[1], "encode") == 0 && argc >= 4) {
        ears_encode_opts opts = {0};
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--frames-per-block") == 0 && i + 1 < argc) {
                opts.frames_per_block = atoi(argv[++i]);
            } else {
                fprintf(stderr, "unknown encode option: %s\n", argv[i]);
                return 1;
            }
        }
        /* Pick container by output extension: ".exa" without ".snu" → SCHl, else SNU. */
        const char *out = argv[3];
        size_t n = strlen(out);
        int is_schl = n >= 4 && strcmp(out + n - 4, ".exa") == 0;
        ears_status s = is_schl
            ? ears_encode_schl_wav_to_file(argv[2], out)
            : ears_encode_wav_to_file_ex(argv[2], out, &opts);
        if (s != EARS_OK) {
            fprintf(stderr, "encode: %s\n", ears_strerror(s));
            return 2;
        }
        printf("wrote %s\n", out);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
