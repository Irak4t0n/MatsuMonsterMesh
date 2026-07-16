#pragma once
// ── Pokemon Red/Blue SRAM Patcher for Daycare ──────────────────────────────
// Reads party data from Game Boy SRAM on check-in (species, level, nickname,
// experience). On check-out, writes back gained XP + levels, recalculates
// stats, and fixes the save checksum.
//
// All offsets are for Pokemon Red/Blue/Yellow (English).
// NOT wired into build yet — standalone for validation.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "emulator_sram_iface.h"
#include "PokemonData.h"   // for Gen1Party / Gen1Pokemon (used by buildGen1Party)

// ── SRAM layout constants ──────────────────────────────────────────────────

static constexpr uint16_t SAV_PARTY_OFFSET      = 0x2F2C;
static constexpr uint16_t SAV_PARTY_COUNT        = 0x2F2C;  // 1 byte
static constexpr uint16_t SAV_SPECIES_LIST       = 0x2F2D;  // 7 bytes
static constexpr uint16_t SAV_POKEMON_DATA       = 0x2F34;  // 44 bytes x 6
static constexpr uint16_t SAV_OT_NAMES           = 0x303C;  // 11 bytes x 6
static constexpr uint16_t SAV_NICKNAMES          = 0x307E;  // 11 bytes x 6

static constexpr uint16_t SAV_CHECKSUM_OFFSET    = 0x3523;
static constexpr uint16_t SAV_CHECKSUM_START     = 0x2598;
static constexpr uint16_t SAV_CHECKSUM_END       = 0x3522;

static constexpr uint8_t  SAV_POKEMON_SIZE       = 44;      // party pokemon struct
static constexpr uint8_t  SAV_NAME_SIZE          = 11;
static constexpr uint8_t  SAV_STRING_TERMINATOR  = 0x50;

// ── Trainer + box-system constants (Gen 1 R/B) ─────────────────────────────
// Player's 2-byte trainer ID (wPlayerID mirror) — used as the OT ID on
// freshly caught Pokémon so they don't trade-evolve incorrectly.
static constexpr uint16_t SAV_TRAINER_ID         = 0x2605;

// Current box number (0-11). Bit 7 = "box changed" flag (ignored here).
static constexpr uint16_t SAV_CURRENT_BOX_NUM    = 0x284C;

// "Current box" mirror in bank 1. The game keeps the active PC box's full
// 1122-byte record here at all times; switching boxes via Bill's PC swaps
// the contents in/out of the per-box slots in banks 2/3 (0x4000+/0x6000+).
static constexpr uint16_t SAV_CURRENT_BOX        = 0x30C0;
static constexpr uint16_t SAV_BOX_SIZE           = 0x462;   // 1122 bytes
static constexpr uint8_t  SAV_BOX_CAPACITY       = 20;
static constexpr uint8_t  SAV_BOX_POKE_SIZE      = 33;      // box record (no level/maxStats)
static constexpr uint8_t  SAV_NUM_BOXES          = 12;
// Sub-region offsets relative to the start of a box (mirror or bank slot).
static constexpr uint16_t BOX_COUNT_OFF          = 0x000;   // 1 byte
static constexpr uint16_t BOX_SPECIES_OFF        = 0x001;   // 20 + 0xFF terminator
static constexpr uint16_t BOX_POKE_OFF           = 0x016;   // 20 × 33
static constexpr uint16_t BOX_OT_OFF             = 0x2AA;   // 20 × 11
static constexpr uint16_t BOX_NICK_OFF           = 0x386;   // 20 × 11

// Per-box permanent slots in SRAM banks 2 (boxes 1-6) and 3 (boxes 7-12).
// Each bank holds 6 boxes at SAV_BOX_SIZE intervals.
static constexpr uint16_t SAV_BANK2_BOX_START    = 0x4000;
static constexpr uint16_t SAV_BANK3_BOX_START    = 0x6000;
// Minimum SRAM size to access all 12 PC boxes (end of box 12).
static constexpr size_t   SAV_ALL_BOXES_MIN      = (size_t)SAV_BANK3_BOX_START + 6u * SAV_BOX_SIZE;

// ── WRAM party layout — Gen 1 (Red/Blue/Yellow) ──────────────────────────
// Offsets relative to WRAM bank 1 (ram.ibank[1]).
// Source: pret/pokered wram.asm (wPartyCount, wPartySpecies, wPartyMons, etc.)
static constexpr uint16_t WRAM_PARTY_COUNT   = 0x163;  // 0xD163
static constexpr uint16_t WRAM_SPECIES_LIST  = 0x164;  // 0xD164 (6 + 0xFF)
static constexpr uint16_t WRAM_POKEMON_DATA  = 0x16B;  // 0xD16B (44 × 6)
static constexpr uint16_t WRAM_OT_NAMES      = 0x273;  // 0xD273 (11 × 6)
static constexpr uint16_t WRAM_NICKNAMES     = 0x2B5;  // 0xD2B5 (11 × 6)

// ── WRAM party layout — Gen 2 (Gold/Silver/Crystal) ─────────────────────
// Offsets relative to WRAM bank 1 (ram.ibank[1]).
// Species bytes are Pokedex numbers directly (no internal-to-dex mapping).
// Pokemon struct is 48 bytes (32 box + 16 party-only).
// Source: pret/pokecrystal wram.asm
static constexpr uint16_t G2_WRAM_PARTY_COUNT  = 0xCD7;  // $DCD7
static constexpr uint16_t G2_WRAM_SPECIES_LIST = 0xCD8;  // $DCD8 (6 + 0xFF)
static constexpr uint16_t G2_WRAM_POKEMON_DATA = 0xCDF;  // $DCDF (48 × 6)
static constexpr uint16_t G2_WRAM_OT_NAMES     = 0xDFF;  // $DDFF (11 × 6)
static constexpr uint16_t G2_WRAM_NICKNAMES    = 0xE41;  // $DE41 (11 × 6)
static constexpr uint8_t  G2_POKEMON_SIZE      = 48;     // 0x30
// Gen 2 party Pokemon offsets
static constexpr uint8_t  G2_PKM_SPECIES       = 0x00;   // 1 byte (dex number)
static constexpr uint8_t  G2_PKM_MOVES         = 0x02;   // 4 bytes
static constexpr uint8_t  G2_PKM_EXP           = 0x08;   // 3 bytes BE
static constexpr uint8_t  G2_PKM_LEVEL         = 0x1F;   // 1 byte

// ── Offsets within the 44-byte Pokemon struct ──────────────────────────────

static constexpr uint8_t PKM_SPECIES     = 0x00;  // 1 byte (internal index!)
static constexpr uint8_t PKM_CURRENT_HP  = 0x01;  // 2 bytes BE
static constexpr uint8_t PKM_LEVEL_PC    = 0x03;  // 1 byte (box level)
static constexpr uint8_t PKM_STATUS      = 0x04;  // 1 byte
static constexpr uint8_t PKM_TYPE1       = 0x05;  // 1 byte
static constexpr uint8_t PKM_TYPE2       = 0x06;  // 1 byte
static constexpr uint8_t PKM_EXP         = 0x0E;  // 3 bytes BE
static constexpr uint8_t PKM_HP_EV       = 0x11;  // 2 bytes BE
static constexpr uint8_t PKM_ATK_EV      = 0x13;  // 2 bytes BE
static constexpr uint8_t PKM_DEF_EV      = 0x15;  // 2 bytes BE
static constexpr uint8_t PKM_SPD_EV      = 0x17;  // 2 bytes BE
static constexpr uint8_t PKM_SPC_EV      = 0x19;  // 2 bytes BE
static constexpr uint8_t PKM_DVS         = 0x1B;  // 2 bytes (packed IVs)
static constexpr uint8_t PKM_LEVEL_PARTY = 0x21;  // 1 byte (authoritative)
static constexpr uint8_t PKM_MAX_HP      = 0x22;  // 2 bytes BE
static constexpr uint8_t PKM_ATTACK      = 0x24;  // 2 bytes BE
static constexpr uint8_t PKM_DEFENSE     = 0x26;  // 2 bytes BE
static constexpr uint8_t PKM_SPEED       = 0x28;  // 2 bytes BE
static constexpr uint8_t PKM_SPECIAL     = 0x2A;  // 2 bytes BE

// ── Experience growth rates ────────────────────────────────────────────────

enum GrowthRate : uint8_t {
    GROWTH_MEDIUM_FAST = 0,  // n^3
    GROWTH_FAST        = 1,  // 4n^3 / 5
    GROWTH_MEDIUM_SLOW = 2,  // 1.2n^3 - 15n^2 + 100n - 140
    GROWTH_SLOW        = 3,  // 5n^3 / 4
};

