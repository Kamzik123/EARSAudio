# EARSAudio

Decoder and encoder for EA `.exa.snu` audio (EAAudioCore / RwAudioCore "SnP1"),
as used by EA Redwood Shores / Visceral titles — Dead Space 1 & 2, The Godfather 2,
etc. Supports the XAS1 codec (decode + encode) and EALayer3 V2 PCM / Spike
(decode only).

Produces:
- `libears.dll` / `libears.a` — the C API (see `include/ears.h`)
- `ears_cli.exe` — standalone command-line tool

## Building

Requires CMake ≥ 3.15 and any C11 compiler. Tested with MinGW-w64 gcc 8.1 and
MSVC 2022.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Artifacts land in `build/bin/`.

## CLI usage

```text
ears_cli info   <input.exa.snu>
ears_cli decode <input.exa.snu> <output.wav>
ears_cli encode <input.wav>     <output.exa.snu> [--frames-per-block N]
```

Examples:

```sh
ears_cli info   sample.exa.snu
ears_cli decode sample.exa.snu sample.wav
ears_cli encode voice.wav      voice.exa.snu
ears_cli encode voice.wav      voice.exa.snu --frames-per-block 26
```

`encode` accepts 16-bit PCM WAV (`WAVE_FORMAT_PCM` or `WAVE_FORMAT_EXTENSIBLE`),
1–8 channels, any sample rate ≤ 262143 Hz.

`--frames-per-block` controls the SNS block granularity. Default is 256 frames
per block (one big block for short clips). The game's streamed files ship with
26 frames per block; pass `--frames-per-block 26` to match that layout.

## Library API

All entry points are declared in `include/ears.h`. Functions return
`ears_status` (`EARS_OK` on success, negative on error; pass to
`ears_strerror` for a message). Buffers returned via out-pointers are
owned by the caller — free with `ears_free`.

### Probe / decode

```c
#include <ears.h>

ears_info info;
ears_probe_file("in.exa.snu", &info);
/* info.codec, info.channels, info.sample_rate, info.num_samples, info.loop_flag */

/* File → WAV (handles XAS1, EALayer3 V2 PCM, EALayer3 V2 Spike) */
ears_decode_file_to_wav("in.exa.snu", "out.wav");

/* In-memory */
int16_t *pcm; size_t samples;
ears_probe_memory(buf, size, &info);
ears_decode_memory(buf, size, &info, &pcm, &samples);
ears_free(pcm);
```

### Encode

```c
/* Simple: defaults (type=1 stream, 256 frames/block) */
ears_encode_wav_to_file("in.wav", "out.exa.snu");
ears_encode_memory(pcm, samples, channels, rate, &snu_buf, &snu_size);
ears_free(snu_buf);

/* With options */
ears_encode_opts opts = {0};
opts.frames_per_block = 26;     /* match the game's streaming block shape */
ears_encode_wav_to_file_ex("in.wav", "out.exa.snu", &opts);
ears_encode_memory_ex(pcm, samples, channels, rate, &opts, &snu_buf, &snu_size);
```

`ears_encode_opts` fields:

| Field | Default | Meaning |
|-------|---------|---------|
| `frames_per_block` | `0` → 256 | SNS frames per block, 1..524287. The game uses 26 for streamed files. |

Encoder only emits XAS1. EALayer3 encoding is not supported.

## Format notes

- **SNU container** (16 bytes): `03 flags 00 channels | groups(LE u32) | body_off(LE u32) | 0`,
  where `groups = floor(num_samples / 32)` and `body_off` points at the first
  SNS block (commonly `0x20`).
- **SNR header** @ `0x10` (BE-packed, 8 bytes):
  - `bits 28-31` version (0)
  - `bits 24-27` codec (4 = XAS1)
  - `bits 18-23` channel config (channels − 1)
  - `bits 0-17`  sample rate
  - `bits 30-31` stream type (0 = RAM, 1 = stream)
  - `bit 29`     loop flag
  - `bits 0-28`  sample count
- **SNS blocks** at `body_off`: 24-bit block size + 1-byte id (`0x00` normal,
  `0x80` last) + 4-byte sample count, followed by frames of `channels × 0x4c`
  bytes. Each `0x4c` frame encodes 128 samples as 4 groups of 32 samples (2 raw
  12-bit headers + 30 4-bit residual nibbles), using the standard CD-XA filter
  pairs `{(0,0), (0.9375,0), (1.796875,-0.8125), (1.53125,-0.859375)}`.

## Status

| Codec | id | Decode | Encode |
|-------|----|--------|--------|
| XAS1 (EA-XAS v1)           | 4 | yes | yes |
| EALAYER3 v2 PCM            | 6 | yes | no  |
| EALAYER3 v2 Spike          | 7 | yes | no  |
| Others (EAXMA, GCADPCM, …) |   | no  | no  |

EALayer3 decoding uses a vendored [minimp3](https://github.com/lieff/minimp3)
for the underlying MPEG-1 Layer III work; the EA-frame reframing logic is
derived from vgmstream's `mpeg_custom_utils_ealayer3.c`. Multichannel
(> 2 channels) is handled for XAS1 only — EALayer3 content is always 1–2
channels in practice.

Seek tables (TOB1 / the byte[1]=0x0C variant) are not written; the encoder
emits a single-buffer SNU that the runtime will still parse, but it won't
be byte-identical to shipped streamed files that carry a precomputed table.

## Credits

- [vgmstream](https://github.com/vgmstream/vgmstream) — reference for the
  SNU/EAAC/XAS1 format and the EALayer3 reframing logic
- [minimp3](https://github.com/lieff/minimp3) (Lion Yang, CC0) — vendored MP3
  decoder used by the EALayer3 path; see `third_party/minimp3.h`
- EALayer3 format originally reverse-engineered by Zench
  (<https://bitbucket.org/Zenchreal/ealayer3>)
