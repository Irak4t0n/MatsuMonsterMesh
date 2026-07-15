// SPDX-License-Identifier: MIT
// See LordRoutes.h.

#include "LordRoutes.h"

#include "Gen1BattleEngine.h"
#include "DaycareData.h"          // daycareSpeciesNames[]
#include "showdown_gen1_moves.h"

#include <esp_random.h>
#include <string.h>
#include <stdio.h>

// ── Route 0: Viridian Forest / Route 1-2 (pre-Pewter, pre-Brock) ────────────
static const LordRouteEncounter kRoute0[] = {
    {  16, 3, 6,  { 33, 45,  0, 0 }, 10 },  // Pidgey
    {  19, 2, 4,  { 33, 39,  0, 0 }, 10 },  // Rattata
    {  10, 3, 5,  { 33, 81,  0, 0 },  8 },  // Caterpie
    {  13, 3, 5,  { 40, 81,  0, 0 },  8 },  // Weedle
    {  25, 3, 5,  { 33, 84,  0, 0 },  1 },  // Pikachu (rare)
};

// ── Route 1: Mt Moon / Routes 3-4 (post-Brock, pre-Misty) ───────────────────
static const LordRouteEncounter kRoute1[] = {
    {  41, 7,11,  { 44, 100, 0, 0 }, 10 },  // Zubat
    {  74, 7,10,  { 33, 88,  0, 0 },  9 },  // Geodude
    {  46, 8,10,  { 10, 77,  0, 0 },  6 },  // Paras
    {  35, 8,11,  { 39, 47,  0, 0 },  3 },  // Clefairy (rare)
    {  39, 8,11,  { 33, 47,  0, 0 },  2 },  // Jigglypuff (rare)
};

// ── Route 2: Routes 5-6, Diglett's Cave (post-Misty, pre-Surge) ─────────────
static const LordRouteEncounter kRoute2[] = {
    {  50,15,19,  { 10, 28,  0, 0 }, 10 },  // Diglett
    {  56,11,15,  { 10, 43,  0, 0 },  9 },  // Mankey
    {  17,12,15,  { 16, 98,  0, 0 },  8 },  // Pidgeotto
    {  52,12,14,  { 10, 39,  0, 0 },  6 },  // Meowth
    {  21,11,14,  { 64, 39,  0, 0 },  5 },  // Spearow
};

// ── Route 3: Rock Tunnel / Routes 8-9 (post-Surge, pre-Erika) ───────────────
static const LordRouteEncounter kRoute3[] = {
    {  95,17,22,  { 88, 20,  0, 0 }, 10 },  // Onix
    {  74,17,21,  { 33, 88,  0, 0 }, 10 },  // Geodude
    {  66,17,21,  { 10, 70,  0, 0 },  9 },  // Machop
    { 115,18,22,  { 33, 23,  0, 0 },  3 },  // Kangaskhan (rare)
    { 100,17,21,  { 33, 84,  0, 0 },  4 },  // Voltorb
};

// ── Route 4: Routes 12-15, Cycling Road (post-Erika, pre-Koga) ──────────────
static const LordRouteEncounter kRoute4[] = {
    {  69,21,24,  { 22, 71,  0, 0 }, 10 },  // Bellsprout
    {  43,22,25,  { 71, 77,  0, 0 }, 10 },  // Oddish
    {  84,21,25,  { 64, 33,  0, 0 },  9 },  // Doduo
    {  48,22,24,  { 33, 79,  0, 0 },  8 },  // Venonat
    {  70,23,26,  { 22, 71,  0, 0 },  4 },  // Weepinbell (rare)
};

// ── Route 5: Pokemon Tower / Safari Zone (post-Koga, pre-Sabrina) ───────────
static const LordRouteEncounter kRoute5[] = {
    {  92,25,28,  {122, 50,  0, 0 }, 10 },  // Gastly
    {  93,28,32,  {122, 50,  0, 0 },  6 },  // Haunter
    {  29,24,28,  { 33, 39,  0, 0 },  9 },  // Nidoran-F
    {  32,24,28,  { 33, 43,  0, 0 },  9 },  // Nidoran-M
    { 113,30,33,  { 39, 47,  0, 0 },  1 },  // Chansey (super rare)
};

// ── Route 6: Routes 16-21 / Seafoam Islands (post-Sabrina, pre-Blaine) ──────
static const LordRouteEncounter kRoute6[] = {
    {  84,29,32,  { 64, 33,  0, 0 }, 10 },  // Doduo
    {  22,30,34,  { 64, 19,  0, 0 },  8 },  // Fearow
    {  17,29,33,  { 16, 98,  0, 0 },  8 },  // Pidgeotto
    {  98,30,32,  { 33, 55,  0, 0 },  9 },  // Krabby
    {  86,30,33,  { 71, 62,  0, 0 },  8 },  // Seel
};

