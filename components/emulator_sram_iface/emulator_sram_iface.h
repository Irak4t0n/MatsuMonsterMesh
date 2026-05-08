#pragma once

// Generic interface to a Game Boy / cartridge battery-backed RAM ("SRAM"),
// abstracted so non-gnuboy emulators can plug in later without touching
// monster_core or the daycare/save patcher code.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IEmulatorSRAM IEmulatorSRAM;

struct IEmulatorSRAM {
    void* ctx;

    size_t   (*size)(void* ctx);
    uint8_t  (*read)(void* ctx, size_t offset);
    void     (*write)(void* ctx, size_t offset, uint8_t value);

    // Optional flat-buffer accessor. Returns NULL for emulator backends that
    // do not expose a contiguous buffer (e.g. paged / banked-on-demand RAM).
    // Callers that need to scan SRAM in bulk should fall back to read/write
    // when this returns NULL.
    uint8_t* (*data)(void* ctx);

    bool     (*is_dirty)(void* ctx);
    void     (*mark_dirty)(void* ctx);
    void     (*clear_dirty)(void* ctx);
    bool     (*is_battery_backed)(void* ctx);
};

static inline size_t   iemu_sram_size(const IEmulatorSRAM* s)                       { return s->size(s->ctx); }
static inline uint8_t  iemu_sram_read(const IEmulatorSRAM* s, size_t o)             { return s->read(s->ctx, o); }
static inline void     iemu_sram_write(const IEmulatorSRAM* s, size_t o, uint8_t v) { s->write(s->ctx, o, v); }
static inline uint8_t* iemu_sram_data(const IEmulatorSRAM* s)                       { return s->data ? s->data(s->ctx) : NULL; }
static inline bool     iemu_sram_is_dirty(const IEmulatorSRAM* s)                   { return s->is_dirty(s->ctx); }
static inline void     iemu_sram_mark_dirty(const IEmulatorSRAM* s)                 { s->mark_dirty(s->ctx); }
static inline void     iemu_sram_clear_dirty(const IEmulatorSRAM* s)                { s->clear_dirty(s->ctx); }
static inline bool     iemu_sram_is_battery_backed(const IEmulatorSRAM* s)          { return s->is_battery_backed(s->ctx); }

#ifdef __cplusplus
}
#endif
