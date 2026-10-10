/*
 * Audio-over-udp_link stream format: the contract between the sender
 * (i2s_mic wifi_audio_tx) and any receiver (the i2s_spk example, or
 * tools/udp_audio_rx.py on a PC).
 *
 * Keep an identical copy on both sides. When the examples become full
 * duplex, both directions use this same format.
 *
 * One udp_link datagram carries one audio frame:
 *   - payload = AUDIO_FRAME_SAMPLES signed 16-bit mono PCM samples,
 *     little-endian, no extra header (udp_link already adds its own header with
 *     a sequence number and session id);
 *   - AUDIO_FRAME_SAMPLES at AUDIO_SAMPLE_RATE = AUDIO_FRAME_MS of audio.
 *
 * The udp_link stream_id doubles as a format id: a receiver configured for a
 * different format rejects these datagrams instead of playing noise.
 */
#pragma once

#include <stdint.h>

#define AUDIO_SAMPLE_RATE       16000
#define AUDIO_BITS_PER_SAMPLE   16
#define AUDIO_CHANNELS          1
#define AUDIO_FRAME_MS          30
#define AUDIO_FRAME_SAMPLES     (AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS / 1000)          /* 480 */
#define AUDIO_FRAME_BYTES       (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8))  /* 960 */

/* udp_link stream_id for "16 kHz, 16-bit, mono, 30 ms frames". Change it if the
 * format above changes, so old and new builds don't talk to each other. */
#define AUDIO_STREAM_ID         0xA1

_Static_assert(AUDIO_FRAME_SAMPLES * 1000 == AUDIO_SAMPLE_RATE * AUDIO_FRAME_MS,
               "frame must be a whole number of samples");
