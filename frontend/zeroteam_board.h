#ifndef ZEROTEAM_BOARD_H
#define ZEROTEAM_BOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero Team runs on a 320x256 raster that the arcade cabinet displayed as a
 * 4:3 picture. The port keeps that geometry: 320 columns, 256 lines, and the
 * 4:3 aspect is restored by the frontend when it scales to the panel. */
enum {
    ZT_WIDTH = 320,
    ZT_HEIGHT = 256,
    ZT_AUDIO_RATE = 48000,
    ZT_MAX_AUDIO_FRAMES = 2048
};

enum {
    ZT_UP = 1, ZT_DOWN = 2, ZT_LEFT = 4, ZT_RIGHT = 8,
    ZT_ATTACK = 16, ZT_JUMP = 32, ZT_START = 64, ZT_COIN = 128
};

/* The board consumes extracted, CRC-verified chips from `directory`. */
int zt_init(const char *directory);
double zt_fps(void);
/* XRGB8888, top to bottom, and interleaved 48 kHz signed stereo.
 * Return the stereo frame count, or a negative value on a board error. */
int zt_frame(uint32_t *pixels, int16_t *stereo, unsigned p1, unsigned p2);
void zt_shutdown(void);

/* Zero size means this build has no verified save-state implementation.
 * Save and load return zero on success; a rejected load leaves the board intact. */
size_t zt_state_size(void);
int zt_save_state(void *out, size_t size);
int zt_load_state(const void *data, size_t size);

/* Diagnostics used by the build's self-test, never by gameplay. */
unsigned zt_aot_sites(void);
unsigned zt_aot_fallback(void);

#ifdef __cplusplus
}
#endif
#endif