// Growth rate for each Pokedex number (1-251)
// Source: pret/pokered + pret/pokecrystal base_stats, Bulbapedia exp type list
// 0=MediumFast(n^3), 1=Fast(4n^3/5), 2=MediumSlow(1.2n^3-15n^2+100n-140), 3=Slow(5n^3/4)
static const uint8_t speciesGrowthRate[252] = {
    0,                          // 0 = unused
    // 1-10: Bulbasaur..Caterpie
    2, 2, 2, 2, 2, 2, 2, 2, 2, 0,
    // 11-20: Metapod..Raticate
    0, 0, 0, 0, 0, 2, 2, 2, 0, 0,
    // 21-30: Spearow..Nidorina
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2,
    // 31-40: Nidoqueen..Wigglytuff
    2, 2, 2, 2, 1, 1, 0, 0, 1, 1,
    // 41-50: Zubat..Diglett
    0, 0, 2, 2, 2, 0, 0, 0, 0, 0,
    // 51-60: Dugtrio..Poliwag
    0, 0, 0, 0, 0, 0, 0, 3, 3, 2,
    // 61-70: Poliwhirl..Weepinbell
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    // 71-80: Victreebel..Slowbro
    2, 3, 3, 2, 2, 2, 0, 0, 0, 0,
    // 81-90: Magnemite..Shellder
    0, 0, 0, 0, 0, 0, 0, 0, 0, 3,
    // 91-100: Cloyster..Voltorb
    3, 2, 2, 2, 0, 0, 0, 0, 0, 0,
    // 101-110: Electrode..Weezing
    0, 3, 3, 0, 0, 0, 0, 0, 0, 0,
    // 111-120: Rhyhorn..Staryu
    3, 3, 1, 0, 0, 0, 0, 0, 0, 3,
    // 121-130: Starmie..Gyarados
    3, 0, 0, 0, 0, 0, 3, 3, 3, 3,
    // 131-140: Lapras..Kabuto
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 141-151: Kabutops..Mew
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2,
    // ── Gen 2 (152-251) ── Source: pret/pokecrystal base_stats/
    // 152-160: Chikorita, Bayleef, Meganium, Cyndaquil, Quilava, Typhlosion, Totodile, Croconaw, Feraligatr
    2, 2, 2, 2, 2, 2, 2, 2, 2,
    // 161-170: Sentret, Furret, Hoothoot, Noctowl, Ledyba, Ledian, Spinarak, Ariados, Crobat, Chinchou
    0, 0, 0, 0, 1, 1, 1, 1, 0, 3,
    // 171-180: Lanturn, Pichu, Cleffa, Igglybuff, Togepi, Togetic, Natu, Xatu, Mareep, Flaaffy
    3, 0, 1, 1, 1, 1, 0, 0, 2, 2,
    // 181-190: Ampharos, Bellossom, Marill, Azumarill, Sudowoodo, Politoed, Hoppip, Skiploom, Jumpluff, Aipom
    2, 2, 1, 1, 0, 2, 2, 2, 2, 1,
    // 191-200: Sunkern, Sunflora, Yanma, Wooper, Quagsire, Espeon, Umbreon, Murkrow, Slowking, Misdreavus
    2, 2, 0, 0, 0, 0, 0, 2, 0, 1,
    // 201-210: Unown, Wobbuffet, Girafarig, Pineco, Forretress, Dunsparce, Gligar, Steelix, Snubbull, Granbull
    0, 0, 0, 0, 0, 0, 2, 0, 1, 1,
    // 211-220: Qwilfish, Scizor, Shuckle, Heracross, Sneasel, Teddiursa, Ursaring, Slugma, Magcargo, Swinub
    0, 0, 2, 3, 2, 0, 0, 0, 0, 3,
    // 221-230: Piloswine, Corsola, Remoraid, Octillery, Delibird, Mantine, Skarmory, Houndour, Houndoom, Kingdra
    3, 1, 0, 0, 1, 3, 3, 3, 3, 0,
    // 231-240: Phanpy, Donphan, Porygon2, Stantler, Smeargle, Tyrogue, Hitmontop, Smoochum, Elekid, Magby
    0, 0, 0, 3, 1, 0, 0, 0, 0, 0,
    // 241-251: Miltank, Blissey, Raikou, Entei, Suicune, Larvitar, Pupitar, Tyranitar, Lugia, Ho-Oh, Celebi
    3, 1, 3, 3, 3, 3, 3, 3, 3, 3, 2
};

// ── Base stats for stat recalculation (HP, Atk, Def, Spd, Spc) ────────────
// Indexed by Pokedex number (1-151). 5 bytes each.

struct BaseStats {
    uint8_t hp, atk, def, spd, spc;
};

static const BaseStats speciesBaseStats[152] = {
    {0,0,0,0,0},           // 0 = unused
    {45,49,49,45,65},      // 1 Bulbasaur
    {60,62,63,60,80},      // 2 Ivysaur
    {80,82,83,80,100},     // 3 Venusaur
    {39,52,43,65,50},      // 4 Charmander
    {58,64,58,80,65},      // 5 Charmeleon
    {78,84,78,100,85},     // 6 Charizard
    {44,48,65,43,50},      // 7 Squirtle
    {59,63,80,58,65},      // 8 Wartortle
    {79,83,100,78,85},     // 9 Blastoise
    {45,30,35,45,20},      // 10 Caterpie
    {50,20,55,30,25},      // 11 Metapod
    {60,45,50,70,80},      // 12 Butterfree
    {40,35,30,50,20},      // 13 Weedle
    {45,25,50,35,25},      // 14 Kakuna
    {65,80,40,75,45},      // 15 Beedrill
    {40,45,40,56,35},      // 16 Pidgey
    {63,60,55,71,50},      // 17 Pidgeotto
    {83,80,75,91,70},      // 18 Pidgeot
    {30,56,35,72,25},      // 19 Rattata
    {55,81,60,97,50},      // 20 Raticate
    {40,60,30,70,31},      // 21 Spearow
    {65,90,65,100,61},     // 22 Fearow
    {35,60,44,55,40},      // 23 Ekans
    {60,85,69,80,65},      // 24 Arbok
    {35,55,30,90,50},      // 25 Pikachu
    {60,90,55,100,90},     // 26 Raichu
    {50,75,85,40,30},      // 27 Sandshrew
    {75,100,110,65,55},    // 28 Sandslash
    {55,47,52,41,40},      // 29 NidoranF
    {70,62,67,56,55},      // 30 Nidorina
    {90,82,87,76,75},      // 31 Nidoqueen
    {46,57,40,50,40},      // 32 NidoranM
    {61,72,57,65,55},      // 33 Nidorino
    {81,92,77,85,75},      // 34 Nidoking
    {70,45,48,35,60},      // 35 Clefairy
    {95,70,73,60,85},      // 36 Clefable
    {38,41,40,65,65},      // 37 Vulpix
    {73,76,75,100,100},    // 38 Ninetales
    {115,45,20,20,25},     // 39 Jigglypuff
    {140,70,45,45,50},     // 40 Wigglytuff
    {40,45,35,55,40},      // 41 Zubat
    {75,80,70,90,75},      // 42 Golbat
    {45,50,55,30,75},      // 43 Oddish
    {60,65,70,40,85},      // 44 Gloom
    {75,80,85,50,100},     // 45 Vileplume
    {35,70,55,25,55},      // 46 Paras
    {60,95,80,30,80},      // 47 Parasect
    {60,55,50,45,40},      // 48 Venonat
    {70,65,60,90,90},      // 49 Venomoth
    {10,55,25,95,45},      // 50 Diglett
    {35,80,50,120,70},     // 51 Dugtrio
    {40,45,35,90,40},      // 52 Meowth
    {65,70,60,115,65},     // 53 Persian
    {50,52,48,55,50},      // 54 Psyduck
    {80,82,78,85,80},      // 55 Golduck
    {40,80,35,70,35},      // 56 Mankey
    {65,105,60,95,60},     // 57 Primeape
    {55,70,45,60,50},      // 58 Growlithe
    {90,110,80,95,80},     // 59 Arcanine
    {40,50,40,90,40},      // 60 Poliwag
    {65,65,65,90,50},      // 61 Poliwhirl
    {90,85,95,70,70},      // 62 Poliwrath
    {25,20,15,90,105},     // 63 Abra
    {40,35,30,105,120},    // 64 Kadabra
    {55,50,45,120,135},    // 65 Alakazam
    {70,80,50,35,35},      // 66 Machop
    {80,100,70,45,50},     // 67 Machoke
    {90,130,80,55,65},     // 68 Machamp
    {50,75,35,40,70},      // 69 Bellsprout
    {65,90,50,55,85},      // 70 Weepinbell
    {80,105,65,70,100},    // 71 Victreebel
    {40,40,35,70,100},     // 72 Tentacool
    {80,70,65,100,120},    // 73 Tentacruel
    {40,80,100,20,30},     // 74 Geodude
    {55,95,115,35,45},     // 75 Graveler
    {80,110,130,45,55},    // 76 Golem
    {50,85,55,90,65},      // 77 Ponyta
    {65,100,70,105,80},    // 78 Rapidash
    {90,65,65,15,40},      // 79 Slowpoke
    {95,75,110,30,80},     // 80 Slowbro
    {25,35,70,45,95},      // 81 Magnemite
    {50,60,95,70,120},     // 82 Magneton
    {52,65,55,60,58},      // 83 Farfetch'd
    {35,85,45,75,35},      // 84 Doduo
    {60,110,70,100,60},    // 85 Dodrio
    {65,45,55,45,70},      // 86 Seel
    {90,70,80,70,95},      // 87 Dewgong
    {80,80,50,25,40},      // 88 Grimer
    {105,105,75,50,65},    // 89 Muk
    {30,65,100,40,45},     // 90 Shellder
    {50,95,180,70,85},     // 91 Cloyster
    {30,35,30,80,100},     // 92 Gastly
    {45,50,45,95,115},     // 93 Haunter
    {60,65,60,110,130},    // 94 Gengar
    {35,45,160,70,30},     // 95 Onix
    {60,48,45,42,90},      // 96 Drowzee
    {85,73,70,67,115},     // 97 Hypno
    {30,105,90,50,25},     // 98 Krabby
    {55,130,115,75,50},    // 99 Kingler
    {40,30,50,100,55},     // 100 Voltorb
    {60,50,70,140,80},     // 101 Electrode
    {60,40,80,40,60},      // 102 Exeggcute
    {95,95,85,55,125},     // 103 Exeggutor
    {50,50,95,35,40},      // 104 Cubone
    {60,80,110,45,50},     // 105 Marowak
    {50,120,53,87,35},     // 106 Hitmonlee
    {50,105,79,76,35},     // 107 Hitmonchan
    {90,55,75,30,60},      // 108 Lickitung
    {40,65,95,35,60},      // 109 Koffing
    {65,90,120,60,85},     // 110 Weezing
    {80,85,95,25,30},      // 111 Rhyhorn
    {105,130,120,40,45},   // 112 Rhydon
    {250,5,5,50,105},      // 113 Chansey
    {65,55,115,60,100},    // 114 Tangela
    {105,95,80,90,40},     // 115 Kangaskhan
    {30,40,70,60,70},      // 116 Horsea
    {55,65,95,85,95},      // 117 Seadra
    {45,67,60,63,50},      // 118 Goldeen
    {80,92,65,68,80},      // 119 Seaking
    {30,45,55,85,70},      // 120 Staryu
    {60,75,85,115,100},    // 121 Starmie
    {40,45,65,90,100},     // 122 Mr. Mime
    {70,110,80,105,55},    // 123 Scyther
    {65,50,35,95,95},      // 124 Jynx
    {65,83,57,105,85},     // 125 Electabuzz
    {65,95,57,93,85},      // 126 Magmar
    {65,125,100,85,55},    // 127 Pinsir
    {75,100,95,110,70},    // 128 Tauros
    {20,10,55,80,20},      // 129 Magikarp
    {95,125,79,81,100},    // 130 Gyarados
    {130,85,80,60,95},     // 131 Lapras
    {48,48,48,48,48},      // 132 Ditto
    {55,55,50,55,65},      // 133 Eevee
    {130,65,60,65,110},    // 134 Vaporeon
    {65,65,60,130,110},    // 135 Jolteon
    {65,130,60,65,110},    // 136 Flareon
    {65,60,70,40,75},      // 137 Porygon
    {35,40,100,35,90},     // 138 Omanyte
    {70,60,125,55,115},    // 139 Omastar
    {30,80,90,55,45},      // 140 Kabuto
    {60,115,105,80,70},    // 141 Kabutops
    {80,105,65,130,60},    // 142 Aerodactyl
    {160,110,65,30,65},    // 143 Snorlax
    {90,85,100,85,125},    // 144 Articuno
    {90,90,85,100,125},    // 145 Zapdos
    {90,100,90,90,125},    // 146 Moltres
    {41,64,45,50,50},      // 147 Dratini
    {61,84,65,70,70},      // 148 Dragonair
    {91,134,95,80,100},    // 149 Dragonite
    {106,110,90,130,154},  // 150 Mewtwo
    {100,100,100,100,100}, // 151 Mew
};

