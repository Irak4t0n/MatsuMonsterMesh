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

#ifdef __cplusplus
}
#endif
