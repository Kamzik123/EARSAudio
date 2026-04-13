# EARSAudio

Decoder and encoder for EA `.exa.snu` audio (EAAudioCore / RwAudioCore "SnP1"),
as used by EA Redwood Shores / Visceral titles — Dead Space 1, The Godfather 2,
etc. Currently supports the XAS1 codec (EA-XAS v1, codec id 4).

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
ears_cli encode <input.wav>     <output.exa.snu>
```

Examples:

```sh
ears_cli info   sample.exa.snu
ears_cli decode sample.exa.snu sample.wav
ears_cli encode voice.wav      voice.exa.snu
```

`encode` accepts 16-bit PCM WAV, mono or stereo, any sample rate ≤ 262143 Hz.

## Library API

```c
#include <ears.h>

ears_info info;
ears_probe_file("in.exa.snu", &info);

/* Decode to interleaved int16 PCM */
int16_t *pcm; size_t samples;
ears_decode_file_to_wav("in.exa.snu", "out.wav");

/* Encode int16 PCM to SNU */
ears_encode_wav_to_file("in.wav", "out.exa.snu");

/* In-memory variants also available */
ears_decode_memory(buf, size, &info, &pcm, &samples);
ears_encode_memory(pcm, samples, channels, rate, &snu_buf, &snu_size);
ears_free(pcm);
```

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
| XAS1 (EA-XAS v1)     | 4 | yes | yes |
| EALAYER3 v2 (Spike)  | 7 | no  | no  |
| Others (EAXMA, GCADPCM, …) | | no | no |

Dead Space 2 multiplayer samples use EALAYER3 v2 Spike and aren't handled yet.

## Credits

- vgmstream — reference for the SNU/EAAC/XAS1 format
  (<https://github.com/vgmstream/vgmstream>)
