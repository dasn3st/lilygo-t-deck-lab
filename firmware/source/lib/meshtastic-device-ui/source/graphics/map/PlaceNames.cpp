// Offline place-name search - see PlaceNames.h for the data, where it comes from, and the rules.
//
// File format (TNM1), all little-endian - written by placenames/build_names.py:
//   header, 32 bytes : "TNM1", u32 recordCount, u32 indexCount, u32 recordsOffset (32),
//                      u32 indexOffset, i32 cellLat, i32 cellLon (-999 = cities.tnm), u32 flags
//                      (bit 0 RANKED: records carry a population rank and are stored biggest first)
//   records          : i32 lat*1e6, i32 lon*1e6, u8 kind, [u8 rank if RANKED], u8 nameLen,
//                      name (UTF-8), u8 keyLen, key - the name folded to lowercase ASCII words; a big
//                      place's other names follow as extra '|' segments ("munich|munchen|muenchen")
//   index            : 16-byte entries sorted by word: char word[12] zero-padded, u32 record offset
//
// A search is two searches (placenames/search_names.py is the reference, step for step):
//   NEARBY  - pick the typed word the fewest index entries start with, walk just that slice of the
//             index, check each record against ALL the typed words. The square under the map's
//             centre first, then the ring around it, then the next - until there are enough hits.
//   CITIES  - suggestions from cities.tnm, "san" -> San Jose, San Francisco, San Diego (Jake,
//             2026-09-30). That file is biggest-first, so reading its top few thousand records
//             finds the big ones whatever they are called; its index finds the smaller ones.

#include "graphics/map/PlaceNames.h"

#include <math.h>
#include <string.h>

namespace {
const char *const kKinds[] = {"place",  "town",    "lake",     "reservoir", "pond",   "river",   "spring",
                              "peak",   "ridge",   "valley",   "pass",      "flat",   "island",  "bay",
                              "cape",   "swamp",   "falls",    "glacier",   "park",   "forest",  "beach",
                              "canal",  "area",    "camp",     "airport",   "station", "landmark", "trail",
                              "basin",  "cliff",   "rapids",   "crossing",  "military", "range", "city"};
const uint8_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
const uint8_t kKindTown = 1, kKindCity = 34;
} // namespace

const char *placenames_kind_name(uint8_t kind)
{
    return kind < kKindCount ? kKinds[kind] : "place";
}

uint8_t placenames_kind_zoom(uint8_t kind)
{
    const char *k = placenames_kind_name(kind);
    if (!strcmp(k, "city"))
        return 11;
    if (!strcmp(k, "range"))
        return 10;
    if (!strcmp(k, "town") || !strcmp(k, "park") || !strcmp(k, "forest") || !strcmp(k, "island") ||
        !strcmp(k, "bay") || !strcmp(k, "glacier") || !strcmp(k, "military") || !strcmp(k, "area"))
        return 13;
    if (!strcmp(k, "lake") || !strcmp(k, "reservoir") || !strcmp(k, "river") || !strcmp(k, "valley") ||
        !strcmp(k, "basin") || !strcmp(k, "airport") || !strcmp(k, "swamp"))
        return 14;
    return 15; // peaks, springs, falls, camps, trailheads: things you walk to
}

#if defined(HAS_SDCARD) && !defined(HAS_SD_MMC) && !defined(ARCH_PORTDUINO)

#include "graphics/common/SdCard.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {

// Words that are never looked up on their own - the builder leaves them out of the index. Same list
// as STOP in build_names.py; if one changes, so must the other.
const char *const kStop[] = {"the", "of", "de", "la", "le", "les", "du",  "des", "di",  "del", "della", "der", "die", "das",
                             "am",  "im", "an", "en", "y",  "et",  "and", "von", "van", "da",  "do",    "dos", "al",  "el",
                             "lo",  "los", "las", "st"};

// The same numbers as search_names.py.
const uint32_t kWalkCap = 250;    // index entries walked per file - bounds the worst case
const uint32_t kTopCities = 4000; // cities.tnm records read from the top (roughly 100,000+ people)
const uint8_t kRankBig = 125;     // 100,000 people: a city this big may go above the nearby results
const int kMaxNear = 9, kMaxCities = 4;

