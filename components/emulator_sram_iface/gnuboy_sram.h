#pragma once

#include "emulator_sram_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise an IEmulatorSRAM that points at gnuboy's live cart battery RAM.
// Must be called AFTER gnuboy has loaded a ROM (so `ram.sbank` has been
// allocated by loader.c). gnuboy is single-instance, so the resulting
// interface is a singleton view of the global emulator state.
void gnuboy_sram_init(IEmulatorSRAM* out);

// Return a pointer to gnuboy's WRAM bank 1 (Game Boy address 0xD000-0xDFFF).
// This is the emulator's LIVE working RAM — party data here is always
// up-to-date, unlike SRAM which only refreshes when the player saves.
// Returns NULL if gnuboy hasn't been initialised yet.
uint8_t* gnuboy_wram_bank1(void);

// Read a single byte from a Game Boy address (0x0000-0xFFFF) through
// gnuboy's memory mapper.  Goes through the rmap fast-path, so it
// respects current ROM / RAM / WRAM bank selection.
// MUST only be called while the emulator is paused (e.g. terminal active).
uint8_t gnuboy_mem_read_byte(uint16_t gb_addr);

// Return a pointer to gnuboy's WRAM bank N (0-7).
// Bank 0 = 0xC000-0xCFFF, banks 1-7 = switchable 0xD000-0xDFFF.
uint8_t* gnuboy_wram_bank(int n);

// Return the ROM title string (up to 16 chars, from the cart header).
const char* gnuboy_rom_name(void);

#ifdef __cplusplus
}
#endif