// ── Internal index ↔ Pokedex number mapping ────────────────────────────────
// Gen 1 uses an internal species index that differs from Pokedex number.
// Index 0 = Missingno, we only need 1-190 range.

// Source: pret/pokered constants/pokemon_constants.asm
// 0 = MissingNo / unused gap. Only 0x01-0xBE have real Pokemon (with gaps).
static const uint8_t internalToDex[256] = {
//   x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xA  xB  xC  xD  xE  xF
      0,112,115, 32, 35, 21,100, 34, 80,  2,103,108,102, 88, 94, 29, // 0x00
     31,104,111,131, 59,151,130, 90, 72, 92,123,120,  9,127,114,  0, // 0x10
      0, 58, 95, 22, 16, 79, 64, 75,113, 67,122,106,107, 24, 47, 54, // 0x20
     96, 76,  0,126,  0,125, 82,109,  0, 56, 86, 50,128,  0,  0,  0, // 0x30
     83, 48,149,  0,  0,  0, 84, 60,124,146,144,145,132, 52, 98,  0, // 0x40
      0,  0, 37, 38, 25, 26,  0,  0,147,148,140,141,116,117,  0,  0, // 0x50
     27, 28,138,139, 39, 40,133,136,135,134, 66, 41, 23, 46, 61, 62, // 0x60
     13, 14, 15,  0, 85, 57, 51, 49, 87,  0,  0, 10, 11, 12, 68,  0, // 0x70
     55, 97, 42,150,143,129,  0,  0, 89,  0, 99, 91,  0,101, 36,110, // 0x80
     53,105,  0, 93, 63, 65, 17, 18,121,  1,  3, 73,  0,118,119,  0, // 0x90
      0,  0,  0, 77, 78, 19, 20, 33, 30, 74,137,142,  0, 81,  0,  0, // 0xA0
      4,  7,  5,  8,  6,  0,  0,  0,  0, 43, 44, 45, 69, 70, 71,  0, // 0xB0
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // 0xC0
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // 0xD0
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // 0xE0
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, // 0xF0
};