bool isStop(const char *w)
{
    for (const char *s : kStop)
        if (!strcmp(s, w))
            return true;
    return false;
}

struct Query {
    static const int kMaxWords = 6;
    char words[kMaxWords][24];
    int n;
    int letters;    // typed letters and digits, all words together
    char full[160]; // the words joined by single spaces, for "is this EXACTLY what was typed"
};

// What was typed, folded the way build_names.py folds names: lowercase ASCII words; an apostrophe
// vanishes ("O'Brien" -> "obrien"), anything else that is not a letter or digit splits words.
void parseQuery(const char *q, Query &out)
{
    out.n = 0;
    out.letters = 0;
    out.full[0] = 0;
    int len = 0;
    for (const char *p = q; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c == '\'')
            continue;
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            if (out.n < Query::kMaxWords && len < (int)sizeof(out.words[0]) - 1)
                out.words[out.n][len++] = c;
        } else if (len) {
            out.words[out.n][len] = 0;
            out.n++;
            len = 0;
        }
    }
    if (len && out.n < Query::kMaxWords) {
        out.words[out.n][len] = 0;
        out.n++;
    }
    size_t fl = 0;
    for (int i = 0; i < out.n; i++) {
        const size_t wl = strlen(out.words[i]);
        out.letters += (int)wl;
        if (fl + wl + 2 >= sizeof(out.full))
            break;
        if (i)
            out.full[fl++] = ' ';
        memcpy(out.full + fl, out.words[i], wl);
        fl += wl;
        out.full[fl] = 0;
    }
}

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct Record {
    float lat, lon;
    uint8_t kind;
    uint8_t rank;
    char name[64];
    char key[256];
};

const uint32_t kMaxRecord = 10 + 1 + 255 + 1 + 255; // lat lon kind rank | nameLen name | keyLen key

// One record out of a buffer. Returns the bytes it took, or 0 when the buffer ends inside it.
uint32_t parseRecord(const uint8_t *b, uint32_t n, bool ranked, Record &r)
{
    uint32_t p = 9 + (ranked ? 1 : 0);
    if (n < p + 1)
        return 0;
    r.lat = (int32_t)rd32(b) / 1e6f;
    r.lon = (int32_t)rd32(b + 4) / 1e6f;
    r.kind = b[8];
    r.rank = ranked ? b[9] : 0;
    const uint32_t nl = b[p++];
    if (n < p + nl + 1)
        return 0;
    const uint32_t keep = nl < sizeof(r.name) - 1 ? nl : sizeof(r.name) - 1;
    memcpy(r.name, b + p, keep);
    r.name[keep] = 0;
    p += nl;
    const uint32_t kl = b[p++];
    if (n < p + kl)
        return 0;
    memcpy(r.key, b + p, kl);
    r.key[kl] = 0;
    return p + kl;
}

struct Tnm {
    FsFile f;
    uint32_t nrec = 0, nidx = 0, recoff = 0, idxoff = 0;
    bool ranked = false;
    uint16_t reads = 0;

    bool open(const char *path)
    {
        f = SDFs.open(path, O_RDONLY);
        if (!f)
            return false;
        uint8_t h[32];
        if (f.read(h, 32) != 32 || memcmp(h, "TNM1", 4) != 0) {
            f.close();
            return false;
        }
        nrec = rd32(h + 4);
        nidx = rd32(h + 8);
        recoff = rd32(h + 12);
        idxoff = rd32(h + 16);
        ranked = (rd32(h + 28) & 1) != 0;
        return true;
    }
    void close() { f.close(); }

    bool entry(uint32_t i, uint8_t *e16)
    {
        reads++;
        return f.seekSet(idxoff + 16ull * i) && f.read(e16, 16) == 16;
    }

