// RomSpriteExtract.cpp — Crystal/Gold/Silver ROM sprite extraction.
// Implements the Gen 2 LZ3 decompressor and 2bpp tile renderer.

#include "RomSpriteExtract.h"
#include "pax_gfx.h"
#include "esp_log.h"
#include <cstring>

// gnuboy ROM access
extern "C" {
    extern struct rom {
        uint8_t (*bank)[16384];
        char name[20];
        int length;
    } rom;
}

static const char *TAG = "romsprite";

// ── Gen 2 LZ3 Decompressor ─────────────────────────────────────────────────
// Crystal/Gold/Silver use this LZ-variant compression for all sprites.
// Control byte: CCCNNNNN (3-bit command, 5-bit length).
// Terminator: 0xFF.

static size_t lz3Decompress(const uint8_t *src, size_t srcLen,
                             uint8_t *dst, size_t dstCap)
{
    size_t si = 0, di = 0;

    while (si < srcLen && di < dstCap) {
        uint8_t ctrl = src[si++];
        if (ctrl == 0xFF) break;  // terminator

        uint8_t cmd = (ctrl >> 5) & 0x07;
        uint16_t len;

        if (cmd == 7) {
            // LZ_LONG: next 3 bits = real command, 10-bit length
            if (si >= srcLen) break;
            cmd = (ctrl >> 2) & 0x07;
            len = ((uint16_t)(ctrl & 0x03) << 8) | src[si++];
            len += 1;
        } else {
            len = (ctrl & 0x1F) + 1;
        }

        switch (cmd) {
            case 0: // LZ_LITERAL: copy N bytes from source
                for (uint16_t i = 0; i < len && si < srcLen && di < dstCap; ++i)
                    dst[di++] = src[si++];
                break;

            case 1: // LZ_ITERATE: repeat 1 byte N times
                if (si >= srcLen) goto done;
                {
                    uint8_t val = src[si++];
                    for (uint16_t i = 0; i < len && di < dstCap; ++i)
                        dst[di++] = val;
                }
                break;

            case 2: // LZ_ALTERNATE: alternate 2 bytes for N bytes
                if (si + 1 >= srcLen) goto done;
                {
                    uint8_t a = src[si++], b = src[si++];
                    for (uint16_t i = 0; i < len && di < dstCap; ++i)
                        dst[di++] = (i & 1) ? b : a;
                }
                break;

            case 3: // LZ_ZERO: write 0x00 N times
                for (uint16_t i = 0; i < len && di < dstCap; ++i)
                    dst[di++] = 0;
                break;

            case 4: // LZ_REPEAT: copy N bytes from earlier output
            case 5: // LZ_FLIP: copy N bytes with bits reversed
            case 6: // LZ_REVERSE: copy N bytes in reverse order
            {
                if (si >= srcLen) goto done;
                uint8_t offByte = src[si++];
                size_t refPos;
                if (offByte & 0x80) {
                    // Negative offset: backward from current position
                    refPos = di - (offByte & 0x7F);
                } else {
                    // Positive offset: from start of output
                    if (si >= srcLen) goto done;
                    uint8_t offLo = src[si++];
                    refPos = ((size_t)offByte << 8) | offLo;
                }
                for (uint16_t i = 0; i < len && di < dstCap; ++i) {
                    size_t srcIdx;
                    if (cmd == 6) // REVERSE: read backwards
                        srcIdx = refPos - i;
                    else
                        srcIdx = refPos + i;
                    if (srcIdx >= di) { dst[di++] = 0; continue; }
                    uint8_t val = dst[srcIdx];
                    if (cmd == 5) {
                        // FLIP: reverse all bits
                        val = (uint8_t)(((val & 0x01) << 7) | ((val & 0x02) << 5) |
                                        ((val & 0x04) << 3) | ((val & 0x08) << 1) |
                                        ((val & 0x10) >> 1) | ((val & 0x20) >> 3) |
                                        ((val & 0x40) >> 5) | ((val & 0x80) >> 7));
                    }
                    dst[di++] = val;
                }
                break;
            }
            default:
                goto done;
        }
    }
done:
    return di;
}

// ── 2BPP tile → pixel conversion ────────────────────────────────────────────
// Crystal sprites use column-major tile order (tiles go down columns, then right).
// Each tile is 8x8 pixels, 16 bytes (2 bytes per row: lo plane + hi plane).