// Reverse: Pokedex number -> internal index
// Source: pret/pokered constants/pokemon_constants.asm
static const uint8_t dexToInternal[152] = {
    0x00,  // 0 = unused
    0x99,  // 1 Bulbasaur
    0x09,  // 2 Ivysaur
    0x9A,  // 3 Venusaur
    0xB0,  // 4 Charmander
    0xB2,  // 5 Charmeleon
    0xB4,  // 6 Charizard
    0xB1,  // 7 Squirtle
    0xB3,  // 8 Wartortle
    0x1C,  // 9 Blastoise
    0x7B,  // 10 Caterpie
    0x7C,  // 11 Metapod
    0x7D,  // 12 Butterfree
    0x70,  // 13 Weedle
    0x71,  // 14 Kakuna
    0x72,  // 15 Beedrill
    0x24,  // 16 Pidgey
    0x96,  // 17 Pidgeotto
    0x97,  // 18 Pidgeot
    0xA5,  // 19 Rattata
    0xA6,  // 20 Raticate
    0x05,  // 21 Spearow
    0x23,  // 22 Fearow
    0x6C,  // 23 Ekans
    0x2D,  // 24 Arbok
    0x54,  // 25 Pikachu
    0x55,  // 26 Raichu
    0x60,  // 27 Sandshrew
    0x61,  // 28 Sandslash
    0x0F,  // 29 NidoranF
    0xA8,  // 30 Nidorina
    0x10,  // 31 Nidoqueen
    0x03,  // 32 NidoranM
    0xA7,  // 33 Nidorino
    0x07,  // 34 Nidoking
    0x04,  // 35 Clefairy
    0x8E,  // 36 Clefable
    0x52,  // 37 Vulpix
    0x53,  // 38 Ninetales
    0x64,  // 39 Jigglypuff
    0x65,  // 40 Wigglytuff
    0x6B,  // 41 Zubat
    0x82,  // 42 Golbat
    0xB9,  // 43 Oddish
    0xBA,  // 44 Gloom
    0xBB,  // 45 Vileplume
    0x6D,  // 46 Paras
    0x2E,  // 47 Parasect
    0x41,  // 48 Venonat
    0x77,  // 49 Venomoth
    0x3B,  // 50 Diglett
    0x76,  // 51 Dugtrio
    0x4D,  // 52 Meowth
    0x90,  // 53 Persian
    0x2F,  // 54 Psyduck
    0x80,  // 55 Golduck
    0x39,  // 56 Mankey
    0x75,  // 57 Primeape
    0x21,  // 58 Growlithe
    0x14,  // 59 Arcanine
    0x47,  // 60 Poliwag
    0x6E,  // 61 Poliwhirl
    0x6F,  // 62 Poliwrath
    0x94,  // 63 Abra
    0x26,  // 64 Kadabra
    0x95,  // 65 Alakazam
    0x6A,  // 66 Machop
    0x29,  // 67 Machoke
    0x7E,  // 68 Machamp
    0xBC,  // 69 Bellsprout
    0xBD,  // 70 Weepinbell
    0xBE,  // 71 Victreebel
    0x18,  // 72 Tentacool
    0x9B,  // 73 Tentacruel
    0xA9,  // 74 Geodude
    0x27,  // 75 Graveler
    0x31,  // 76 Golem
    0xA3,  // 77 Ponyta
    0xA4,  // 78 Rapidash
    0x25,  // 79 Slowpoke
    0x08,  // 80 Slowbro
    0xAD,  // 81 Magnemite
    0x36,  // 82 Magneton
    0x40,  // 83 Farfetch'd
    0x46,  // 84 Doduo
    0x74,  // 85 Dodrio
    0x3A,  // 86 Seel
    0x78,  // 87 Dewgong
    0x0D,  // 88 Grimer
    0x88,  // 89 Muk
    0x17,  // 90 Shellder
    0x8B,  // 91 Cloyster
    0x19,  // 92 Gastly
    0x93,  // 93 Haunter
    0x0E,  // 94 Gengar
    0x22,  // 95 Onix
    0x30,  // 96 Drowzee
    0x81,  // 97 Hypno
    0x4E,  // 98 Krabby
    0x8A,  // 99 Kingler
    0x06,  // 100 Voltorb
    0x8D,  // 101 Electrode
    0x0C,  // 102 Exeggcute
    0x0A,  // 103 Exeggutor
    0x11,  // 104 Cubone
    0x91,  // 105 Marowak
    0x2B,  // 106 Hitmonlee
    0x2C,  // 107 Hitmonchan
    0x0B,  // 108 Lickitung
    0x37,  // 109 Koffing
    0x8F,  // 110 Weezing
    0x12,  // 111 Rhyhorn
    0x01,  // 112 Rhydon
    0x28,  // 113 Chansey
    0x1E,  // 114 Tangela
    0x02,  // 115 Kangaskhan
    0x5C,  // 116 Horsea
    0x5D,  // 117 Seadra
    0x9D,  // 118 Goldeen
    0x9E,  // 119 Seaking
    0x1B,  // 120 Staryu
    0x98,  // 121 Starmie
    0x2A,  // 122 Mr. Mime
    0x1A,  // 123 Scyther
    0x48,  // 124 Jynx
    0x35,  // 125 Electabuzz
    0x33,  // 126 Magmar
    0x1D,  // 127 Pinsir
    0x3C,  // 128 Tauros
    0x85,  // 129 Magikarp
    0x16,  // 130 Gyarados
    0x13,  // 131 Lapras
    0x4C,  // 132 Ditto
    0x66,  // 133 Eevee
    0x69,  // 134 Vaporeon
    0x68,  // 135 Jolteon
    0x67,  // 136 Flareon
    0xAA,  // 137 Porygon
    0x62,  // 138 Omanyte
    0x63,  // 139 Omastar
    0x5A,  // 140 Kabuto
    0x5B,  // 141 Kabutops
    0xAB,  // 142 Aerodactyl
    0x84,  // 143 Snorlax
    0x4A,  // 144 Articuno
    0x4B,  // 145 Zapdos
    0x49,  // 146 Moltres
    0x58,  // 147 Dratini
    0x59,  // 148 Dragonair
    0x42,  // 149 Dragonite
    0x83,  // 150 Mewtwo
    0x15,  // 151 Mew
};

// ── Gen 1 text encoding ↔ ASCII ────────────────────────────────────────────
// gen1CharToAscii is defined in Gen1Species.h — use that version.
// This extended version adds punctuation the base version doesn't cover.

#ifndef GEN1_CHAR_TO_ASCII_DEFINED
#define GEN1_CHAR_TO_ASCII_DEFINED
inline char gen1CharToAscii(uint8_t c) {
    if (c >= 0x80 && c <= 0x99) return 'A' + (c - 0x80);  // A-Z
    if (c >= 0xA0 && c <= 0xB9) return 'a' + (c - 0xA0);  // a-z
    if (c >= 0xF6 && c <= 0xFF) return '0' + (c - 0xF6);  // 0-9
    if (c == 0x7F) return ' ';
    if (c == 0xE8) return '.';
    if (c == 0xE3) return '-';
    if (c == 0xEF) return '\'';
    if (c == 0xF4) return ',';
    if (c == 0xF3) return '/';
    if (c == 0x50) return '\0';  // string terminator
    return '?';
}
#endif

static uint8_t asciiToGen1Char(char c) {
    if (c >= 'A' && c <= 'Z') return 0x80 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 0xA0 + (c - 'a');
    if (c >= '0' && c <= '9') return 0xF6 + (c - '0');
    if (c == ' ') return 0x7F;
    if (c == '.') return 0xE8;
    if (c == '-') return 0xE3;
    if (c == '\'') return 0xEF;
    if (c == ',') return 0xF4;
    if (c == '/') return 0xF3;
    if (c == '\0') return 0x50;
    return 0x50;  // unknown → terminator
}

// ── Helper: read/write big-endian values ───────────────────────────────────

static inline uint16_t readBE16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static inline void writeBE16(uint8_t *p, uint16_t val) {
    p[0] = (val >> 8) & 0xFF;
    p[1] = val & 0xFF;
}

static inline uint32_t readBE24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline void writeBE24(uint8_t *p, uint32_t val) {
    p[0] = (val >> 16) & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    p[2] = val & 0xFF;
}

// ── Experience calculation ─────────────────────────────────────────────────

static uint32_t expForLevel(uint8_t dexNum, uint8_t level) {
    if (dexNum == 0 || dexNum > 251 || level <= 1) return 0;

    uint32_t n = level;
    uint32_t n3 = n * n * n;

    switch (speciesGrowthRate[dexNum]) {
        case GROWTH_FAST:
            return (4 * n3) / 5;
        case GROWTH_MEDIUM_FAST:
            return n3;
        case GROWTH_MEDIUM_SLOW: {
            // 1.2*n^3 - 15*n^2 + 100*n - 140
            // Use integer math: (6*n^3)/5 - 15*n^2 + 100*n - 140
            int32_t exp = (int32_t)((6 * n3) / 5) - (int32_t)(15 * n * n)
                        + (int32_t)(100 * n) - 140;
            return (exp < 0) ? 0 : (uint32_t)exp;
        }
        case GROWTH_SLOW:
            return (5 * n3) / 4;
        default:
            return n3;  // fallback to medium fast
    }
}

// Given current total EXP, find what level that corresponds to
static uint8_t levelForExp(uint8_t dexNum, uint32_t totalExp) {
    if (dexNum == 0 || dexNum > 251) return 1;

    uint8_t level = 1;
    for (uint8_t l = 2; l <= 100; l++) {
        if (expForLevel(dexNum, l) > totalExp) break;
        level = l;
    }
    return level;
}

// ── Stat calculation (Gen 1 formula) ───────────────────────────────────────

