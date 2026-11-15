/* PAL 2600A digital signals, with schematic bench coordinates in mm.
 * The clock, reset and pull-ups are circuit equivalents. See docs/atari2600-pal.md. */
#include "board_data.h"

enum {
    GND, VCC, A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12,
    D0, D1, D2, D3, D4, D5, D6, D7, RW, RES, RDY, OSC, PHI0, PHI2,
    SYNC, LUM0, LUM1, LUM2, COL, PALI, PALS, CADJ, AUD,
    IN0, IN1, IN2, IN3, IN4, IN5,
    P0U, P0D, P0L, P0R, P1U, P1D, P1L, P1R, RESET, SELECT, COLOR, DIFF0, DIFF1,
    NC = -1
};

static const char *const atari_nets[] = {
    "GND", "+5V", "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9", "A10", "A11", "A12",
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "R/W", "/RES", "RDY", "OSC", "PHI0", "PHI2",
    "CSYNC", "LUM0", "LUM1", "LUM2", "COL", "PALI", "PALS", "CADJ", "AUD",
    "INPT0", "INPT1", "INPT2", "INPT3", "INPT4", "INPT5",
    "P0_UP", "P0_DOWN", "P0_LEFT", "P0_RIGHT", "P1_UP", "P1_DOWN", "P1_LEFT", "P1_RIGHT",
    "GAME_RESET", "GAME_SELECT", "COLOR", "DIFF0", "DIFF1"
};

#define APAD(n, x, y, net) { #n, x, y, 1.8f, 1.8f, net }
#define ADIP_L(n, x, y, net) APAD(n, x, (y) + ((n)-1)*2.54f, net)
#define ADIP_R(n, rows, x, y, net) APAD(n, (x) + 15.24f, (y) + (2*(rows)-(n))*2.54f, net)
#define ACPU_L(n, net) ADIP_L(n, 59, 36, net)
#define ACPU_R(n, net) ADIP_R(n, 14, 59, 36, net)
#define ARIOT_L(n, net) ADIP_L(n, 27, 36, net)
#define ARIOT_R(n, net) ADIP_R(n, 20, 27, 36, net)
#define ATIA_L(n, net) ADIP_L(n, 91, 36, net)
#define ATIA_R(n, net) ADIP_R(n, 20, 91, 36, net)
#define ACART(n, i, y, net) APAD(n, 42 + (i)*2.54f, y, net)
#define AJOY(n, i, y, x, net) APAD(n, (x) + (i)*2.77f, y, net)

