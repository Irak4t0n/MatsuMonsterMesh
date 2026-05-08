// Compatibility shim for code ported from MonsterMesh (Arduino/PlatformIO
// project) into this ESP-IDF component. Provides drop-in replacements for the
// few Arduino APIs the ported code uses, mapped to the ESP-IDF equivalents
// per the Step 3 substitution rules:
//
//   millis()   -> esp_timer_get_time() / 1000
//
// Other Arduino APIs (Serial, String, delay, F(), etc.) are intentionally
// not provided — the included monster_core files don't use them. If a future
// port pulls in code that does, replace those calls explicitly rather than
// growing this header.

#pragma once

#include <stdint.h>
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t mm_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#ifdef __cplusplus
}
#endif