static inline uint16_t isqrt16(uint16_t n) {
    // Integer square root for stat exp calculation
    if (n == 0) return 0;
    uint16_t x = n;
    uint16_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

static uint16_t calcStatHP(uint8_t base, uint8_t dv, uint16_t statExp, uint8_t level) {
    uint16_t sqrtExp = isqrt16(statExp);
    // ceil(sqrt(statExp)) — isqrt gives floor, so check
    if ((uint32_t)sqrtExp * sqrtExp < statExp) sqrtExp++;
    uint16_t evBonus = sqrtExp / 4;
    return ((uint16_t)((base + dv) * 2 + evBonus) * level / 100) + level + 10;
}

static uint16_t calcStat(uint8_t base, uint8_t dv, uint16_t statExp, uint8_t level) {
    uint16_t sqrtExp = isqrt16(statExp);
    if ((uint32_t)sqrtExp * sqrtExp < statExp) sqrtExp++;
    uint16_t evBonus = sqrtExp / 4;
    return ((uint16_t)((base + dv) * 2 + evBonus) * level / 100) + 5;
}

// ── The Patcher ────────────────────────────────────────────────────────────

struct DaycarePartyInfo {
    uint8_t  dexNum;         // Pokedex number (1-151)
    uint8_t  level;          // from SAV
    uint32_t totalExp;       // from SAV (3-byte BE)
    char     nickname[11];   // decoded to ASCII
    char     otName[11];     // decoded to ASCII
    uint8_t  moves[4];       // 4 move IDs from SAV
};

class DaycareSavPatcher {
public:
    // Minimum buffer size the patcher needs to safely read/write everything
    // up through the checksum byte (0x3523). A real Gen 1 SAV is 32 KB.
    static constexpr size_t SAV_MIN_BYTES = (size_t)SAV_CHECKSUM_OFFSET + 1;

    // ── Read party from SRAM ───────────────────────────────────────────
    // sram: pointer to the full 32KB SRAM buffer (must be non-null)
    // out: array of 6 DaycarePartyInfo to fill
    // Returns party count (0-6); 0 also signals "sram missing".
    static uint8_t readParty(const uint8_t *sram, DaycarePartyInfo *out) {
        if (!sram || !out) return 0;
        uint8_t count = sram[SAV_PARTY_COUNT];
        if (count > 6) count = 6;

        for (uint8_t i = 0; i < count; i++) {
            const uint8_t *pkm = &sram[SAV_POKEMON_DATA + i * SAV_POKEMON_SIZE];

            // Species: internal index → Pokedex number
            uint8_t internalIdx = pkm[PKM_SPECIES];
            out[i].dexNum = internalToDex[internalIdx];

            // Level (party authoritative)
            out[i].level = pkm[PKM_LEVEL_PARTY];

            // Experience (3 bytes big-endian)
            out[i].totalExp = readBE24(&pkm[PKM_EXP]);

            // Nickname (Gen 1 encoding → ASCII)
            const uint8_t *nickRaw = &sram[SAV_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].nickname[j] = '\0';
                    break;
                }
                out[i].nickname[j] = gen1CharToAscii(nickRaw[j]);
                if (j == 9) out[i].nickname[10] = '\0';
            }

            // OT name
            const uint8_t *otRaw = &sram[SAV_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].otName[j] = '\0';
                    break;
                }
                out[i].otName[j] = gen1CharToAscii(otRaw[j]);
                if (j == 9) out[i].otName[10] = '\0';
            }

            // Moves (bytes 0x08-0x0B in the party Pokemon struct)
            for (int m = 0; m < 4; m++) out[i].moves[m] = pkm[0x08 + m];
        }
        return count;
    }

    // ── Read party from WRAM (live emulator state) ────────────────────
    // wram_bank1: pointer to gnuboy's ram.ibank[1] (4096 bytes, maps to
    //             Game Boy address 0xD000-0xDFFF). This is ALWAYS up-to-
    //             date — no need for the player to save in-game first.
    static uint8_t readPartyFromWRAM(const uint8_t *wram_bank1,
                                     DaycarePartyInfo *out) {
        if (!wram_bank1 || !out) return 0;
        uint8_t count = wram_bank1[WRAM_PARTY_COUNT];
        if (count > 6) count = 6;

        for (uint8_t i = 0; i < count; i++) {
            const uint8_t *pkm = &wram_bank1[WRAM_POKEMON_DATA + i * SAV_POKEMON_SIZE];
            uint8_t internalIdx = pkm[PKM_SPECIES];
            out[i].dexNum = internalToDex[internalIdx];
            out[i].level = pkm[PKM_LEVEL_PARTY];
            out[i].totalExp = readBE24(&pkm[PKM_EXP]);

            const uint8_t *nickRaw = &wram_bank1[WRAM_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].nickname[j] = '\0'; break;
                }
                out[i].nickname[j] = gen1CharToAscii(nickRaw[j]);
                if (j == 9) out[i].nickname[10] = '\0';
            }
            const uint8_t *otRaw = &wram_bank1[WRAM_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].otName[j] = '\0'; break;
                }
                out[i].otName[j] = gen1CharToAscii(otRaw[j]);
                if (j == 9) out[i].otName[10] = '\0';
            }
            for (int m = 0; m < 4; m++) out[i].moves[m] = pkm[0x08 + m];
        }
        return count;
    }

    // ── Read party from WRAM — Gen 2 (Gold/Silver/Crystal) ──────────
    static uint8_t readPartyFromWRAM_Gen2(const uint8_t *wram_bank1,
                                          DaycarePartyInfo *out) {
        if (!wram_bank1 || !out) return 0;
        uint8_t count = wram_bank1[G2_WRAM_PARTY_COUNT];
        if (count > 6) count = 6;

        for (uint8_t i = 0; i < count; i++) {
            const uint8_t *pkm = &wram_bank1[G2_WRAM_POKEMON_DATA + i * G2_POKEMON_SIZE];
            // Gen 2 species byte IS the Pokedex number directly
            out[i].dexNum = pkm[G2_PKM_SPECIES];
            out[i].level  = pkm[G2_PKM_LEVEL];
            out[i].totalExp = readBE24(&pkm[G2_PKM_EXP]);

            const uint8_t *nickRaw = &wram_bank1[G2_WRAM_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].nickname[j] = '\0'; break;
                }
                out[i].nickname[j] = gen1CharToAscii(nickRaw[j]);
                if (j == 9) out[i].nickname[10] = '\0';
            }
            const uint8_t *otRaw = &wram_bank1[G2_WRAM_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; j++) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out[i].otName[j] = '\0'; break;
                }
                out[i].otName[j] = gen1CharToAscii(otRaw[j]);
                if (j == 9) out[i].otName[10] = '\0';
            }
            for (int m = 0; m < 4; m++) {
                uint8_t mv = pkm[G2_PKM_MOVES + m];
                out[i].moves[m] = (mv <= 165) ? mv : 0; // drop Gen 2 moves
            }
        }
        return count;
    }

    // ── Write daycare XP/levels back to SRAM ───────────────────────────
    // sram: writable 32KB SRAM buffer
    // partyIdx: which party slot (0-5)
    // dexNum: Pokedex number of the Pokemon
    // xpGained: total XP gained during daycare
    // Returns new level (or 0 on error)
    static uint8_t patchPokemon(uint8_t *sram, uint8_t partyIdx, uint8_t dexNum,
                                 uint32_t xpGained) {
        if (!sram || partyIdx >= 6 || dexNum == 0 || dexNum > 151) return 0;

        uint8_t *pkm = &sram[SAV_POKEMON_DATA + partyIdx * SAV_POKEMON_SIZE];

        // Read current EXP
        uint32_t currentExp = readBE24(&pkm[PKM_EXP]);

        // Add daycare XP
        uint32_t newExp = currentExp + xpGained;

        // Cap at level 100's EXP for this species' growth rate
        uint32_t maxExp = expForLevel(dexNum, 100);
        if (newExp > maxExp) newExp = maxExp;

        // Calculate new level
        uint8_t newLevel = levelForExp(dexNum, newExp);
        if (newLevel > 100) newLevel = 100;

        // Write new EXP (3 bytes BE)
        writeBE24(&pkm[PKM_EXP], newExp);

        // Write new level (both box and party)
        pkm[PKM_LEVEL_PC] = newLevel;
        pkm[PKM_LEVEL_PARTY] = newLevel;

        // Recalculate stats
        recalcStats(pkm, dexNum, newLevel);

        return newLevel;
    }

    // ── Recalculate and fix the save checksum ──────────────────────────
    static void fixChecksum(uint8_t *sram) {
        if (!sram) return;
        uint8_t sum = 0;
        for (uint32_t i = SAV_CHECKSUM_START; i <= SAV_CHECKSUM_END; i++) {
            sum += sram[i];
        }
        sram[SAV_CHECKSUM_OFFSET] = ~sum;
    }

    // ── Caught-Pokémon insertion ───────────────────────────────────────
    // Description of a freshly-caught wild, captured in the terminal's
    // catch flow. Caller fills in everything from the live BattlePoke;
    // this struct intentionally mirrors that data so the patcher doesn't
    // need to depend on the battle engine headers.
    struct CaughtMon {
        uint8_t  dexNum;          // Pokédex number (1..151)
        uint8_t  level;           // 1..100
        uint16_t hp, maxHp;       // current and max HP at catch time
        uint16_t atk, def, spd, spc;
        uint8_t  type1, type2;    // type indices (Gen 1 numbering)
        uint8_t  moves[4];
        uint8_t  pp[4];
        uint8_t  dvs[2];          // packed DV pair (matches PKM_DVS layout)
        char     nickname[11];    // ASCII; encoded to Gen 1 charset on write
    };

    // Returns:
    //   0       → added to party
    //   1..12   → added to PC box N (1-indexed)
    //  -1       → no room (party AND all PC boxes full)
    //  -2       → SRAM unavailable / invalid CaughtMon
    //
    // Caller must fixChecksum() afterwards (skipped here so multiple writes
    // can be batched). Searches boxes 1-12 sequentially when the party is
    // full, using the current-box mirror for the active box and the
    // permanent bank 2/3 slots for all others.
    static int addCaughtMon(uint8_t *sram, size_t sramSize, const CaughtMon &mon) {
        if (!sram) return -2;
        if (mon.dexNum == 0 || mon.dexNum > 151) return -2;

        const uint8_t internalIdx = dexToInternal[mon.dexNum];

        // Pull the player's identity off the save so the new mon's OT
        // matches their existing party (otherwise the game treats it as a
        // traded Pokémon — disobedient at high levels, etc.).
        uint16_t trainer_id = readBE16(&sram[SAV_TRAINER_ID]);
        if (trainer_id == 0 || trainer_id == 0xFFFF) trainer_id = 0xC0DE;

        uint8_t trainer_name[SAV_NAME_SIZE] = {};
        memcpy(trainer_name, &sram[SAV_CHECKSUM_START], SAV_NAME_SIZE);
        // Player name is stored Gen-1-encoded with a 0x50 terminator. If the
        // SAV is uninitialised guarantee a terminator so the game doesn't
        // chase past the buffer when rendering OT in PC summaries.
        bool terminated = false;
        for (int j = 0; j < SAV_NAME_SIZE; ++j) {
            if (trainer_name[j] == SAV_STRING_TERMINATOR) { terminated = true; break; }
        }
        if (!terminated) trainer_name[SAV_NAME_SIZE - 1] = SAV_STRING_TERMINATOR;

        const uint32_t totalExp = expForLevel(mon.dexNum, mon.level);

        uint8_t party_count = sram[SAV_PARTY_COUNT];

        // ── Party path (count < 6) ────────────────────────────────────
        if (party_count < 6) {
            const uint8_t i = party_count;
            sram[SAV_PARTY_COUNT] = party_count + 1;

            // Species list is `count` species followed by 0xFF terminator.
            // Insert at slot i, push terminator to slot i+1.
            sram[SAV_SPECIES_LIST + i]     = internalIdx;
            sram[SAV_SPECIES_LIST + i + 1] = 0xFF;

            uint8_t *pkm = &sram[SAV_POKEMON_DATA + i * SAV_POKEMON_SIZE];
            memset(pkm, 0, SAV_POKEMON_SIZE);
            pkm[PKM_SPECIES]     = internalIdx;
            writeBE16(&pkm[PKM_CURRENT_HP], mon.hp ? mon.hp : mon.maxHp);
            pkm[PKM_LEVEL_PC]    = mon.level;
            pkm[PKM_STATUS]      = 0;
            pkm[PKM_TYPE1]       = mon.type1;
            pkm[PKM_TYPE2]       = mon.type2;
            pkm[7]               = 45;        // catchRate fallback (unused post-catch)
            memcpy(&pkm[8], mon.moves, 4);
            writeBE16(&pkm[12], trainer_id);  // otId
            writeBE24(&pkm[PKM_EXP], totalExp);
            // EVs left zero (default for fresh wild captures).
            pkm[PKM_DVS]         = mon.dvs[0];
            pkm[PKM_DVS + 1]     = mon.dvs[1];
            memcpy(&pkm[29], mon.pp, 4);
            pkm[PKM_LEVEL_PARTY] = mon.level;
            writeBE16(&pkm[PKM_MAX_HP],  mon.maxHp);
            writeBE16(&pkm[PKM_ATTACK],  mon.atk);
            writeBE16(&pkm[PKM_DEFENSE], mon.def);
            writeBE16(&pkm[PKM_SPEED],   mon.spd);
            writeBE16(&pkm[PKM_SPECIAL], mon.spc);

            memcpy(&sram[SAV_OT_NAMES + i * SAV_NAME_SIZE], trainer_name, SAV_NAME_SIZE);
            writeNickname(&sram[SAV_NICKNAMES + i * SAV_NAME_SIZE], mon.nickname);
            return 0;
        }

        // ── PC box path (party full) — search boxes 1-12 sequentially ─
        uint8_t curBoxNum = (sramSize > SAV_CURRENT_BOX_NUM)
                                ? (sram[SAV_CURRENT_BOX_NUM] & 0x0F) : 0;
        if (curBoxNum >= SAV_NUM_BOXES) curBoxNum = 0;

        for (uint8_t b = 0; b < SAV_NUM_BOXES; ++b) {
            uint16_t boxBase;
            if (b == curBoxNum) {
                // Active box → use the mirror (always within bank 1)
                boxBase = SAV_CURRENT_BOX;
            } else {
                // Inactive box → permanent slot in bank 2/3
                uint16_t perm = boxPermanentAddr(b);
                if (sramSize < (size_t)perm + SAV_BOX_SIZE) continue;
                boxBase = perm;
            }

            if (writeToBox(sram, boxBase, internalIdx, mon,
                           trainer_name, trainer_id, totalExp))
                return 1 + (int)b;   // 1-indexed box number
        }

        return -1;   // all 12 boxes full
    }

    // IEmulatorSRAM overload — flat-buffer backends only (matches the rest
    // of this class).
    static int addCaughtMon(IEmulatorSRAM *sram, const CaughtMon &mon) {
        if (!sram) return -2;
        size_t sz = iemu_sram_size(sram);
        if (sz < SAV_MIN_BYTES) return -2;
        uint8_t *buf = iemu_sram_data(sram);
        if (!buf) return -2;
        int r = addCaughtMon(buf, sz, mon);
        if (r >= 0) iemu_sram_mark_dirty(sram);
        return r;
    }

    // ── Full checkout: patch all party Pokemon and fix checksum ─────────
    // xpGained: array of XP gained per party slot
    // dexNums: array of Pokedex numbers per party slot
    // partyCount: number of Pokemon
    // Returns true if any Pokemon was patched
    static bool checkout(uint8_t *sram, const uint8_t *dexNums,
                         const uint32_t *xpGained, uint8_t partyCount) {
        if (!sram || !dexNums || !xpGained) return false;
        bool patched = false;
        for (uint8_t i = 0; i < partyCount && i < 6; i++) {
            if (xpGained[i] > 0) {
                patchPokemon(sram, i, dexNums[i], xpGained[i]);
                patched = true;
            }
        }
        if (patched) {
            fixChecksum(sram);
        }
        return patched;
    }

    // ── IEmulatorSRAM-based overloads ──────────────────────────────────
    // Per the MatsuMonsterMesh port, all cart-RAM reads/writes go through
    // the IEmulatorSRAM interface. For flat-buffer backends (gnuboy) we
    // grab the underlying pointer once and reuse the raw-ptr code paths.
    // Future banked-backend emulators that return NULL from data() would
    // need a read/write-loop variant; not implemented yet.

    static uint8_t readParty(const IEmulatorSRAM *sram, DaycarePartyInfo *out) {
        if (!sram || !out) return 0;
        if (iemu_sram_size(sram) < SAV_MIN_BYTES) return 0;
        const uint8_t *buf = iemu_sram_data(sram);
        if (!buf) return 0;  // non-flat backend not supported yet
        return readParty(buf, out);
    }

    static bool checkout(IEmulatorSRAM *sram, const uint8_t *dexNums,
                         const uint32_t *xpGained, uint8_t partyCount) {
        if (!sram || !dexNums || !xpGained) return false;
        if (iemu_sram_size(sram) < SAV_MIN_BYTES) return false;
        uint8_t *buf = iemu_sram_data(sram);
        if (!buf) return false;
        bool ok = checkout(buf, dexNums, xpGained, partyCount);
        if (ok) iemu_sram_mark_dirty(sram);
        return ok;
    }

    // ── Build a full Gen1Party from a flat SRAM buffer ─────────────────
    // Ported verbatim from upstream MonsterMeshModule::buildTerminalPartyFromSram
    // (_refs_monster_mesh/.../MonsterMeshModule.cpp:2598). Upstream cached
    // this once at boot and reused it for every battle / lobby challenge.
    // Returns party count (0..6); 0 means SRAM unavailable or empty party.
    static uint8_t buildGen1Party(const uint8_t *sram, Gen1Party *out) {
        if (!sram || !out) return 0;
        memset(out, 0, sizeof(*out));
        uint8_t count = sram[SAV_PARTY_COUNT];
        if (count > 6) count = 6;
        out->count = count;
        memcpy(out->species, &sram[SAV_SPECIES_LIST], 7);
        memcpy(out->mons,    &sram[SAV_POKEMON_DATA], count * sizeof(Gen1Pokemon));
        // Convert Gen 1 internal codes → dex numbers (engine uses dex directly)
        for (uint8_t i = 0; i < count; ++i) {
            out->species[i]      = internalToDex[out->species[i]];
            out->mons[i].species = internalToDex[out->mons[i].species];
        }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t *nickRaw = &sram[SAV_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out->nicknames[i][j] = 0;
                    break;
                }
                out->nicknames[i][j] = (uint8_t)gen1CharToAscii(nickRaw[j]);
                if (j == 9) out->nicknames[i][10] = 0;
            }
            const uint8_t *otRaw = &sram[SAV_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out->otNames[i][j] = 0;
                    break;
                }
                out->otNames[i][j] = (uint8_t)gen1CharToAscii(otRaw[j]);
                if (j == 9) out->otNames[i][10] = 0;
            }
        }
        return count;
    }

    static uint8_t buildGen1Party(const IEmulatorSRAM *sram, Gen1Party *out) {
        if (!sram || !out) return 0;
        if (iemu_sram_size(sram) < SAV_MIN_BYTES) return 0;
        const uint8_t *buf = iemu_sram_data(sram);
        if (!buf) return 0;
        return buildGen1Party(buf, out);
    }

    // ── Build Gen1Party from Gen 2 WRAM (Crystal/Gold/Silver) ──────────
    // Maps the Gen 2 48-byte party struct into the Gen 1 44-byte layout
    // used by the battle engine. Gen 2 species 1-151 map 1:1 to Gen 1;
    // species >151 are clamped to dex 151 (Mew) for battle compatibility.
    static uint8_t buildGen1PartyFromWRAM_Gen2(const uint8_t *wram_bank1,
                                               Gen1Party *out) {
        if (!wram_bank1 || !out) return 0;
        memset(out, 0, sizeof(*out));
        uint8_t count = wram_bank1[G2_WRAM_PARTY_COUNT];
        if (count > 6) count = 6;
        out->count = count;

        // Species list — dex numbers directly (Gen 2 format)
        for (uint8_t i = 0; i < count; ++i)
            out->species[i] = wram_bank1[G2_WRAM_SPECIES_LIST + i];
        out->species[count] = 0xFF;

        // Map each 48-byte Gen 2 mon into the 44-byte Gen 1 struct
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t *g2 = &wram_bank1[G2_WRAM_POKEMON_DATA + i * G2_POKEMON_SIZE];
            Gen1Pokemon &g1 = out->mons[i];
            memset(&g1, 0, sizeof(g1));

            g1.species   = g2[0x00]; // dex number directly
            g1.hp[0]     = g2[0x22]; g1.hp[1] = g2[0x23];     // curHP
            g1.boxLevel  = g2[0x1F];                            // level
            g1.status    = g2[0x20];
            // Gen 1 only has moves 1-165; drop Gen 2 moves (>165)
            for (int m = 0; m < 4; ++m)
                g1.moves[m] = (g2[0x02 + m] <= 165) ? g2[0x02 + m] : 0;
            g1.otId[0]   = g2[0x06]; g1.otId[1]  = g2[0x07];
            g1.exp[0]    = g2[0x08]; g1.exp[1]   = g2[0x09]; g1.exp[2] = g2[0x0A];
            g1.hpExp[0]  = g2[0x0B]; g1.hpExp[1] = g2[0x0C];
            g1.atkExp[0] = g2[0x0D]; g1.atkExp[1]= g2[0x0E];
            g1.defExp[0] = g2[0x0F]; g1.defExp[1]= g2[0x10];
            g1.spdExp[0] = g2[0x11]; g1.spdExp[1]= g2[0x12];
            g1.spcExp[0] = g2[0x13]; g1.spcExp[1]= g2[0x14];
            g1.dvs[0]    = g2[0x15]; g1.dvs[1]   = g2[0x16];
            g1.pp[0]     = g2[0x17]; g1.pp[1]    = g2[0x18];
            g1.pp[2]     = g2[0x19]; g1.pp[3]    = g2[0x1A];
            // If ALL moves were Gen 2 (filtered to 0), give Struggle so the
            // mon can still fight in the Gen 1 battle engine.
            if (g1.moves[0] == 0 && g1.moves[1] == 0 &&
                g1.moves[2] == 0 && g1.moves[3] == 0) {
                g1.moves[0] = 165; // Struggle
                g1.pp[0]    = 10;
            }
            g1.level     = g2[0x1F];
            g1.maxHp[0]  = g2[0x24]; g1.maxHp[1] = g2[0x25];
            g1.atk[0]    = g2[0x26]; g1.atk[1]   = g2[0x27];
            g1.def[0]    = g2[0x28]; g1.def[1]   = g2[0x29];
            g1.spd[0]    = g2[0x2A]; g1.spd[1]   = g2[0x2B];
            g1.spc[0]    = g2[0x2C]; g1.spc[1]   = g2[0x2D]; // use SpAtk as Spc
        }

        // Nicknames + OT names
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t *nickRaw = &wram_bank1[G2_WRAM_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out->nicknames[i][j] = 0; break;
                }
                out->nicknames[i][j] = (uint8_t)gen1CharToAscii(nickRaw[j]);
                if (j == 9) out->nicknames[i][10] = 0;
            }
            const uint8_t *otRaw = &wram_bank1[G2_WRAM_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out->otNames[i][j] = 0; break;
                }
                out->otNames[i][j] = (uint8_t)gen1CharToAscii(otRaw[j]);
                if (j == 9) out->otNames[i][10] = 0;
            }
        }
        return count;
    }

    // ── Patch Gen 2 Pokemon XP/level directly in WRAM ─────────────────
    // Called while the emulator is paused (terminal active). Adds xpGained
    // to the Pokemon at partyIdx, recalculates level, and writes back.
    // Returns the new level (0 on failure).
    static uint8_t patchGen2WRAM(uint8_t *wram_bank1, uint8_t partyIdx,
                                  uint8_t dexNum, uint32_t xpGained) {
        if (!wram_bank1 || partyIdx >= 6 || dexNum == 0) return 0;
        uint8_t count = wram_bank1[G2_WRAM_PARTY_COUNT];
        if (partyIdx >= count) return 0;

        uint8_t *g2 = &wram_bank1[G2_WRAM_POKEMON_DATA + partyIdx * G2_POKEMON_SIZE];
        // Read current EXP (3 bytes BE at offset 0x08)
        uint32_t curExp = ((uint32_t)g2[0x08] << 16) |
                          ((uint32_t)g2[0x09] << 8) | g2[0x0A];
        uint32_t newExp = curExp + xpGained;
        uint32_t maxExp = (dexNum <= 251) ? expForLevel(dexNum, 100) : 1000000;
        if (newExp > maxExp) newExp = maxExp;

        // Write new EXP
        g2[0x08] = (uint8_t)(newExp >> 16);
        g2[0x09] = (uint8_t)(newExp >> 8);
        g2[0x0A] = (uint8_t)(newExp);

        // Calculate and write new level
        uint8_t newLevel = (dexNum <= 251) ? levelForExp(dexNum, newExp) : g2[0x1F];
        if (newLevel > 100) newLevel = 100;
        g2[0x1F] = newLevel;

        return newLevel;
    }

    // ── Add caught Pokemon to Gen 2 WRAM party ────────────────────────
    // Writes directly to live WRAM (party only, no PC support).
    // Returns 0 on success (added to party), -1 if party full.
    static int addCaughtMonGen2(uint8_t *wram_bank1, const CaughtMon &mon) {
        if (!wram_bank1 || mon.dexNum == 0) return -2;
        uint8_t count = wram_bank1[G2_WRAM_PARTY_COUNT];
        if (count >= 6) return -1;  // party full

        uint8_t i = count;
        wram_bank1[G2_WRAM_PARTY_COUNT] = count + 1;

        // Species list (dex numbers directly for Gen 2)
        wram_bank1[G2_WRAM_SPECIES_LIST + i] = mon.dexNum;
        wram_bank1[G2_WRAM_SPECIES_LIST + i + 1] = 0xFF; // terminator

        // 48-byte Gen 2 Pokemon struct
        uint8_t *g2 = &wram_bank1[G2_WRAM_POKEMON_DATA + i * G2_POKEMON_SIZE];
        memset(g2, 0, G2_POKEMON_SIZE);
        g2[0x00] = mon.dexNum;       // species (dex number)
        g2[0x01] = 0;                // held item
        memcpy(&g2[0x02], mon.moves, 4);  // moves
        g2[0x06] = 0x00; g2[0x07] = 0x01; // OT ID placeholder
        // EXP (3 bytes BE) — calculate from level
        uint32_t exp = (mon.dexNum <= 251) ? expForLevel(mon.dexNum, mon.level) : 0;
        g2[0x08] = (uint8_t)(exp >> 16);
        g2[0x09] = (uint8_t)(exp >> 8);
        g2[0x0A] = (uint8_t)(exp);
        // Stat EXP (all zero for wild catch)
        // DVs
        g2[0x15] = mon.dvs[0];
        g2[0x16] = mon.dvs[1];
        // PP
        memcpy(&g2[0x17], mon.pp, 4);
        // Level
        g2[0x1F] = mon.level;
        // Status = 0
        // Current HP
        g2[0x22] = (uint8_t)(mon.maxHp >> 8);
        g2[0x23] = (uint8_t)(mon.maxHp);
        // Calculated stats (pre-calculated from battle engine)
        g2[0x24] = (uint8_t)(mon.maxHp >> 8); g2[0x25] = (uint8_t)(mon.maxHp);
        g2[0x26] = (uint8_t)(mon.atk >> 8);   g2[0x27] = (uint8_t)(mon.atk);
        g2[0x28] = (uint8_t)(mon.def >> 8);   g2[0x29] = (uint8_t)(mon.def);
        g2[0x2A] = (uint8_t)(mon.spd >> 8);   g2[0x2B] = (uint8_t)(mon.spd);
        g2[0x2C] = (uint8_t)(mon.spc >> 8);   g2[0x2D] = (uint8_t)(mon.spc);
        g2[0x2E] = (uint8_t)(mon.spc >> 8);   g2[0x2F] = (uint8_t)(mon.spc); // SpDef = Spc

        // Nickname (Gen 1 charset, 11 bytes with 0x50 terminator)
        uint8_t *nick = &wram_bank1[G2_WRAM_NICKNAMES + i * SAV_NAME_SIZE];
        memset(nick, SAV_STRING_TERMINATOR, SAV_NAME_SIZE);
        for (int j = 0; j < 10 && mon.nickname[j]; ++j)
            nick[j] = asciiToGen1Char(mon.nickname[j]);

        // OT name (use "MESH" as placeholder)
        uint8_t *ot = &wram_bank1[G2_WRAM_OT_NAMES + i * SAV_NAME_SIZE];
        memset(ot, SAV_STRING_TERMINATOR, SAV_NAME_SIZE);
        ot[0] = 0x8C; ot[1] = 0x84; ot[2] = 0x92; ot[3] = 0x87; // MESH

        return 0;
    }

    // ── Build a Gen1Party from WRAM (live emulator state) ─────────────
    static uint8_t buildGen1PartyFromWRAM(const uint8_t *wram_bank1,
                                          Gen1Party *out) {
        if (!wram_bank1 || !out) return 0;
        memset(out, 0, sizeof(*out));
        uint8_t count = wram_bank1[WRAM_PARTY_COUNT];
        if (count > 6) count = 6;
        out->count = count;
        memcpy(out->species, &wram_bank1[WRAM_SPECIES_LIST], 7);
        memcpy(out->mons, &wram_bank1[WRAM_POKEMON_DATA],
               count * sizeof(Gen1Pokemon));
        // Convert Gen 1 internal codes → dex numbers (engine uses dex directly)
        for (uint8_t i = 0; i < count; ++i) {
            out->species[i]      = internalToDex[out->species[i]];
            out->mons[i].species = internalToDex[out->mons[i].species];
        }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t *nickRaw = &wram_bank1[WRAM_NICKNAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (nickRaw[j] == SAV_STRING_TERMINATOR) {
                    out->nicknames[i][j] = 0; break;
                }
                out->nicknames[i][j] = (uint8_t)gen1CharToAscii(nickRaw[j]);
                if (j == 9) out->nicknames[i][10] = 0;
            }
            const uint8_t *otRaw = &wram_bank1[WRAM_OT_NAMES + i * SAV_NAME_SIZE];
            for (int j = 0; j < 10; ++j) {
                if (otRaw[j] == SAV_STRING_TERMINATOR) {
                    out->otNames[i][j] = 0; break;
                }
                out->otNames[i][j] = (uint8_t)gen1CharToAscii(otRaw[j]);
                if (j == 9) out->otNames[i][10] = 0;
            }
        }
        return count;
    }

