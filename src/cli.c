#include "ears.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s info <input.exa.snu>\n"
        "  %s decode <input.exa.snu> <output.wav>\n"
        "  %s encode <input.wav> <output.exa.snu>\n",
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
        ears_status s = ears_encode_wav_to_file(argv[2], argv[3]);
        if (s != EARS_OK) {
            fprintf(stderr, "encode: %s\n", ears_strerror(s));
            return 2;
        }
        printf("wrote %s\n", argv[3]);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
