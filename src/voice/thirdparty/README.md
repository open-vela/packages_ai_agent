# thirdparty/

## minimp3.h — MP3 decoder (vendored)

* Upstream: https://github.com/lieff/minimp3
* Fetched from: https://cdn.jsdelivr.net/gh/lieff/minimp3@master/minimp3.h
  (master, 2026-10-11; md5 \`c3eb1a828607e8f9f0b6f3ae35f4100b\`, 76,831 bytes)
* License: CC0 1.0 Universal (public domain dedication) — see
  \`minimp3.LICENSE\` (the upstream LICENSE file, verbatim).
* The header is **unmodified**.

It is compiled in exactly one translation unit, \`../edge_mp3.c\`:

\`\`\`c
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3 /* the Edge endpoint only sends Layer III */
#define MINIMP3_NO_SIMD  /* Cortex-M33: portable C path */
#include "thirdparty/minimp3.h"
\`\`\`

Why \`edge_mp3.c\` carries its own copy of \`mp3dec_decode_frame()\`:
upstream keeps a \`mp3dec_scratch_t\` on the stack, which is **16,256 bytes**
with this toolchain, while the voice path runs on 16 KB threads (outbound
channel, LVGL UI channel, NSH console for \`voice_test_tts\`).  The copy in
\`edge_mp3.c\` is upstream's function body with that buffer moved into the
heap state allocated by \`edge_mp3_open()\`; the two were verified
sample-exact against each other on the host.  If the vendored header is ever
updated, re-check that copy.

Memory per open decoder (all heap): mp3dec_t 6,668 B + scratch 16,256 B +
4 KB input buffer + 4,608 B PCM frame buffer ≈ 31.5 KB.
