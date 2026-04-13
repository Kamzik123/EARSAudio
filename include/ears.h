#ifndef EARS_H
#define EARS_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(EARS_BUILD_DLL)
#    define EARS_API __declspec(dllexport)
#  else
#    define EARS_API __declspec(dllimport)
#  endif
#else
#  define EARS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EARS_OK              = 0,
    EARS_ERR_IO          = -1,
    EARS_ERR_FORMAT      = -2,
    EARS_ERR_UNSUPPORTED = -3,
    EARS_ERR_MEMORY      = -4,
    EARS_ERR_ARG         = -5
} ears_status;

typedef struct {
    int      codec;        /* EAAC codec id (4 = XAS1) */
    int      channels;
    int      sample_rate;
    int      num_samples;  /* per channel */
    int      loop_flag;
    int      loop_start;
    int      loop_end;
} ears_info;

/* Probe a .exa.snu file in memory, fill info.  */
EARS_API ears_status ears_probe_memory(const void *data, size_t size, ears_info *out_info);

/* Decode to interleaved int16 PCM. Caller frees *out_pcm with ears_free. */
EARS_API ears_status ears_decode_memory(const void *data, size_t size,
                                        ears_info *out_info,
                                        int16_t **out_pcm, size_t *out_samples);

/* File helpers. out_path for wav. */
EARS_API ears_status ears_decode_file_to_wav(const char *in_path, const char *out_wav_path);
EARS_API ears_status ears_probe_file(const char *in_path, ears_info *out_info);

/* Encode interleaved int16 PCM to a XAS1-coded SNU file in memory.
 * out_data must be freed with ears_free. */
EARS_API ears_status ears_encode_memory(const int16_t *pcm, size_t samples,
                                        int channels, int sample_rate,
                                        void **out_data, size_t *out_size);

/* Encode a WAV file (PCM int16) to an .exa.snu file. */
EARS_API ears_status ears_encode_wav_to_file(const char *in_wav_path, const char *out_snu_path);

EARS_API void ears_free(void *p);

EARS_API const char *ears_strerror(ears_status s);

#ifdef __cplusplus
}
#endif

#endif
