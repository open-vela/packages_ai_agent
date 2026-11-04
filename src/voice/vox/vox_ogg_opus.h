/*
 * vox_ogg_opus.h -- Ogg page framing, stripped to Opus packets.
 *
 * WHY THIS FILE IS NEEDED
 *
 * The backend does not send bare Opus.  It sends an Ogg stream carrying Opus,
 * and the decoder must reassemble pages into packets before it can decode
 * anything.  Getting this wrong looks exactly like a broken codec: no error,
 * no sound.
 *
 * This is the one piece of the xiaozhi protocol that cannot be guessed.  It was
 * taken from the reference implementation:
 *
 *   78/xiaozhi-esp32, main/audio/demuxer/ogg_demuxer.{h,cc}
 *
 * whose comment states the parameters plainly (audio_service.h:40):
 *
 *   #define OPUS_FRAME_DURATION_MS 60
 *
 * and the authoritative document says the same:
 *
 *   采样率 16000 Hz / 通道数 1 / 帧长 60 ms / OPUS / Protocol-Version 1
 *
 * WHAT THE SERVER ACTUALLY SENDS
 *
 * An Ogg page begins "OggS", version 0, and its segment count sits at byte 26.
 * Each packet is a run of segment lengths; a length below 255 ends the packet,
 * 255 means "continues into the next segment".  The header packet is OpusHead,
 * which carries the real sample rate in its 12th byte -- and the reference
 * deliberately decodes at 48000 regardless, because Opus always operates
 * internally at 48 kHz and the input rate is informational:
 *
 *   "The OpusHead input rate is informational. Decode at a fixed 48k"
 *
 * Two things that are easy to get wrong and are handled explicitly here:
 *
 *   - a packet that arrives before OpusHead must be dropped, not decoded;
 *   - the reference drops a packet whose accumulated length exceeds a limit, so
 *     a corrupt length table cannot make the decoder wait forever.
 *
 * This file does NO Opus decoding.  It only produces packets, which the codec
 * layer then decodes.  Keeping the two apart means the framing can be tested
 * against hand-built pages without a codec library, and it keeps the codec
 * swappable -- the board currently has FFmpeg's libopus, which is a large piece
 * of code that has not been built for Xtensa yet.
 *
 * Buffer ownership: a decoded packet points into the decoder's own storage and
 * is valid until the next vox_ogg_feed() call.
 */

#ifndef VOX_OGG_OPUS_H
#define VOX_OGG_OPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The protocol's fixed parameters, from audio_service.h:40 and the protocol
 * document.  Stated here so a mismatch with the backend is visible rather than
 * implied. */
#define VOX_OPUS_SAMPLE_RATE 16000
#define VOX_OPUS_CHANNELS    1
#define VOX_OPUS_FRAME_MS    60

/* Opus decodes at 48 kHz internally; the protocol's 16 kHz is the wire rate of
 * the original capture.  The reference decoder hard-codes 48000 (see the
 * comment in ogg_demuxer.cc:317). */
#define VOX_OPUS_DECODE_RATE 48000

/* One 60 ms frame at 48 kHz, mono, 16-bit. */
#define VOX_OPUS_SAMPLES_PER_FRAME \
  ((VOX_OPUS_DECODE_RATE / 1000) * VOX_OPUS_FRAME_MS)

/* A single Opus packet is small; anything larger is a corrupt length table. */
#ifndef VOX_OPUS_MAX_PACKET
#define VOX_OPUS_MAX_PACKET 4096
#endif

/* Staged receive buffer.  A WebSocket frame can carry up to VOX_WS_MAX_MESSAGE,
 * and a 60 ms Opus packet is far smaller, so this holds several frames worth of
 * reassembly.  Two frames' worth would be the floor; the extra headroom costs
 * 8 KiB of static memory on a board whose kernel heap has been measured with as
 * little as 119 KiB free. */
#ifndef VOX_OGG_BUF
#define VOX_OGG_BUF 16384
#endif

typedef struct
{
  /* "OggS" + version + flags + granule + serial + seq + crc + seg_count */
  uint8_t header[27];
  int     data_offset;   /* bytes of header read so far */
  int     bytes_needed;  /* bytes of header still expected */

  uint8_t seg_table[255];
  int     seg_count;
  int     seg_index;

  /* A packet being assembled from segments. */
  uint8_t packet[VOX_OPUS_MAX_PACKET];
  int     packet_len;

  bool     have_head;    /* OpusHead has been seen */
  bool     saw_tags;     /* OpusTags has been seen */
  bool     error;        /* a protocol violation was seen */
  uint8_t  channels;     /* from OpusHead */
  uint32_t input_rate;   /* from OpusHead, informational */
} vox_ogg_t;

void vox_ogg_init(vox_ogg_t *d);

/* Stages received bytes.  Any chunk size works, including one byte at a time;
 * a page may span any number of feeds.
 *
 * Feeding does NOT parse.  Parsing happens in vox_ogg_next_packet(), one packet
 * per call, which is what lets a decoded packet survive until the caller has
 * used it: an earlier version parsed inside feed() and lost every packet except
 * the last one in a buffer. */
void vox_ogg_feed(vox_ogg_t *d, const uint8_t *data, size_t len);

/* Returns true and fills pkt/pkt_len when a complete Opus packet is available.
 * Call repeatedly until it returns false; each call yields at most one packet,
 * and the pointer stays valid until the call after the one that returned it.
 *
 * Packets seen before OpusHead are consumed and dropped rather than returned,
 * which is what the reference does -- handing them to a decoder produces noise.
 *
 * Returns false on "no packet right now", which is not an error: a caller loop
 * is simply
 *
 *     while (vox_ogg_next_packet(&ogg, &pkt, &len)) { use(pkt, len); }
 *
 * after each feed. */
bool vox_ogg_next_packet(vox_ogg_t *d, const uint8_t **pkt, size_t *pkt_len);

/* True once OpusHead has been seen and no error has occurred. */
bool vox_ogg_ready(const vox_ogg_t *d);

/* Diagnostics. */
uint32_t vox_ogg_packets(const vox_ogg_t *d);
uint32_t vox_ogg_pages(const vox_ogg_t *d);
uint32_t vox_ogg_dropped(const vox_ogg_t *d);

#ifdef __cplusplus
}
#endif

#endif /* VOX_OGG_OPUS_H */