private:
    // ── Flat SRAM offset for a permanent box slot (0-11) ──────────────
    static uint16_t boxPermanentAddr(uint8_t boxIdx) {
        if (boxIdx < 6)
            return SAV_BANK2_BOX_START + (uint16_t)boxIdx * SAV_BOX_SIZE;
        return SAV_BANK3_BOX_START + (uint16_t)(boxIdx - 6) * SAV_BOX_SIZE;
    }

    // ── Write a caught mon into a box at `boxBase` ────────────────────
    // Returns true on success (room found), false if the box is full.
    static bool writeToBox(uint8_t *sram, uint16_t boxBase,
                           uint8_t internalIdx, const CaughtMon &mon,
                           const uint8_t *trainerName, uint16_t trainerId,
                           uint32_t totalExp)
    {
        uint8_t box_count = sram[boxBase + BOX_COUNT_OFF];
        if (box_count > SAV_BOX_CAPACITY) box_count = 0;   // garbage → reset
        if (box_count >= SAV_BOX_CAPACITY) return false;

        const uint8_t i = box_count;
        sram[boxBase + BOX_COUNT_OFF]              = box_count + 1;
        sram[boxBase + BOX_SPECIES_OFF + i]        = internalIdx;
        sram[boxBase + BOX_SPECIES_OFF + i + 1]    = 0xFF;  // re-terminate

        uint8_t *bm = &sram[boxBase + BOX_POKE_OFF + i * SAV_BOX_POKE_SIZE];
        memset(bm, 0, SAV_BOX_POKE_SIZE);
        bm[PKM_SPECIES]     = internalIdx;
        writeBE16(&bm[PKM_CURRENT_HP], mon.hp ? mon.hp : mon.maxHp);
        bm[PKM_LEVEL_PC]    = mon.level;
        bm[PKM_STATUS]      = 0;
        bm[PKM_TYPE1]       = mon.type1;
        bm[PKM_TYPE2]       = mon.type2;
        bm[7]               = 45;
        memcpy(&bm[8], mon.moves, 4);
        writeBE16(&bm[12], trainerId);
        writeBE24(&bm[PKM_EXP], totalExp);
        bm[PKM_DVS]         = mon.dvs[0];
        bm[PKM_DVS + 1]     = mon.dvs[1];
        memcpy(&bm[29], mon.pp, 4);

        memcpy(&sram[boxBase + BOX_OT_OFF  + i * SAV_NAME_SIZE],
               trainerName, SAV_NAME_SIZE);
        writeNickname(&sram[boxBase + BOX_NICK_OFF + i * SAV_NAME_SIZE],
                      mon.nickname);
        return true;
    }

    // ── Encode an ASCII nickname (up to 10 chars) into Gen 1 charset ───
    // Always writes a full 11-byte buffer: encoded chars followed by a
    // 0x50 terminator and 0x50 padding for any unused slots. Used by the
    // catch flow to populate party + box nickname rows.
    static void writeNickname(uint8_t *dst, const char *ascii) {
        // Pre-fill with the terminator so trailing bytes are clean.
        memset(dst, SAV_STRING_TERMINATOR, SAV_NAME_SIZE);
        if (!ascii) return;
        int j = 0;
        for (; j < 10 && ascii[j]; ++j) {
            dst[j] = asciiToGen1Char(ascii[j]);
        }
        dst[j] = SAV_STRING_TERMINATOR;
    }

    // ── Recalculate all 5 stats for a party Pokemon ────────────────────
    static void recalcStats(uint8_t *pkm, uint8_t dexNum, uint8_t level) {
        if (dexNum == 0 || dexNum > 151) return;
        const BaseStats &base = speciesBaseStats[dexNum];

        // Read DVs
        uint8_t atkDV = (pkm[PKM_DVS] >> 4) & 0x0F;
        uint8_t defDV = pkm[PKM_DVS] & 0x0F;
        uint8_t spdDV = (pkm[PKM_DVS + 1] >> 4) & 0x0F;
        uint8_t spcDV = pkm[PKM_DVS + 1] & 0x0F;
        uint8_t hpDV  = ((atkDV & 1) << 3) | ((defDV & 1) << 2) |
                        ((spdDV & 1) << 1) | (spcDV & 1);

        // Read stat experience
        uint16_t hpEV  = readBE16(&pkm[PKM_HP_EV]);
        uint16_t atkEV = readBE16(&pkm[PKM_ATK_EV]);
        uint16_t defEV = readBE16(&pkm[PKM_DEF_EV]);
        uint16_t spdEV = readBE16(&pkm[PKM_SPD_EV]);
        uint16_t spcEV = readBE16(&pkm[PKM_SPC_EV]);

        // Calculate new stats
        uint16_t maxHP  = calcStatHP(base.hp, hpDV, hpEV, level);
        uint16_t attack = calcStat(base.atk, atkDV, atkEV, level);
        uint16_t defense = calcStat(base.def, defDV, defEV, level);
        uint16_t speed   = calcStat(base.spd, spdDV, spdEV, level);
        uint16_t special = calcStat(base.spc, spcDV, spcEV, level);

        // Read old max HP and current HP to scale proportionally
        uint16_t oldMaxHP = readBE16(&pkm[PKM_MAX_HP]);
        uint16_t oldCurHP = readBE16(&pkm[PKM_CURRENT_HP]);

        // Scale current HP proportionally to new max HP
        uint16_t newCurHP;
        if (oldMaxHP > 0 && oldCurHP > 0) {
            newCurHP = (uint16_t)((uint32_t)oldCurHP * maxHP / oldMaxHP);
            if (newCurHP == 0) newCurHP = 1;  // don't kill the Pokemon
            if (newCurHP > maxHP) newCurHP = maxHP;
        } else {
            newCurHP = maxHP;  // full heal if data was zeroed
        }

        // Write stats
        writeBE16(&pkm[PKM_CURRENT_HP], newCurHP);
        writeBE16(&pkm[PKM_MAX_HP], maxHP);
        writeBE16(&pkm[PKM_ATTACK], attack);
        writeBE16(&pkm[PKM_DEFENSE], defense);
        writeBE16(&pkm[PKM_SPEED], speed);
        writeBE16(&pkm[PKM_SPECIAL], special);
    }
};
