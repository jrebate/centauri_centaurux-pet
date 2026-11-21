#pragma once
#include <stdint.h>
#define SPR_W 49
#define SPR_H 38
enum { ST_SLEEPING, ST_THINKING, ST_WORKING, ST_WAITING, ST_HAPPY, ST_WORRIED };
extern const uint16_t SPR_PALETTE[32];
extern const uint8_t SPRITES[6][SPR_W * SPR_H];
