/* PAL Atari 2600 Television Interface Adapter (C011903). */
#ifndef A2600_TIA_H
#define A2600_TIA_H

#include "board.h"

#define A2600_CLOCKS_PER_LINE 228
#define A2600_SCREEN_W 160
#define A2600_SCREEN_H 312

extern const part_type_t a2600_tia_type;

/* Call after settling OSC, once per oscillator half-period. PHI0 is
 * derived from the level changes observed on the TIA's OSC pin. */
void a2600_tia_clock(sim_t *s, part_t *p);
/* Call after settling the outputs and committing the CPU bus edge. */
void a2600_tia_video(sim_t *s, part_t *p);
void a2600_tia_monitor(board_t *b, part_t *p, uint32_t *out);

#endif
