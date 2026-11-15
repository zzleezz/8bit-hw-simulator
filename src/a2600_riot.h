#ifndef A2600_RIOT_H
#define A2600_RIOT_H

#include "parts.h"

#define A2600_RIOT_RAM_ADDR_BITS 7
#define A2600_RIOT_RAM_SIZE (1u << A2600_RIOT_RAM_ADDR_BITS)

extern const part_type_t a2600_riot_type;

#endif