static const bd_pad_t atari_pads[] = {
    ACPU_L(1, RES), ACPU_L(2, GND), ACPU_L(3, RDY), ACPU_L(4, VCC),
    ACPU_L(5, A0), ACPU_L(6, A1), ACPU_L(7, A2), ACPU_L(8, A3), ACPU_L(9, A4),
    ACPU_L(10, A5), ACPU_L(11, A6), ACPU_L(12, A7), ACPU_L(13, A8), ACPU_L(14, A9),
    ACPU_R(15, A10), ACPU_R(16, A11), ACPU_R(17, A12), ACPU_R(18, D7), ACPU_R(19, D6),
    ACPU_R(20, D5), ACPU_R(21, D4), ACPU_R(22, D3), ACPU_R(23, D2), ACPU_R(24, D1),
    ACPU_R(25, D0), ACPU_R(26, RW), ACPU_R(27, PHI0), ACPU_R(28, PHI2),

    ARIOT_L(1, GND), ARIOT_L(2, A5), ARIOT_L(3, A4), ARIOT_L(4, A3), ARIOT_L(5, A2),
    ARIOT_L(6, A1), ARIOT_L(7, A0), ARIOT_L(8, P1U), ARIOT_L(9, P1D), ARIOT_L(10, P1L),
    ARIOT_L(11, P1R), ARIOT_L(12, P0U), ARIOT_L(13, P0D), ARIOT_L(14, P0L), ARIOT_L(15, P0R),
    ARIOT_L(16, DIFF1), ARIOT_L(17, DIFF0), ARIOT_L(18, NC), ARIOT_L(19, NC), ARIOT_L(20, VCC),
    ARIOT_R(21, COLOR), ARIOT_R(22, NC), ARIOT_R(23, SELECT), ARIOT_R(24, RESET), ARIOT_R(25, NC),
    ARIOT_R(26, D7), ARIOT_R(27, D6), ARIOT_R(28, D5), ARIOT_R(29, D4), ARIOT_R(30, D3),
    ARIOT_R(31, D2), ARIOT_R(32, D1), ARIOT_R(33, D0), ARIOT_R(34, RES), ARIOT_R(35, RW),
    ARIOT_R(36, A9), ARIOT_R(37, A12), ARIOT_R(38, A7), ARIOT_R(39, PHI2), ARIOT_R(40, A6),

    ATIA_L(1, GND), ATIA_L(2, SYNC), ATIA_L(3, RDY), ATIA_L(4, PHI0), ATIA_L(5, LUM1),
    ATIA_L(6, LUM2), ATIA_L(7, LUM0), ATIA_L(8, PALI), ATIA_L(9, COL), ATIA_L(10, CADJ),
    ATIA_L(11, OSC), ATIA_L(12, PALS), ATIA_L(13, AUD), ATIA_L(14, D0), ATIA_L(15, D1),
    ATIA_L(16, D2), ATIA_L(17, D3), ATIA_L(18, D4), ATIA_L(19, D5), ATIA_L(20, VCC),
    ATIA_R(21, A7), ATIA_R(22, GND), ATIA_R(23, VCC), ATIA_R(24, A12), ATIA_R(25, RW),
    ATIA_R(26, PHI2), ATIA_R(27, A5), ATIA_R(28, A4), ATIA_R(29, A3), ATIA_R(30, A2),
    ATIA_R(31, A1), ATIA_R(32, A0), ATIA_R(33, D6), ATIA_R(34, D7), ATIA_R(35, IN5),
    ATIA_R(36, IN4), ATIA_R(37, IN3), ATIA_R(38, IN2), ATIA_R(39, IN1), ATIA_R(40, IN0),

    ACART(1, 0, 21, A7), ACART(2, 1, 21, A6), ACART(3, 2, 21, A5), ACART(4, 3, 21, A4),
    ACART(5, 4, 21, A3), ACART(6, 5, 21, A2), ACART(7, 6, 21, A1), ACART(8, 7, 21, A0),
    ACART(9, 8, 21, D0), ACART(10, 9, 21, D1), ACART(11, 10, 21, D2), ACART(12, 11, 21, GND),
    ACART(13, 0, 16, D3), ACART(14, 1, 16, D4), ACART(15, 2, 16, D5), ACART(16, 3, 16, D6),
    ACART(17, 4, 16, D7), ACART(18, 5, 16, A12), ACART(19, 6, 16, A10), ACART(20, 7, 16, A11),
    ACART(21, 8, 16, A9), ACART(22, 9, 16, A8), ACART(23, 10, 16, VCC), ACART(24, 11, 16, GND),

    AJOY(1, 0, 16, 11, P0U), AJOY(2, 1, 16, 11, P0D), AJOY(3, 2, 16, 11, P0L),
    AJOY(4, 3, 16, 11, P0R), AJOY(5, 4, 16, 11, IN1), AJOY(6, 0.5f, 21, 11, IN4),
    AJOY(7, 1.5f, 21, 11, VCC), AJOY(8, 2.5f, 21, 11, GND), AJOY(9, 3.5f, 21, 11, IN0),
    AJOY(1, 0, 16, 96, P1U), AJOY(2, 1, 16, 96, P1D), AJOY(3, 2, 16, 96, P1L),
    AJOY(4, 3, 16, 96, P1R), AJOY(5, 4, 16, 96, IN3), AJOY(6, 0.5f, 21, 96, IN5),
    AJOY(7, 1.5f, 21, 96, VCC), AJOY(8, 2.5f, 21, 96, GND), AJOY(9, 3.5f, 21, 96, IN2),

    APAD(1, 12, 40, RESET), APAD(2, 12, 46, GND),
    APAD(1, 12, 58, SELECT), APAD(2, 12, 64, GND),

    APAD(1, 58, 88, GND), APAD(2, 58, 94, VCC), APAD(3, 66, 88, OSC),
    APAD(1, 58, 75, GND), APAD(2, 58, 81, VCC), APAD(3, 66, 75, RES),

    APAD(1, 117, 39, VCC), APAD(2, 120, 39, RDY), APAD(3, 120, 44, SYNC),
    APAD(4, 120, 49, LUM0), APAD(5, 120, 54, LUM1), APAD(6, 120, 59, LUM2),
    APAD(7, 120, 64, COL), APAD(8, 120, 69, IN4), APAD(9, 120, 74, IN5)
};

static const bd_comp_t atari_components[] = {
    { "A200", "MOS 6507", "DIP-28", 0, 28 },
    { "A202", "MOS 6532", "DIP-40", 28, 40 },
    { "A201", "PAL TIA", "DIP-40", 68, 40 },
    { "CART", "Cartridge", "Edge_24", 108, 24 },
    { "JOY0", "Left controller", "DE9", 132, 9 },
    { "JOY1", "Right controller", "DE9", 141, 9 },
    { "SW_RESET", "Game reset", "Switch", 150, 2 },
    { "SW_SELECT", "Game select", "Switch", 152, 2 },
    { "CLOCK", "PAL oscillator", "Oscillator", 154, 3 },
    { "POR", "Power-on reset", "Reset", 157, 3 },
    { "RP", "Digital pull-ups (bussed)", "R_Array_SIP", 160, 9 }
};

const board_data_t bd_a2600pal = {
    { 4, 5, 128, 100 },
    atari_nets, sizeof(atari_nets) / sizeof(*atari_nets),
    atari_pads, sizeof(atari_pads) / sizeof(*atari_pads),
    atari_components, sizeof(atari_components) / sizeof(*atari_components)
};