    // First index entry whose word is >= the prefix. An entry that merely STARTS with the prefix
    // counts as >= it, which is exactly what makes [lower(p), lower(p+1)) the "starts with p" slice.
    uint32_t lower(const uint8_t *p, size_t plen)
    {
        uint32_t lo = 0, hi = nidx;
        uint8_t e[16];
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (!entry(mid, e))
                return lo; // a read error ends the search here rather than looping on garbage
            if (memcmp(e, p, plen) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    void range(const char *w, uint32_t &a, uint32_t &b)
    {
        uint8_t p[12];
        size_t plen = strlen(w);
        if (plen > 12)
            plen = 12; // the index keeps 12 characters; the record check sees the rest
        memcpy(p, w, plen);
        a = lower(p, plen);
        p[plen - 1]++; // "sea" -> "seb": everything starting "sea" sorts before it
        b = lower(p, plen);
    }

    bool record(uint32_t off, Record &r)
    {
        reads++;
        uint8_t buf[kMaxRecord];
        if (!f.seekSet(off))
            return false;
        const int n = f.read(buf, sizeof(buf));
        return n > 0 && parseRecord(buf, (uint32_t)n, ranked, r) != 0;
    }
};

// Does the place answer what was typed? Its own name must contain every typed word, each as the
// start of a word, in any order ("washington lake" finds Lake Washington). Its OTHER names ('|'
// segments after the first) count only from 4 typed letters, and only read from their first word
// in order: GeoNames alternates are full of oddities - "sari" for Surrey, "fi sang" for
// Philadelphia - that otherwise turned up as suggestions for "sa" and "san f".
bool matches(const char *key, const Query &q)
{
    const char *seg = key;
    for (int segNo = 0;; segNo++) {
        const char *end = strchr(seg, '|');
        if (!end)
            end = seg + strlen(seg);
        bool all = true;
        if (segNo == 0) {
            for (int i = 0; i < q.n && all; i++) {
                const size_t wl = strlen(q.words[i]);
                bool found = false;
                for (const char *p = seg; p < end && !found; p++) {
                    if (p != seg && p[-1] != ' ')
                        continue; // only at the start of a word
                    if (p + wl <= end && !strncmp(p, q.words[i], wl))
                        found = true;
                }
                all = found;
            }
        } else {
            if (q.letters < 4)
                return false;
            const char *p = seg;
            for (int i = 0; i < q.n && all; i++) {
                const size_t wl = strlen(q.words[i]);
                if (p >= end || p + wl > end || strncmp(p, q.words[i], wl) != 0) {
                    all = false;
                    break;
                }
                while (p < end && *p != ' ') // on to the next word of this name
                    p++;
                if (p < end)
                    p++;
            }
        }
        if (all)
            return true;
        if (!*end)
            return false;
        seg = end + 1;
    }
}

bool isExactName(const char *key, const char *full)
{
    const size_t fl = strlen(full);
    const char *seg = key;
    for (;;) {
        const char *end = strchr(seg, '|');
        if (!end)
            end = seg + strlen(seg);
        if ((size_t)(end - seg) == fl && !strncmp(seg, full, fl))
            return true;
        if (!*end)
            return false;
        seg = end + 1;
    }
}

float distKm(float la1, float lo1, float la2, float lo2)
{
    const float d2r = (float)M_PI / 180.0f;
    const float x = (lo2 - lo1) * d2r * cosf((la1 + la2) * 0.5f * d2r);
    const float y = (la2 - la1) * d2r;
    return 6371.0f * sqrtf(x * x + y * y);
}

// The best hits so far, kept sorted, lower score first. Small on purpose: the screen shows a dozen.
struct Hits {
    static const int kKeep = 24;
    PlaceHit h[kKeep];
    int n;
    int raw; // matches before de-duplication - what decides whether to search the next ring

    void clear() { n = raw = 0; }

    void add(const Record &r, float km, float score, uint8_t group, bool exact)
    {
        raw++;
        // The same place can be reached through more than one of its names, or be in two files;
        // same name and the same spot to about 100m is the same place.
        for (int i = 0; i < n; i++) {
            if (!strcmp(h[i].name, r.name) && fabsf(h[i].lat - r.lat) < 0.001f && fabsf(h[i].lon - r.lon) < 0.001f) {
                if (score < h[i].score) {
                    h[i].score = score;
                    resort(i);
                }
                return;
            }
        }
        if (n == kKeep && score >= h[n - 1].score)
            return;
        int at = n < kKeep ? n++ : kKeep - 1;
        PlaceHit &p = h[at];
        p.lat = r.lat;
        p.lon = r.lon;
        p.kind = r.kind;
        p.rank = r.rank;
        p.group = group;
        p.exact = exact;
        strncpy(p.name, r.name, sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = 0;
        p.score = score;
        p.km = km;
        resort(at);
    }
    void resort(int i)
    {
        while (i > 0 && h[i].score < h[i - 1].score) {
            PlaceHit t = h[i];
            h[i] = h[i - 1];
            h[i - 1] = t;
            i--;
        }
    }
    void remove(int i)
    {
        for (int j = i; j + 1 < n; j++)
            h[j] = h[j + 1];
        n--;
    }
};

// Everything a search works in, ~10KB, taken from PSRAM on the first search and kept. As static
// arrays it sat in internal RAM, which is the memory this device actually runs out of.
struct Work {
    Query q;
    Hits near, cities;
    Record r;
    uint32_t seen[kWalkCap];
    uint8_t scan[4096 + kMaxRecord];
};
Work *s_work = nullptr;

bool late(uint32_t deadline)
{
    return (int32_t)(millis() - deadline) > 0;
}

void scoreNear(const Record &r, float clat, float clon, const Query &q, Hits &hits)
{
    // Nearest first - but a name that IS what you typed beats a longer name that merely contains
    // it, and a town or city beats a creek named after it. Divided, so a far exact match still
    // loses to a very close one.
    const float km = distKm(clat, clon, r.lat, r.lon);
    const bool exact = isExactName(r.key, q.full);
    float score = km + 0.5f;
    if (exact)
        score /= 8;
    if (r.kind == kKindTown || r.kind == kKindCity)
        score /= 3;
    hits.add(r, km, score, 0, exact);
}

void scoreCity(const Record &r, float clat, float clon, const Query &q, Hits &hits)
{
    // Bigger and closer first: ten times the people outweighs 5,000 km. Stored negated, because
    // Hits keeps the LOWEST score first.
    const float km = distKm(clat, clon, r.lat, r.lon);
    hits.add(r, km, km / 200.0f - (float)r.rank, 1, isExactName(r.key, q.full));
}

typedef void (*ScoreFn)(const Record &, float, float, const Query &, Hits &);

// Walk the index slice of the most selective typed word, checking each record it points at.
void walkIndex(Tnm &t, const Query &q, float clat, float clon, Hits &hits, ScoreFn score, uint32_t deadline,
               bool &wasLate)
{
    uint32_t bestA = 0, bestB = 0;
    bool have = false, anyReal = false;
    for (int i = 0; i < q.n; i++)
        if (!isStop(q.words[i]))
            anyReal = true;
    for (int i = 0; i < q.n; i++) {
        if (anyReal && isStop(q.words[i]))
            continue;
        uint32_t a, b;
        t.range(q.words[i], a, b);
        if (!have || b - a < bestB - bestA) {
            bestA = a;
            bestB = b;
            have = true;
        }
    }
    if (!have || bestA >= bestB)
        return;

    uint32_t *seen = s_work->seen;
    uint32_t nSeen = 0;
    const uint32_t stop = bestB - bestA > kWalkCap ? bestA + kWalkCap : bestB;
    Record &r = s_work->r;
    uint8_t batch[16 * 32];
    for (uint32_t i = bestA; i < stop;) {
        // Read index entries 32 at a time: one card read instead of 32, and the records they
        // point at are read in between.
        const uint32_t take = stop - i < 32 ? stop - i : 32;
        t.reads++;
        if (!t.f.seekSet(t.idxoff + 16ull * i) || t.f.read(batch, 16 * take) != (int)(16 * take))
            return;
        for (uint32_t j = 0; j < take; j++) {
            const uint32_t off = rd32(batch + 16 * j + 12);
            bool dup = false;
            for (uint32_t s = 0; s < nSeen && !dup; s++)
                dup = (seen[s] == off);
            if (dup)
                continue;
            seen[nSeen++] = off;
            if (t.record(off, r) && matches(r.key, q))
                score(r, clat, clon, q, hits);
        }
        i += take;
        if (late(deadline)) {
            wasLate = true;
            return;
        }
    }
}

// The top of cities.tnm, held in PSRAM while Maps is open. Reading it off the card was the one fixed
// cost of every search - half a second even for "xq", measured on the device 2026-09-30. ~260KB,
// loaded by the first search, handed back by placenames_release() when Maps closes.
struct TopCache {
    uint8_t *buf;
    uint32_t len;          // bytes of whole records in buf
    uint32_t nrec, idxoff; // which file it came from: a newly downloaded cities.tnm is re-read
};
TopCache s_top{};
const uint32_t kTopBytes = 300 * 1024;

bool topLoad(Tnm &t)
{
    if (s_top.buf && s_top.nrec == t.nrec && s_top.idxoff == t.idxoff)
        return true;
    if (s_top.buf) {
        heap_caps_free(s_top.buf);
        s_top.buf = nullptr;
    }
    // Not at the cost of everything else: the device keeps 512KB of PSRAM spare (see chess, tiles).
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kTopBytes + 768u * 1024u)
        return false;
    uint32_t want = t.idxoff - t.recoff;
    if (want > kTopBytes)
        want = kTopBytes;
    uint8_t *b = (uint8_t *)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (!b)
        return false;
    uint32_t got = 0;
    if (t.f.seekSet(t.recoff)) {
        while (got < want) {
            const uint32_t chunk = want - got > 32768 ? 32768 : want - got;
            const int r = t.f.read(b + got, chunk);
            t.reads++;
            if (r <= 0)
                break;
            got += (uint32_t)r;
        }
    }
    if (got != want) { // a short read is a failed read: never keep half of it (cached-failed-read lesson)
        heap_caps_free(b);
        return false;
    }
    // Keep only whole records, up to kTopCities of them.
    Record &r = s_work->r;
    uint32_t at = 0;
    for (uint32_t i = 0; i < kTopCities; i++) {
        const uint32_t used = parseRecord(b + at, want - at, t.ranked, r);
        if (!used)
            break;
        at += used;
    }
    s_top = {b, at, t.nrec, t.idxoff};
    return true;
}

// Read the first n records of a biggest-first file straight through - from the PSRAM copy when
// there is one, otherwise off the card in 4KB reads - and keep the ones that match.
void scanTop(Tnm &t, const Query &q, uint32_t n, float clat, float clon, Hits &hits, uint32_t deadline)
{
    if (topLoad(t)) {
        Record &r = s_work->r;
        uint32_t at = 0;
        while (at < s_top.len) {
            const uint32_t used = parseRecord(s_top.buf + at, s_top.len - at, t.ranked, r);
            if (!used)
                break;
            at += used;
            if (matches(r.key, q))
                scoreCity(r, clat, clon, q, hits);
        }
        return;
    }

    uint8_t *buf = s_work->scan;
    const uint32_t cap = sizeof(s_work->scan);
    uint32_t have = 0, at = 0, filePos = t.recoff; // filePos = where buf[0] came from
    if (!t.f.seekSet(t.recoff))
        return;
    Record &r = s_work->r;
    for (uint32_t i = 0; i < n && i < t.nrec; i++) {
        if (have - at < kMaxRecord && filePos + have < t.idxoff) { // top up, keeping what is unread
            memmove(buf, buf + at, have - at);
            filePos += at;
            have -= at;
            at = 0;
            uint32_t want = cap - have;
            if (want > t.idxoff - (filePos + have))
                want = t.idxoff - (filePos + have);
            const int got = t.f.read(buf + have, want);
            t.reads++;
            if (got <= 0)
                return;
            have += (uint32_t)got;
        }
        const uint32_t used = parseRecord(buf + at, have - at, t.ranked, r);
        if (!used)
            return;
        at += used;
        if (matches(r.key, q))
            scoreCity(r, clat, clon, q, hits);
        if ((i & 255) == 255 && late(deadline))
            return;
    }
}

} // namespace

bool placenames_on_card(void)
{
    return SDFs.exists("/names");
}

void placenames_release(void)
{
    if (s_top.buf) {
        heap_caps_free(s_top.buf);
        s_top = {};
    }
}

int placenames_search(const char *query, float lat, float lon, PlaceHit *out, int maxOut, uint32_t budgetMs,
                      PlaceSearchInfo *info)
{
    PlaceSearchInfo scratch;
    PlaceSearchInfo &inf = info ? *info : scratch;
    memset(&inf, 0, sizeof(inf));
    const uint32_t t0 = millis();

    if (!s_work) {
        s_work = (Work *)heap_caps_malloc(sizeof(Work), MALLOC_CAP_SPIRAM);
        if (!s_work)
            return 0;
    }
    Query &q = s_work->q;
    parseQuery(query ? query : "", q);
    if (!q.n || maxOut <= 0)
        return 0;

    Hits &near = s_work->near;
    Hits &cities = s_work->cities;
    near.clear();
    cities.clear();
    bool wasLate = false;
    const uint32_t deadline = t0 + budgetMs;
    char path[40];
    Tnm t;

    // Nearby: from three letters. Two match half the map and say nothing about where you are.
    if (q.letters >= 3) {
        const int c0 = (int)floorf(lat), c1 = (int)floorf(lon);
        for (int ring = 0; ring <= 2 && !wasLate; ring++) {
            for (int dla = -ring; dla <= ring && !wasLate; dla++) {
                for (int dlo = -ring; dlo <= ring && !wasLate; dlo++) {
                    if ((dla < 0 ? -dla : dla) != ring && (dlo < 0 ? -dlo : dlo) != ring)
                        continue; // inside this ring: already searched
                    int la = c0 + dla, lo = c1 + dlo;
                    if (la < -90 || la > 89)
                        continue;
                    if (lo < -180)
                        lo += 360;
                    if (lo > 179)
                        lo -= 360;
                    snprintf(path, sizeof(path), "/names/%d/%d.tnm", la, lo);
                    if (!t.open(path))
                        continue;
                    inf.files++;
                    if (ring == 0)
                        inf.haveHere = true;
                    walkIndex(t, q, lat, lon, near, scoreNear, deadline, wasLate);
                    inf.reads += t.reads;
                    t.reads = 0;
                    t.close();
                }
            }
            if (near.raw >= 12)
                break;
        }
    }
    inf.truncated = wasLate;

    // Cities: from two letters. Its own small allowance, so running out of time nearby does not
    // also lose the suggestions.
    if (q.letters >= 2 && t.open("/names/cities.tnm")) {
        inf.haveCities = true;
        inf.files++;
        const uint32_t cdl = millis() + 600;
        bool late2 = false;
        if (t.ranked)
            scanTop(t, q, kTopCities, lat, lon, cities, cdl);
        walkIndex(t, q, lat, lon, cities, scoreCity, cdl, late2);
        inf.reads += t.reads;
        t.reads = 0;
        t.close();
    }

    // A city already in the nearby list is not suggested twice - it is marked a city there instead.
    const int nNear = near.n < kMaxNear ? near.n : kMaxNear;
    for (int i = 0; i < nNear; i++) {
        for (int j = 0; j < cities.n; j++) {
            const PlaceHit &c = cities.h[j];
            if (!strcmp(c.name, near.h[i].name) && fabsf(c.lat - near.h[i].lat) < 0.01f &&
                fabsf(c.lon - near.h[i].lon) < 0.01f) {
                near.h[i].kind = kKindCity;
                near.h[i].rank = c.rank;
                cities.remove(j);
                break;
            }
        }
    }
    const int nCities = cities.n < kMaxCities ? cities.n : kMaxCities;

    // Cities go on top when the best one is big and nothing nearby is exactly what was typed.
    const bool nearWins = nNear > 0 && near.h[0].exact;
    inf.citiesFirst = nCities > 0 && cities.h[0].rank >= kRankBig && !nearWins;

    int n = 0;
    const Hits *order[2] = {inf.citiesFirst ? &cities : &near, inf.citiesFirst ? &near : &cities};
    const int counts[2] = {inf.citiesFirst ? nCities : nNear, inf.citiesFirst ? nNear : nCities};
    for (int g = 0; g < 2; g++)
        for (int i = 0; i < counts[g] && n < maxOut; i++)
            out[n++] = order[g]->h[i];
    for (int i = 0; i < n; i++) {
        if (out[i].group)
            inf.nCities++;
        else
            inf.nNear++;
    }
    inf.ms = millis() - t0;
    return n;
}

#else // no SdFat card on this build

bool placenames_on_card(void)
{
    return false;
}

void placenames_release(void) {}

int placenames_search(const char *, float, float, PlaceHit *, int, uint32_t, PlaceSearchInfo *info)
{
    if (info)
        memset(info, 0, sizeof(*info));
    return 0;
}

#endif