// ── Route 7: Pokemon Mansion / Cerulean Cave (post-Blaine, pre-Giovanni) ────
static const LordRouteEncounter kRoute7[] = {
    { 126,32,36,  { 52, 33,  0, 0 }, 10 },  // Magmar
    {  77,32,36,  { 52, 36,  0, 0 },  9 },  // Ponyta
    {  88,31,35,  { 33, 51,  0, 0 },  9 },  // Grimer
    { 109,32,36,  { 33, 51,  0, 0 },  9 },  // Koffing
    { 150,40,42,  { 94, 60,  0, 0 },  1 },  // Mewtwo (legendary)
};

static const LordRoute kRoutes[8] = {
    { "Viridian Forest", kRoute0, sizeof(kRoute0)/sizeof(kRoute0[0]) },
    { "Mt. Moon",        kRoute1, sizeof(kRoute1)/sizeof(kRoute1[0]) },
    { "Diglett's Cave",  kRoute2, sizeof(kRoute2)/sizeof(kRoute2[0]) },
    { "Rock Tunnel",     kRoute3, sizeof(kRoute3)/sizeof(kRoute3[0]) },
    { "Cycling Road",    kRoute4, sizeof(kRoute4)/sizeof(kRoute4[0]) },
    { "Safari Zone",     kRoute5, sizeof(kRoute5)/sizeof(kRoute5[0]) },
    { "Seafoam Islands", kRoute6, sizeof(kRoute6)/sizeof(kRoute6[0]) },
    { "Cerulean Cave",   kRoute7, sizeof(kRoute7)/sizeof(kRoute7[0]) },
};

const LordRoute *lordRoute(uint8_t idx)
{
    return idx < 8 ? &kRoutes[idx] : nullptr;
}

bool lordPickWildEncounter(uint8_t routeIdx, Gen1Party &out)
{
    const LordRoute *r = lordRoute(routeIdx);
    if (!r || !r->pool || r->poolCount == 0) return false;

    uint16_t total = 0;
    for (uint8_t i = 0; i < r->poolCount; ++i) total += r->pool[i].weight;
    if (total == 0) return false;

    uint16_t roll = (uint16_t)(esp_random() % total);
    const LordRouteEncounter *pick = &r->pool[0];
    uint16_t acc = 0;
    for (uint8_t i = 0; i < r->poolCount; ++i) {
        acc += r->pool[i].weight;
        if (roll < acc) { pick = &r->pool[i]; break; }
    }

    uint8_t span = pick->levelMax >= pick->levelMin
                       ? (uint8_t)(pick->levelMax - pick->levelMin + 1)
                       : 1;
    uint8_t level = (uint8_t)(pick->levelMin + (esp_random() % span));

    memset(&out, 0, sizeof(out));
    out.count = 1;

    Gen1BattleEngine::BattlePoke tmp;
    Gen1BattleEngine::initBattlePokeFromBase(tmp, pick->species, level, pick->moves);

    auto setBe16 = [](uint8_t *dst, uint16_t v) {
        dst[0] = (uint8_t)(v >> 8); dst[1] = (uint8_t)v;
    };

    Gen1Pokemon &p = out.mons[0];
    memset(&p, 0, sizeof(p));
    p.species  = pick->species;   // dex number (our convention)
    p.boxLevel = level;
    p.level    = level;
    setBe16(p.maxHp, tmp.maxHp);
    setBe16(p.hp,    tmp.hp);
    setBe16(p.atk,   tmp.atk);
    setBe16(p.def,   tmp.def);
    setBe16(p.spd,   tmp.spd);
    setBe16(p.spc,   tmp.spc);
    p.type1 = tmp.type1;
    p.type2 = tmp.type2;
    p.dvs[0] = 0x88;
    p.dvs[1] = 0x88;
    memcpy(p.moves, pick->moves, 4);
    for (int i = 0; i < 4; ++i) {
        const Gen1MoveData *m = gen1Move(pick->moves[i]);
        p.pp[i] = m ? m->pp : 0;
    }
    out.species[0] = pick->species;
    const char *name = (pick->species >= 1 && pick->species <= 151)
                           ? daycareSpeciesNames[pick->species] : "???";
    snprintf((char *)out.nicknames[0], 11, "%s", name);
    return true;
}
