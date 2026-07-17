// RomSpriteExtract.h — Extract Pokemon sprites from Crystal/Gold/Silver ROM data.
// Reads compressed 2bpp sprite data via gnuboy's rom.bank[], decompresses with
// the Gen 2 LZ3 algorithm, and converts to pixels for the battle station.
#pragma once
#include <cstdint>
#include <cstddef>
#include "pax_gfx.h"

// Decompress and render a Pokemon sprite from the loaded Crystal/Gold/Silver ROM.
// `species` is the Pokedex number (1-251). `isBack` selects front or back sprite.
// Draws at (dx, dy) with the given scale factor (1 or 2).
// `bgARGB` is the ARGB8888 background color for transparent pixels (palette index 0).
// Returns true if the sprite was successfully extracted and rendered.
bool renderRomSprite(pax_buf_t *fb, int dx, int dy,
                     uint8_t species, bool isBack,
                     uint32_t bgARGB, int scale);
