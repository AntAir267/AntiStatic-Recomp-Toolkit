/* A replacement font for the game's letters (1NSANE's FONT option): the port's font_atlas gives
   the atlas (a size x size alpha image) and its generation, which changes when it is rebuilt; the
   weak default (vk.c) has none, so the game's own letters are drawn (LEGO Island 2). */
#pragma once
#include <stdint.h>

const uint8_t *font_atlas(int *size, uint32_t *gen);
