// Adapter: exposes gnuboy's cartridge SRAM through the IEmulatorSRAM interface.
//
// gnuboy stores cart battery RAM in the global `struct ram ram;` (declared in
// components/gnuboy/mem.h, defined in mem.c). The relevant field is
// `ram.sbank` — a single flat malloc'd buffer allocated by sram_load() in
// loader.c with size `mbc.ramsize * 8192`. We point at this buffer directly
// so reads/writes hit the live emulator state with zero copy. Other relevant
// fields: `ram.sram_dirty` (gnuboy's own dirty flag, also set by gnuboy on
// emulator-side writes) and `mbc.batt` (cart has battery-backed RAM).

#include "gnuboy_sram.h"
#include "mem.h"

// ROMs without battery RAM (e.g. Tetris) result in mbc.ramsize == 0 and
// ram.sbank == NULL — the loader simply doesn't allocate. The IEmulatorSRAM
// contract is "size()==0 + data()==NULL means no SRAM available", so we
// check both conditions on every accessor and degrade gracefully instead
// of dereferencing a null pointer.

static inline bool gb_sram_present(void) {
    return ram.sbank != NULL && mbc.ramsize > 0;
}

static size_t   gb_sram_size(void* c)                          { (void)c; return gb_sram_present() ? (size_t)mbc.ramsize * 8192u : 0u; }
static uint8_t  gb_sram_read(void* c, size_t o)                { (void)c; if (!gb_sram_present()) return 0; return ((uint8_t*)ram.sbank)[o]; }
static void     gb_sram_write(void* c, size_t o, uint8_t v)    { (void)c; if (!gb_sram_present()) return; ((uint8_t*)ram.sbank)[o] = v; ram.sram_dirty = 1; }
static uint8_t* gb_sram_data(void* c)                          { (void)c; return gb_sram_present() ? (uint8_t*)ram.sbank : NULL; }
static bool     gb_sram_is_dirty(void* c)                      { (void)c; return gb_sram_present() && ram.sram_dirty != 0; }
static void     gb_sram_mark_dirty(void* c)                    { (void)c; if (gb_sram_present()) ram.sram_dirty = 1; }
static void     gb_sram_clear_dirty(void* c)                   { (void)c; if (gb_sram_present()) ram.sram_dirty = 0; }
static bool     gb_sram_is_battery_backed(void* c)             { (void)c; return gb_sram_present() && mbc.batt != 0; }

void gnuboy_sram_init(IEmulatorSRAM* out) {
    out->ctx               = NULL;  // gnuboy is a singleton; no per-instance state
    out->size              = gb_sram_size;
    out->read              = gb_sram_read;
    out->write             = gb_sram_write;
    out->data              = gb_sram_data;
    out->is_dirty          = gb_sram_is_dirty;
    out->mark_dirty        = gb_sram_mark_dirty;
    out->clear_dirty       = gb_sram_clear_dirty;
    out->is_battery_backed = gb_sram_is_battery_backed;
}
