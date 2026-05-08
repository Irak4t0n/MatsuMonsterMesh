// SPDX-License-Identifier: MIT
// See LordSave.h.

#include "LordSave.h"

// Originally talked to Meshtastic's FSCom (LittleFS) and gps/RTC. On Tanmatsu
// we go through ESP-IDF VFS (fopen/fwrite) and POSIX time().
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static constexpr const char *LORD_PATH = "/monstermesh/lord.dat";

void lordInitDefaults(LordSave &s)
{
    memset(&s, 0, sizeof(s));
    s.magic   = LORD_MAGIC;
    s.version = LORD_VERSION;
}

bool lordLoad(LordSave &s)
{
    lordInitDefaults(s);

    FILE *f = fopen(LORD_PATH, "rb");
    if (!f) return false;

    LordSave tmp;
    size_t n = fread(&tmp, 1, sizeof(tmp), f);
    fclose(f);

    if (n != sizeof(tmp))             return false;
    if (tmp.magic   != LORD_MAGIC)    return false;
    if (tmp.version != LORD_VERSION)  return false;

    s = tmp;
    return true;
}

bool lordSave(const LordSave &s)
{
    // Ensure containing directory exists.
    mkdir("/monstermesh", 0775);   // ignore EEXIST

    FILE *f = fopen(LORD_PATH, "wb");
    if (!f) return false;

    size_t n = fwrite(&s, 1, sizeof(s), f);
    fflush(f);
    fclose(f);
    return n == sizeof(s);
}

void lordAppendNews(LordSave &s, uint8_t type, uint8_t arg1, uint16_t arg2)
{
    LordNewsEntry &e = s.news[s.newsHead];
    // Was Meshtastic's getTime() (gps/RTC.h). POSIX time() returns 0 if the
    // RTC hasn't been set yet; the news ring tolerates that — entries just
    // sort by their arrival order in the buffer rather than wallclock.
    e.ts       = (uint32_t)time(NULL);
    e.type     = type;
    e.arg1     = arg1;
    e.arg2     = arg2;
    e.reserved = 0;

    s.newsHead = (uint8_t)((s.newsHead + 1) % LORD_NEWS_CAP);
    if (s.newsCount < LORD_NEWS_CAP) s.newsCount++;
}
