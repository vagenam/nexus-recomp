/*
 * ds_player -- the DualSense speaker's own sound player (see ds_player.c).
 *
 * The game side picks what the controller plays and hands whole sounds over;
 * cellAudio.c pulls the mixed result once per audio block.
 */
#ifndef PS3RECOMP_DS_PLAYER_H
#define PS3RECOMP_DS_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sounds are taken only while the player is on (cellAudio: a controller
 * audio device is open, or RACACIT_DS_RAW captures for a test). */
void ds_player_enable(int on);
int  ds_player_enabled(void);

/* A SCREAM (989snd SFX block) bank sound, run from its grain script.
 * `owner` is the game's sound instance -- ds_player_stop(owner) ends it;
 * `spk` / `felt`: its level on the speaker / the actuators (0 = not there). */
void ds_player_scream(uint32_t bank_ea, uint32_t sound, float spk, float felt,
                      uint32_t owner, const char* name);

/* Mono PS-ADPCM (16-byte frames; copied) at `rate` Hz: a dialogue line or a
 * streamed sound, starting `delay_ms` from now. */
void ds_player_adpcm(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, float felt,
                     uint32_t owner, uint32_t delay_ms, const char* name);

void ds_player_stop(uint32_t owner);   /* the game stopped that sound: fade it out */

/* A dialogue line the controller plays instead of the TV; returns its token. The game keeps
 * streaming it (the reads are zeroed), so ds_player_line_read(token, bytes) says how far it
 * has got, and the copy never runs more than 2 s past that: if the game pauses the line or
 * drops it by a path no hook sees, the copy holds, and after 30 s it ends. */
uint32_t ds_player_line(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, uint32_t delay_ms, const char* name);
void ds_player_line_read(uint32_t token, uint32_t bytes);
void ds_player_stop_lines(int all);    /* the game ended speech; all = 0: not Mr. Zurkon's or the Zoni's */
void ds_player_stream_pause(uint32_t stream, int on);   /* SCREAM's stream pause callback */

/* `owner`'s sound is the game's SCREAM sound `handle` (known once the game's
 * start returned); `snd_globals`: SCREAM's globals. When that sound is gone
 * -- stopped by a path no hook sees, or over -- the controller's copy ends. */
void ds_player_bind(uint32_t owner, uint32_t handle, uint32_t snd_globals);
void ds_player_stop_all(void);

/* The next `n` samples (48 kHz mono), added into speaker[] and felt[]. */
void ds_player_render(float* speaker, float* felt, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