static void tiles2bppToPixels(const uint8_t *tiles, int tilesW, int tilesH,
                               uint32_t palette[4], uint32_t *pixels, int pixW)
{
    for (int tc = 0; tc < tilesW; ++tc) {
        for (int tr = 0; tr < tilesH; ++tr) {
            int tileIdx = tc * tilesH + tr;  // column-major
            const uint8_t *tile = &tiles[tileIdx * 16];
            int baseX = tc * 8;
            int baseY = tr * 8;
            for (int row = 0; row < 8; ++row) {
                uint8_t lo = tile[row * 2];
                uint8_t hi = tile[row * 2 + 1];
                for (int col = 0; col < 8; ++col) {
                    int bit = 7 - col;
                    int colorIdx = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
                    int px = baseX + col;
                    int py = baseY + row;
                    if (px < pixW && py < pixW)
                        pixels[py * pixW + px] = palette[colorIdx];
                }
            }
        }
    }
}

// ── PokemonPicPointers table ────────────────────────────────────────────────
// Crystal: bank $48, address $4000 (offset 0 within the bank).
// Each entry: 6 bytes [front_bank_enc, front_lo, front_hi, back_bank_enc, back_lo, back_hi]
// real_bank = encoded_byte + PICS_FIX (0x36)

static constexpr uint8_t  PIC_PTR_BANK  = 0x48;
static constexpr uint16_t PIC_PTR_ADDR  = 0x0000; // offset within bank $48
static constexpr uint8_t  PICS_FIX      = 0x36;

// ── Pokemon palette lookup ──────────────────────────────────────────────────
// PokemonPalettes in Crystal: 8 bytes per species (4 normal + 4 shiny).
// Each pair is 2 RGB555 colors (indices 1 and 2; 0=white, 3=black are fixed).
// We search the ROM for the table by matching entry 0's known values.

// Entry 0 (MissingNo): RGB(30,22,17) and RGB(16,14,19)
// RGB555: r | (g<<5) | (b<<10)
// Color 1: 30 + 22*32 + 17*1024 = 0x46DE  → bytes 0xDE, 0x46
// Color 2: 16 + 14*32 + 19*1024 = 0x4DD0  → bytes 0xD0, 0x4D
static const uint8_t kPalSearchPattern[8] = {
    0xDE, 0x46, 0xD0, 0x4D,  // normal
    0xDE, 0x46, 0xD0, 0x4D   // shiny (same for entry 0)
};

static int s_palBank = -1;
static int s_palOff  = -1;

static void findPaletteTable()
{
    if (s_palBank >= 0) return;  // already found
    int totalBanks = rom.length / 16384;
    // Search banks 0x00-0x3F (palette is in the lower banks)
    for (int b = 0; b < totalBanks && b < 0x40; ++b) {
        const uint8_t *bank = rom.bank[b];
        if (!bank) continue;
        for (int off = 0; off < 16384 - 8; ++off) {
            if (memcmp(&bank[off], kPalSearchPattern, 8) == 0) {
                s_palBank = b;
                s_palOff  = off;
                ESP_LOGI(TAG, "PokemonPalettes found at bank 0x%02X offset 0x%04X", b, off);
                return;
            }
        }
    }
    ESP_LOGW(TAG, "PokemonPalettes not found in ROM");
}

// Convert GBC RGB555 to ARGB8888
static inline uint32_t rgb555toARGB(uint16_t c) {
    uint8_t r5 = c & 0x1F;
    uint8_t g5 = (c >> 5) & 0x1F;
    uint8_t b5 = (c >> 10) & 0x1F;
    return 0xFF000000u | ((uint32_t)(r5 << 3 | r5 >> 2) << 16)
                       | ((uint32_t)(g5 << 3 | g5 >> 2) << 8)
                       | (uint32_t)(b5 << 3 | b5 >> 2);
}

static bool getSpeciesPalette(uint8_t species, uint32_t outPal[4], uint32_t bgARGB)
{
    findPaletteTable();
    outPal[0] = bgARGB;             // index 0 = background/white
    outPal[3] = 0xFF081820;         // index 3 = near-black

    if (s_palBank < 0 || species == 0 || species > 251) {
        // Fallback: GBC green
        outPal[1] = 0xFF88C070;
        outPal[2] = 0xFF346856;
        return false;
    }

    int entryOff = s_palOff + species * 8;  // 8 bytes per entry
    int bank = s_palBank;
    // Handle cross-bank boundary
    if (entryOff + 3 >= 16384) {
        // Fallback for entries beyond bank boundary
        outPal[1] = 0xFF88C070;
        outPal[2] = 0xFF346856;
        return false;
    }

    const uint8_t *pal = &rom.bank[bank][entryOff];
    uint16_t c1 = (uint16_t)pal[0] | ((uint16_t)pal[1] << 8);
    uint16_t c2 = (uint16_t)pal[2] | ((uint16_t)pal[3] << 8);
    outPal[1] = rgb555toARGB(c1);  // light species color
    outPal[2] = rgb555toARGB(c2);  // dark species color
    return true;
}

