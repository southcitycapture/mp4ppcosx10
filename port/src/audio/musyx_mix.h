/* The CPU mixer that stands in for the GameCube's dspSlave ucode.
 * Implemented in musyx_mix.c, called from musyx_sal.c's salCtrlDsp.
 */
#ifndef PORT_MUSYX_MIX_H
#define PORT_MUSYX_MIX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Called once, from salStartAi, after salInitDspCtrl has allocated the
 * dspVoice array and the studio accumulation buffers. */
void port_musyx_mix_init(void);

/* Render exactly one DSP frame: 160 frames of interleaved stereo signed
 * 16-bit at 32 kHz (640 bytes) into `dest`, and update the host-visible voice
 * state (currentAddr above all) that MusyX reads back. */
void port_musyx_mix_frame(short* dest);

void port_musyx_mix_shutdown(void);
void port_musyx_mix_report(void);

/* Set by --mute: keep every bit of state and timing, emit silence. */
extern int port_musyx_mix_mute;

#ifdef __cplusplus
}
#endif

#endif