bool renderRomSprite(pax_buf_t *fb, int dx, int dy,
                     uint8_t species, bool isBack,
                     uint32_t bgARGB, int scale)
{
    if (!fb || species == 0 || species > 251) return false;
    if (!rom.bank || rom.length == 0) return false;

    // Check if the ROM has enough banks for Crystal's pic data
    int totalBanks = rom.length / 16384;
    if (totalBanks < 0x60) {
        ESP_LOGD(TAG, "ROM too small for Crystal pics (%d banks)", totalBanks);
        return false;
    }

    // Read PokemonPicPointers entry for this species
    const uint8_t *ptrTable = rom.bank[PIC_PTR_BANK];
    if (!ptrTable) return false;

    int entryOff = PIC_PTR_ADDR + (species - 1) * 6;
    if (entryOff + 5 >= 16384) return false;

    int subEntry = isBack ? 3 : 0;
    uint8_t bankEnc = ptrTable[entryOff + subEntry];
    uint16_t addr = (uint16_t)ptrTable[entryOff + subEntry + 1] |
                    ((uint16_t)ptrTable[entryOff + subEntry + 2] << 8);

    uint8_t realBank = bankEnc + PICS_FIX;
    uint16_t bankOff = addr & 0x3FFF;  // offset within the 16KB bank

    if (realBank >= totalBanks) {
        ESP_LOGW(TAG, "sprite bank 0x%02X out of range (total %d)", realBank, totalBanks);
        return false;
    }

    const uint8_t *sprData = &rom.bank[realBank][bankOff];
    size_t maxSprLen = 16384 - bankOff;  // can't read past bank boundary

    // Decompress LZ3 — max output 784 bytes (7x7 tiles * 16 bytes/tile)
    // Front sprites are animated (2 frames); we only want frame 1.
    // Back sprites are single frame.
    static uint8_t decompBuf[2048];  // generous buffer for animated frames
    size_t decompLen = lz3Decompress(sprData, maxSprLen, decompBuf, sizeof(decompBuf));

    if (decompLen == 0) {
        ESP_LOGW(TAG, "LZ3 decompress failed for species %u", species);
        return false;
    }

    // Determine tile dimensions from decompressed size.
    // Each tile = 16 bytes. Common sizes: 5x5=400, 6x6=576, 7x7=784 bytes (single frame).
    // Animated front sprites have 2 frames, so 800/1152/1568 bytes total.
    int tilesW, tilesH;
    size_t frameSize;
    if (decompLen >= 1568) { tilesW = 7; tilesH = 7; frameSize = 784; }
    else if (decompLen >= 1152) { tilesW = 6; tilesH = 6; frameSize = 576; }
    else if (decompLen >= 784) { tilesW = 7; tilesH = 7; frameSize = 784; }
    else if (decompLen >= 576) { tilesW = 6; tilesH = 6; frameSize = 576; }
    else if (decompLen >= 400) { tilesW = 5; tilesH = 5; frameSize = 400; }
    else { tilesW = 5; tilesH = 5; frameSize = decompLen; }

    int pixW = tilesW * 8;  // pixel width/height

    // Species-specific palette from ROM (falls back to GBC green if not found)
    uint32_t palette[4];
    getSpeciesPalette(species, palette, bgARGB);

    // Convert 2bpp tiles to pixel buffer
    static uint32_t pixBuf[56 * 56];
    memset(pixBuf, 0, sizeof(pixBuf));
    // Fill with background
    for (int i = 0; i < pixW * pixW; ++i) pixBuf[i] = bgARGB;

    tiles2bppToPixels(decompBuf, tilesW, tilesH, palette, pixBuf, pixW);

    // Render to pax framebuffer with scaling
    for (int py = 0; py < pixW; ++py) {
        for (int px = 0; px < pixW; ++px) {
            uint32_t c = pixBuf[py * pixW + px];
            if (scale <= 1) {
                pax_set_pixel(fb, c, dx + px, dy + py);
            } else {
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                        pax_set_pixel(fb, c, dx + px * scale + sx, dy + py * scale + sy);
            }
        }
    }

    ESP_LOGI(TAG, "ROM sprite: species=%u back=%d bank=0x%02X off=0x%04X decomp=%u tiles=%dx%d",
             species, isBack, realBank, bankOff, (unsigned)decompLen, tilesW, tilesH);
    return true;
}
