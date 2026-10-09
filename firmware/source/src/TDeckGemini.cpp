// -----------------------------------------------------------------------------
// Ollama Agent for the colour T-Deck. This file keeps the proven async network path from the
// original Gemini experiment while replacing Google's provider with Ollama Cloud.
//
// Based on the existing Gemini transport, keeping the two things that version paid for:
//
//   * HTTPS runs through the shared T-Deck network door, so the UI task never blocks and the
//     TLS memory reserve is handled in one place.
//
// The network goes through tdeck_net_post() rather than a WiFiClientSecure of its own. That is
// deliberate: HTTPS works on this board only because of a contiguous-RAM reserve taken at boot
// and released just before the handshake, and a second TLS path would starve exactly as the
// Weather app did when it was missing that release.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "mesh/NodeDB.h"

#include "TDeckGemini.h"
#include "graphics/common/SdCard.h" // SDFs, the shared SdFat instance
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

extern "C" bool tdeck_net_post(const char *url, const char *body, const char *contentType);
extern "C" bool tdeck_net_fetch(const char *url);
extern "C" void tdeck_net_set_auth(const char *value);
extern "C" int tdeck_net_poll(void);
extern "C" int tdeck_net_state(void); // raw NetState, for the progress text
extern "C" int tdeck_net_result(char *buf, int cap);
extern "C" void tdeck_net_reset(void);
extern "C" int tdeck_net_http_code(void);

namespace tdeckgemini
{
namespace
{
constexpr int kMaxPrompt = 4096;
constexpr int kMaxReply = 1800;
// The conversation remains on the SD card and the request now carries a much larger rolling
// window. This is intentionally a bounded context: an LLM still has a finite context window,
// but six turns was far too short for a real conversation on the T-Deck.
constexpr int kMaxHistoryTurns = 24;
constexpr int kHistoryReply = 1000;
constexpr int kChatTranscript = 32768;
// Build the request in PSRAM, then let TDeckNet copy it into its own async buffer. Keeping this
// out of internal RAM leaves the TLS handshake room it needs.
constexpr int kRequestBody = 192 * 1024;
constexpr char kActiveChatPath[] = "/chats/active.txt";
constexpr char kActiveChatTempPath[] = "/chats/active.tmp";
constexpr char kCalendarPath[] = "/calendar/events.ics";
constexpr char kCalendarTempPath[] = "/calendar/events.tmp";
// The catalogue is authoritative. This is only a temporary label before /api/tags returns;
// once it does, an account-visible model is selected automatically if this one is absent.
const char *kDefaultModel = "gpt-oss:120b";

// ⛔ THESE ARE tdeck_net_poll()'s RETURN CODES, NOT TDeckNet's internal NetState enum.
//
// They used to be the enum - NET_DONE = 4, NET_ERROR = 5 - "mirroring TDeckNet's enum". But
// tdeck_net_poll() does not return that enum. It COLLAPSES it: "0 idle, 1 working, 2 done,
// 3 error". So the comparisons could never match, service() fell through to "keep waiting" on
// every single tick, and Gemini sat on "Connecting..." forever while the answer was sitting
// there waiting to be read.
//
// Jake found it the day he first had a key to try: "asking it 'hello' is just stuck at
// connecting". It had never worked, from the commit that added the app. Mirroring an enum
// across a module boundary is exactly the kind of duplication that rots silently - mirror what
// the FUNCTION returns, and name the constants after the function so the next person cannot
// make the same swap.
constexpr int POLL_IDLE = 0, POLL_WORKING = 1, POLL_DONE = 2, POLL_ERROR = 3;
// And these ARE the raw NetState, only ever compared against tdeck_net_state().
constexpr int RAW_CONNECTING = 2, RAW_FETCH = 3;

char gKey[80] = {0};
char gModel[64] = {0};
char gSttUrl[256] = {0};
char gSttKey[80] = {0};
char gSttModel[64] = {0};
char gSttLanguage[16] = {0};
bool gConfigRead = false;
bool gDeepgramConfigRead = false;
bool gModelConfigured = false;
bool gSummaryRequest = false;
ConversationMode gMode = MODE_CHAT;

char gPrompt[kMaxPrompt + 1] = {0};
// ⛔ PSRAM. This is reply TEXT - cold, large, and read only while the Gemini screen is up.
// In internal RAM it was 1,201 bytes of the very thing a TLS handshake needs contiguously.
// Allocated on first use; every reader already tolerates an empty string.
char *gReply = nullptr;

struct HistoryTurn {
    char user[kMaxPrompt + 1];
    char assistant[kHistoryReply + 1];
};

struct CalendarEvent {
    char title[96];
    char date[11];      // YYYY-MM-DD
    char start[6];      // HH:MM
    char end[6];        // HH:MM, optional
    char timezone[64];
    char location[96];
    char notes[256];
    char question[180]; // non-empty when Ollama needs clarification
    bool ready;
};

HistoryTurn *gHistory = nullptr;
int gHistoryCount = 0;
bool gHistoryPsram = false;
char *gChatTranscript = nullptr;
bool gChatTranscriptPsram = false;
bool gSessionLoaded = false;
CalendarEvent gCalendarDraft = {};
bool gCalendarDraftReady = false;

HistoryTurn *historyStorage()
{
    if (!gHistory) {
        gHistory = (HistoryTurn *)heap_caps_calloc(kMaxHistoryTurns, sizeof(HistoryTurn), MALLOC_CAP_SPIRAM);
        if (gHistory)
            gHistoryPsram = true;
        else
            gHistory = (HistoryTurn *)calloc(kMaxHistoryTurns, sizeof(HistoryTurn));
    }
    return gHistory;
}

void rememberExchange(const char *user, const char *assistant)
{
    HistoryTurn *h = historyStorage();
    if (!h)
        return; // Chat still works; only the optional local context is unavailable.
    if (gHistoryCount == kMaxHistoryTurns) {
        memmove(h, h + 1, sizeof(HistoryTurn) * (kMaxHistoryTurns - 1));
        gHistoryCount--;
    }
    snprintf(h[gHistoryCount].user, sizeof(h[gHistoryCount].user), "%s", user ? user : "");
    snprintf(h[gHistoryCount].assistant, sizeof(h[gHistoryCount].assistant), "%s", assistant ? assistant : "");
    gHistoryCount++;
}

const char *conversationTextInternal()
{
    if (!gChatTranscript) {
        gChatTranscript = (char *)heap_caps_calloc(kChatTranscript, 1, MALLOC_CAP_SPIRAM);
        if (gChatTranscript)
            gChatTranscriptPsram = true;
        else
            gChatTranscript = (char *)calloc(kChatTranscript, 1);
    }
    if (!gChatTranscript)
        return "";
    gChatTranscript[0] = 0;
    size_t used = 0;
    for (int i = 0; i < gHistoryCount; i++) {
        const int n = snprintf(gChatTranscript + used, kChatTranscript - used,
                               "You:\n%s\n\nAI:\n%s\n\n", gHistory[i].user, gHistory[i].assistant);
        if (n < 0 || (size_t)n >= kChatTranscript - used) {
            used = kChatTranscript - 1;
            gChatTranscript[used] = 0;
            break;
        }
        used += (size_t)n;
    }
    return gChatTranscript;
}

unsigned nextNoteId()
{
    unsigned maxId = 0;
    FsFile dir = SDFs.open("/notes", O_RDONLY);
    if (!dir || !dir.isDirectory())
        return 1;
    FsFile entry;
    while ((entry = dir.openNextFile())) {
        char name[32] = {};
        entry.getName(name, sizeof(name));
        unsigned id = 0;
        if (!entry.isDirectory() && sscanf(name, "n%u.txt", &id) == 1 && id > maxId)
            maxId = id;
        entry.close();
    }
    dir.close();
    return maxId + 1;
}

bool saveSummaryNote(const char *summary)
{
    if (!summary || !*summary)
        return false;
    SDFs.mkdir("/notes");
    char path[48];
    snprintf(path, sizeof(path), "/notes/n%03u.txt", nextNoteId());
    FsFile f = SDFs.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return false;
    f.print("Ollama Agent - conversation summary\n");
    f.print(summary);
    f.close();
    return true;
}

// Saving a note must not depend on the second Ollama request completing.  The normal path
// stores the compact AI summary; when that request fails or times out we still have the complete
// conversation locally and can save it as valid Markdown immediately.  This is the important
// offline guarantee: pressing Save never silently throws the user's work away.
bool saveConversationNote()
{
    if (!gHistory || gHistoryCount <= 0)
        return false;
    return saveSummaryNote(conversationTextInternal());
}

bool readLine(FsFile &f, char *line, size_t cap)
{
    const int n = f.fgets(line, cap);
    if (n <= 0)
        return false;
    size_t len = (size_t)n;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
    return true;
}

bool readSized(FsFile &f, const char *line, const char *prefix, char *dst, size_t cap)
{
    if (strncmp(line, prefix, strlen(prefix)) != 0)
        return false;
    const unsigned long want = strtoul(line + strlen(prefix), nullptr, 10);
    if (want >= cap)
        return false;
    const int n = f.read((uint8_t *)dst, (size_t)want);
    if (n != (int)want)
        return false;
    dst[want] = 0;
    int c = f.read();
    if (c == '\r')
        c = f.read();
    return c == '\n';
}

void writeSized(FsFile &f, const char *prefix, const char *text)
{
    const char *value = text ? text : "";
    const size_t len = strlen(value);
    f.print(prefix);
    f.println((unsigned)len);
    f.write((const uint8_t *)value, len);
    f.write((uint8_t)'\n');
}

// These live below the allocator helpers, but session restore is kept next to the file format.
extern char gStatus[96];
extern int gState;
static char *replyBuf(void);

void saveSession()
{
    HistoryTurn *h = gHistory;
    if (!h || gHistoryCount <= 0)
        return;
    SDFs.mkdir("/chats");
    FsFile f = SDFs.open(kActiveChatTempPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return;
    f.println("TDECK_CHAT_V1");
    f.print("MODE=");
    f.println((int)gMode);
    f.print("TURNS=");
    f.println(gHistoryCount);
    for (int i = 0; i < gHistoryCount; i++) {
        writeSized(f, "USER_LEN=", h[i].user);
        writeSized(f, "ASSISTANT_LEN=", h[i].assistant);
    }
    writeSized(f, "REPLY_LEN=", gReply ? gReply : "");
    f.print("CAL_READY=");
    f.println(gCalendarDraftReady ? 1 : 0);
    if (gCalendarDraftReady) {
        writeSized(f, "CAL_TITLE_LEN=", gCalendarDraft.title);
        writeSized(f, "CAL_DATE_LEN=", gCalendarDraft.date);
        writeSized(f, "CAL_START_LEN=", gCalendarDraft.start);
        writeSized(f, "CAL_END_LEN=", gCalendarDraft.end);
        writeSized(f, "CAL_TZ_LEN=", gCalendarDraft.timezone);
        writeSized(f, "CAL_LOCATION_LEN=", gCalendarDraft.location);
        writeSized(f, "CAL_NOTES_LEN=", gCalendarDraft.notes);
    }
    f.close();
    SDFs.remove(kActiveChatPath); // rename does not replace on the SD implementation
    SDFs.rename(kActiveChatTempPath, kActiveChatPath);
}

void loadSession()
{
    if (gSessionLoaded)
        return;
    gSessionLoaded = true;
    FsFile f = SDFs.open(kActiveChatPath, O_RDONLY);
    if (!f)
        return;
    char line[48] = {};
    if (!readLine(f, line, sizeof(line)) || strcmp(line, "TDECK_CHAT_V1")) {
        f.close();
        return;
    }
    if (!readLine(f, line, sizeof(line)) || strncmp(line, "MODE=", 5)) {
        f.close();
        return;
    }
    const int storedMode = atoi(line + 5);
    if (storedMode == MODE_MARKDOWN || storedMode == MODE_CALENDAR)
        gMode = (ConversationMode)storedMode;
    if (!readLine(f, line, sizeof(line)) || strncmp(line, "TURNS=", 6)) {
        f.close();
        return;
    }
    int turns = atoi(line + 6);
    if (turns < 0 || turns > kMaxHistoryTurns)
        turns = kMaxHistoryTurns;
    HistoryTurn *h = historyStorage();
    if (!h) {
        f.close();
        return;
    }
    for (int i = 0; i < turns; i++) {
        if (!readLine(f, line, sizeof(line)) ||
            !readSized(f, line, "USER_LEN=", h[i].user, sizeof(h[i].user)) ||
            !readLine(f, line, sizeof(line)) ||
            !readSized(f, line, "ASSISTANT_LEN=", h[i].assistant, sizeof(h[i].assistant))) {
            gHistoryCount = 0;
            f.close();
            return;
        }
    }
    if (!readLine(f, line, sizeof(line)) || !replyBuf() ||
        !readSized(f, line, "REPLY_LEN=", gReply, kMaxReply + 1)) {
        gHistoryCount = 0;
        f.close();
        return;
    }
    gHistoryCount = turns;
    gCalendarDraftReady = false;
    memset(&gCalendarDraft, 0, sizeof(gCalendarDraft));
    if (readLine(f, line, sizeof(line)) && !strncmp(line, "CAL_READY=", 10) && atoi(line + 10)) {
        bool ok = readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_TITLE_LEN=", gCalendarDraft.title, sizeof(gCalendarDraft.title));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_DATE_LEN=", gCalendarDraft.date, sizeof(gCalendarDraft.date));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_START_LEN=", gCalendarDraft.start, sizeof(gCalendarDraft.start));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_END_LEN=", gCalendarDraft.end, sizeof(gCalendarDraft.end));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_TZ_LEN=", gCalendarDraft.timezone, sizeof(gCalendarDraft.timezone));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_LOCATION_LEN=", gCalendarDraft.location, sizeof(gCalendarDraft.location));
        ok = ok && readLine(f, line, sizeof(line)) && readSized(f, line, "CAL_NOTES_LEN=", gCalendarDraft.notes, sizeof(gCalendarDraft.notes));
        if (ok) {
            gCalendarDraft.ready = true;
            gCalendarDraftReady = true;
        } else {
            memset(&gCalendarDraft, 0, sizeof(gCalendarDraft));
        }
    }
    f.close();
    gState = DONE;
    snprintf(gStatus, sizeof(gStatus), "Restored chat");
}

// ⛔ THE SIZE HAS TO BE A NAMED CONSTANT, not sizeof(). This buffer used to be `char buf[4096]`,
// where sizeof(buf) was 4096. Moving it to PSRAM made it a POINTER, and every sizeof(buf) left
// behind silently became 4 - so the HTTP reply was read 4 bytes at a time and Gemini stopped
// working. Nothing warns about it; the code still compiles and still looks right.
static const int kWorkBuf = 4096;
static char *s_workBuf = nullptr;

static char *geminiWorkBuf(void)
{
    if (!s_workBuf) {
        s_workBuf = (char *)heap_caps_malloc(kWorkBuf, MALLOC_CAP_SPIRAM);
        if (!s_workBuf)
            s_workBuf = (char *)malloc(kWorkBuf);
    }
    return s_workBuf;
}

static void geminiReleaseWork(void)
{
    if (s_workBuf) {
        heap_caps_free(s_workBuf);
        s_workBuf = nullptr;
    }
}

static char *replyBuf(void)
{
    if (!gReply) {
        gReply = (char *)heap_caps_calloc(kMaxReply + 1, 1, MALLOC_CAP_SPIRAM);
        if (!gReply)
            gReply = (char *)calloc(kMaxReply + 1, 1); // tiny boards / no PSRAM
    }
    return gReply;
}
char gStatus[96] = {0};
int gState = IDLE;
bool gAskPending = false;
int gRetries = 0;         // 503/429 attempts used on THIS question
uint32_t gRetryAtMs = 0;  // when to fire the next one; 0 = nothing pending

// Cloud model catalogue. Direct Ollama Cloud access exposes the models available to the
// account at /api/tags. Keep only short names in PSRAM-sized static storage; the UI can cycle
// through them without requiring a second settings screen.
constexpr int kMaxModels = 32;
char gModels[kMaxModels][64] = {{0}};
int gModelCount = 0;
bool gModelsPending = false;
bool gModelsActive = false;

// ---- /ollama.txt ---------------------------------------------------------------------------
// key=... and model=..., one per line, '#' comments. The key stays on the SD card and is never
// compiled into a shareable firmware image.
// ⛔ A FAILED SD READ IS NOT PROOF THE KEY IS ABSENT, AND MUST NOT BE CACHED.
//
// This set gConfigRead = true BEFORE opening the file, so a single failed open - the card shares
// its SPI bus with the display and the radio, and readConfig() runs early in boot when it may not
// even be mounted yet - latched "no key" for the rest of the session. Jake's key was on the card
// the whole time; the device had decided otherwise once, seconds after power-on, and never looked
// again.
//
// This is the FOURTH time today: Mail's haveCreds() asking him to log in every time, the map style
// list saying "no styles on card", the app icon reader, and now this. The shape is always the same
// - an unreliable read whose failure is recorded as a fact. Only cache a SUCCESS.
void readConfig()
{
    if (gConfigRead)
        return;
    FsFile f = SDFs.open("/ollama.txt", O_RDONLY);
    if (!f) {
        delay(20); // the other user of the bus is mid-transfer; give it a moment
        f = SDFs.open("/ollama.txt", O_RDONLY);
    }
    if (!f) {
        LOG_INFO("[OLLAMA] could not read /ollama.txt - will try again next time, not caching this");
        return; // deliberately NOT setting gConfigRead: we do not know, so we will ask again
    }
    gConfigRead = true;
    char line[160];
    while (f.available()) {
        int n = f.fgets(line, sizeof(line));
        if (n <= 0)
            break;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !*p)
            continue;
        if (!strncmp(p, "key=", 4))
            snprintf(gKey, sizeof(gKey), "%s", p + 4);
        else if (!strncmp(p, "model=", 6)) {
            snprintf(gModel, sizeof(gModel), "%s", p + 6);
            gModelConfigured = gModel[0] != 0;
        }
    }
    f.close();
    if (!gModel[0])
        snprintf(gModel, sizeof(gModel), "%s", kDefaultModel);
    LOG_INFO("[OLLAMA] config: key=%s model=%s", gKey[0] ? "yes" : "MISSING", gModel);
}

// Deepgram has its own SD-card file and credential. Never borrow the Ollama key here.
// The filename is /deepgram.txt; accept both concise and stt_-prefixed names so an existing
// setup made from the earlier template keeps working.
void readDeepgramConfig()
{
    if (gDeepgramConfigRead)
        return;
    FsFile f = SDFs.open("/deepgram.txt", O_RDONLY);
    if (!f) {
        delay(20);
        f = SDFs.open("/deepgram.txt", O_RDONLY);
    }
    if (!f) {
        LOG_INFO("[VOICE] could not read /deepgram.txt - will try again next time");
        return;
    }
    gDeepgramConfigRead = true;
    char line[160];
    while (f.available()) {
        int n = f.fgets(line, sizeof(line));
        if (n <= 0)
            break;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || !*p)
            continue;
        if (!strncmp(p, "stt_key=", 8))
            snprintf(gSttKey, sizeof(gSttKey), "%s", p + 8);
        else if (!strncmp(p, "deepgram_key=", 13))
            snprintf(gSttKey, sizeof(gSttKey), "%s", p + 13);
        else if (!strncmp(p, "api_key=", 8))
            snprintf(gSttKey, sizeof(gSttKey), "%s", p + 8);
        else if (!strncmp(p, "key=", 4))
            snprintf(gSttKey, sizeof(gSttKey), "%s", p + 4);
        else if (!strncmp(p, "stt_url=", 8))
            snprintf(gSttUrl, sizeof(gSttUrl), "%s", p + 8);
        else if (!strncmp(p, "url=", 4))
            snprintf(gSttUrl, sizeof(gSttUrl), "%s", p + 4);
        else if (!strncmp(p, "stt_model=", 10))
            snprintf(gSttModel, sizeof(gSttModel), "%s", p + 10);
        else if (!strncmp(p, "model=", 6))
            snprintf(gSttModel, sizeof(gSttModel), "%s", p + 6);
        else if (!strncmp(p, "stt_language=", 13))
            snprintf(gSttLanguage, sizeof(gSttLanguage), "%s", p + 13);
        else if (!strncmp(p, "language=", 9))
            snprintf(gSttLanguage, sizeof(gSttLanguage), "%s", p + 9);
        else if (!strchr(p, '=') && !gSttKey[0])
            snprintf(gSttKey, sizeof(gSttKey), "%s", p); // allow a plain key as the only line
    }
    f.close();
    if (!gSttUrl[0])
        snprintf(gSttUrl, sizeof(gSttUrl), "%s", "https://api.deepgram.com/v1/listen");
    if (!gSttModel[0])
        snprintf(gSttModel, sizeof(gSttModel), "%s", "nova-3");
    if (!gSttLanguage[0])
        snprintf(gSttLanguage, sizeof(gSttLanguage), "%s", "de");
    LOG_INFO("[VOICE] Deepgram config: key=%s model=%s", gSttKey[0] ? "yes" : "MISSING", gSttModel);
}

void fail(const char *why)
{
    snprintf(gStatus, sizeof(gStatus), "%s", why);
    gState = FAILED;
    LOG_WARN("[GEMINI] %s", why);
}

// ---- JSON in and out -------------------------------------------------------------------------
void jsonEscape(const char *in, char *out, int outSize)
{
    int o = 0;
    for (const char *p = in; *p && o < outSize - 8; p++) {
        switch (*p) {
        case '"':
            out[o++] = '\\';
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            out[o++] = '\\';
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            break;
        case '\t':
            out[o++] = ' ';
            break;
        default:
            if ((unsigned char)*p >= 0x20)
                out[o++] = *p;
        }
    }
    out[o] = 0;
}

void jsonUnescapeInto(const char *p, char *out, int outSize)
{
    int o = 0;
    for (; *p && *p != '"' && o < outSize - 1; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':
                out[o++] = '\n';
                break;
            case 't':
                out[o++] = ' ';
                break;
            case 'u': // \uXXXX - not worth a decoder here; skip the code point
                if (p[1] && p[2] && p[3] && p[4])
                    p += 4;
                break;
            default:
                out[o++] = *p;
            }
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

// Ollama returns the answer in message.content. Error responses carry message instead, and
// saying WHICH is the difference between a fixable problem and a shrug.
bool extractReply(const char *body, char *out, int outSize)
{
    const char *t = strstr(body, "\"content\":");
    if (t) {
        const char *q = strchr(t + 10, '"');
        if (q) {
            jsonUnescapeInto(q + 1, out, outSize);
            return out[0] != 0;
        }
    }
    const char *m = strstr(body, "\"message\":");
    if (m) {
        const char *q = strchr(m + 10, '"');
        if (q) {
            char msg[160];
            jsonUnescapeInto(q + 1, msg, sizeof(msg));
            snprintf(out, outSize, "Ollama said no: %s", msg);
        }
    }
    return false;
}

bool jsonField(const char *json, const char *key, char *out, int outSize)
{
    char needle[48];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return false;
    const char *colon = strchr(p + strlen(needle), ':');
    if (!colon)
        return false;
    const char *quote = strchr(colon + 1, '"');
    if (!quote)
        return false;
    jsonUnescapeInto(quote + 1, out, outSize);
    return true;
}

bool validDate(const char *s)
{
    return s && strlen(s) == 10 && s[4] == '-' && s[7] == '-' &&
           isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) &&
           isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]) &&
           isdigit((unsigned char)s[5]) && isdigit((unsigned char)s[6]) &&
           isdigit((unsigned char)s[8]) && isdigit((unsigned char)s[9]);
}

bool validTime(const char *s)
{
    return s && strlen(s) == 5 && s[2] == ':' && isdigit((unsigned char)s[0]) &&
           isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]);
}

bool parseCalendarEvent(const char *reply, CalendarEvent &event)
{
    memset(&event, 0, sizeof(event));
    const char *json = strchr(reply, '{');
    if (!json)
        return false;
    const char *ready = strstr(json, "\"ready\"");
    if (ready) {
        const char *colon = strchr(ready, ':');
        if (colon) {
            while (*++colon == ' ' || *colon == '\t')
                ;
            event.ready = !strncmp(colon, "true", 4);
        }
    }
    jsonField(json, "title", event.title, sizeof(event.title));
    jsonField(json, "date", event.date, sizeof(event.date));
    jsonField(json, "start", event.start, sizeof(event.start));
    jsonField(json, "end", event.end, sizeof(event.end));
    jsonField(json, "timezone", event.timezone, sizeof(event.timezone));
    jsonField(json, "location", event.location, sizeof(event.location));
    jsonField(json, "notes", event.notes, sizeof(event.notes));
    jsonField(json, "question", event.question, sizeof(event.question));
    if (!event.timezone[0])
        snprintf(event.timezone, sizeof(event.timezone), "%s", config.device.tzdef[0] ? config.device.tzdef : "UTC");
    if (event.ready && (!event.title[0] || !validDate(event.date) || !validTime(event.start) ||
                        !validTime(event.end))) {
        event.ready = false;
        if (!event.question[0])
            snprintf(event.question, sizeof(event.question), "Welche Endzeit soll der Termin haben?");
    }
    return true;
}

void formatCalendarPreview(const CalendarEvent &event, char *out, int outSize)
{
    int used = snprintf(out, outSize, "Calendar draft:\n%s\n%s %s-%s",
                        event.title, event.date, event.start, event.end);
    if (used < 0 || used >= outSize)
        return;
    if (event.location[0]) {
        used += snprintf(out + used, outSize - used, "\nLocation: %s", event.location);
    }
    if (event.notes[0] && used < outSize)
        snprintf(out + used, outSize - used, "\n%s", event.notes);
}

void writeIcsEscaped(FsFile &f, const char *text)
{
    for (const char *p = text ? text : ""; *p; p++) {
        switch (*p) {
        case '\\':
            f.print("\\\\");
            break;
        case ';':
            f.print("\\;");
            break;
        case ',':
            f.print("\\,");
            break;
        case '\n':
            f.print("\\n");
            break;
        case '\r':
            break;
        default:
            f.write((uint8_t)*p);
            break;
        }
    }
}

void writeIcsField(FsFile &f, const char *name, const char *value)
{
    f.print(name);
    f.print(":");
    writeIcsEscaped(f, value);
    f.print("\r\n");
}

void writeIcsDateTime(FsFile &f, const char *name, const char *date, const char *clock)
{
    f.print(name);
    f.print(":");
    for (int i = 0; i < 10; i++)
        if (date[i] != '-')
            f.write((uint8_t)date[i]);
    f.print("T");
    for (int i = 0; i < 5; i++)
        if (clock[i] != ':')
            f.write((uint8_t)clock[i]);
    f.print("00\r\n");
}

bool saveCalendarEvent(const CalendarEvent &event)
{
    if (!event.ready || !validDate(event.date) || !validTime(event.start) || !validTime(event.end))
        return false;

    SDFs.mkdir("/calendar");
    FsFile out = SDFs.open(kCalendarTempPath, O_WRONLY | O_CREAT | O_TRUNC);
    if (!out)
        return false;

    bool copiedCalendar = false;
    FsFile old = SDFs.open(kCalendarPath, O_RDONLY);
    if (old) {
        char line[256];
        while (old.fgets(line, sizeof(line)) > 0) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = 0;
            if (!strcmp(line, "END:VCALENDAR"))
                continue;
            if (!strcmp(line, "BEGIN:VCALENDAR"))
                copiedCalendar = true;
            out.print(line);
            out.print("\r\n");
        }
        old.close();
    }
    if (!copiedCalendar) {
        out.print("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//T-Deck Ollama Agent//EN\r\n");
    }

    char uid[64];
    snprintf(uid, sizeof(uid), "tdeck-%lu@tdeck", (unsigned long)millis());
    out.print("BEGIN:VEVENT\r\n");
    writeIcsField(out, "UID", uid);
    writeIcsField(out, "DTSTAMP", "19700101T000000Z");
    writeIcsDateTime(out, "DTSTART", event.date, event.start);
    writeIcsDateTime(out, "DTEND", event.date, event.end);
    writeIcsField(out, "SUMMARY", event.title);
    if (event.location[0])
        writeIcsField(out, "LOCATION", event.location);
    if (event.notes[0])
        writeIcsField(out, "DESCRIPTION", event.notes);
    if (event.timezone[0])
        writeIcsField(out, "X-T-DECK-TIMEZONE", event.timezone);
    out.print("END:VEVENT\r\nEND:VCALENDAR\r\n");
    out.close();

    SDFs.remove(kCalendarPath);
    return SDFs.rename(kCalendarTempPath, kCalendarPath);
}

void extractModels(const char *body)
{
    gModelCount = 0;
    const char *p = body;
    while (p && *p && gModelCount < kMaxModels) {
        p = strstr(p, "\"name\":");
        if (!p)
            break;
        const char *q = strchr(p + 7, '"');
        if (!q)
            break;
        ++q;
        const char *end = strchr(q, '"');
        if (!end)
            break;
        int n = (int)(end - q);
        if (n > 0 && n < (int)sizeof(gModels[0])) {
            memcpy(gModels[gModelCount], q, (size_t)n);
            gModels[gModelCount][n] = 0;
            gModelCount++;
        }
        p = end + 1;
    }
    if (!gModelConfigured && gModelCount > 0) {
        bool currentAvailable = false;
        for (int i = 0; i < gModelCount; i++) {
            if (!strcmp(gModels[i], gModel)) {
                currentAvailable = true;
                break;
            }
        }
        if (!currentAvailable)
            snprintf(gModel, sizeof(gModel), "%s", gModels[0]);
    }
    LOG_INFO("[OLLAMA] cloud catalogue: %d models", gModelCount);
}

bool appendMessage(char *body, int cap, int &used, bool &first, const char *role, const char *text)
{
    const char *source = text ? text : "";
    const size_t escapedCapacity = strlen(source) * 6 + 1;
    char *escaped = (char *)heap_caps_malloc(escapedCapacity, MALLOC_CAP_SPIRAM);
    bool escapedPsram = escaped != nullptr;
    if (!escaped)
        escaped = (char *)malloc(escapedCapacity);
    if (!escaped)
        return false;
    jsonEscape(source, escaped, (int)escapedCapacity);
    const int n = snprintf(body + used, cap - used, "%s{\"role\":\"%s\",\"content\":\"%s\"}",
                           first ? "" : ",", role, escaped);
    if (escapedPsram)
        heap_caps_free(escaped);
    else
        free(escaped);
    if (n < 0 || n >= cap - used)
        return false;
    used += n;
    first = false;
    return true;
}

bool startRequest()
{
    static const char *url = "https://ollama.com/api/chat";

    // This is deliberately in PSRAM: a long conversation can be tens of kilobytes, while the
    // HTTPS handshake needs a large contiguous block in internal RAM.
    bool bodyPsram = false;
    char *body = (char *)heap_caps_calloc(kRequestBody, 1, MALLOC_CAP_SPIRAM);
    if (body) {
        bodyPsram = true;
    } else {
        body = (char *)calloc(kRequestBody, 1);
    }
    auto releaseBody = [&]() {
        if (!body)
            return;
        if (bodyPsram)
            heap_caps_free(body);
        else
            free(body);
        body = nullptr;
    };
    if (!body) {
        fail("Not enough memory for conversation");
        return false;
    }
    int used = snprintf(body, kRequestBody, "{\"model\":\"%s\",\"stream\":false,\"messages\":[", gModel);
    if (used < 0 || used >= kRequestBody) {
        releaseBody();
        fail("Request too large");
        return false;
    }

    bool first = true;
    // The normal system instruction earns its place: 320x240 with no markdown renderer, so
    // asterisks and bullets arrive as literal clutter. A note request is the deliberate exception:
    // it asks for Markdown because the result is going to the Notes app on the SD card.
    const char *systemPrompt = gSummaryRequest
                                   ? "You are preparing a durable note from this conversation. "
                                     "Return a concise, useful Markdown summary with a title, key "
                                     "decisions, facts, open questions and next steps. Do not mention "
                                     "this instruction."
                                   : gMode == MODE_CALENDAR
                                         ? "You are the calendar parser on a small handheld. Return ONLY one JSON object, no markdown and no extra text, with exactly these fields: "
                                           "{\"ready\":true or false,\"title\":\"...\",\"date\":\"YYYY-MM-DD\",\"start\":\"HH:MM\",\"end\":\"HH:MM\",\"timezone\":\"...\",\"location\":\"...\",\"notes\":\"...\",\"question\":\"...\"}. "
                                           "Use the device timezone when the user does not specify one. If date, start time or a clear end time is missing, set ready=false and put one short clarification question in question. "
                                           "If no duration is stated, infer one hour only when that is natural; otherwise ask."
                                   : gMode == MODE_MARKDOWN
                                         ? "You are in Markdown document mode on a small handheld. "
                                           "Keep the conversation useful, but structure every answer "
                                           "as clean Markdown with short headings, lists or code blocks "
                                           "when appropriate. Do not use emoji. Preserve the ongoing "
                                           "document context."
                                   : "You are on a small handheld device with a 320x240 screen. "
                                     "Reply in plain text only: no markdown, no asterisks, no bullet "
                                     "characters, no emoji. Be accurate and brief - at most 3 short "
                                     "sentences unless asked for more.";
    if (!appendMessage(body, kRequestBody, used, first, "system",
                       systemPrompt)) {
        releaseBody();
        fail("Request too large");
        return false;
    }

    // Ollama receives the recent turns as normal chat messages. The active session is held in
    // PSRAM while running and mirrored to /chats/active.txt after each completed answer, so
    // leaving the app does not lose it. Only Delete removes the saved session.
    HistoryTurn *h = gHistory;
    for (int i = 0; h && i < gHistoryCount; i++) {
        if (!appendMessage(body, kRequestBody, used, first, "user", h[i].user) ||
            !appendMessage(body, kRequestBody, used, first, "assistant", h[i].assistant)) {
            releaseBody();
            fail("Conversation too long");
            return false;
        }
    }
    if (!appendMessage(body, kRequestBody, used, first, "user", gPrompt)) {
        releaseBody();
        fail("Question too long");
        return false;
    }
    const int tail = snprintf(body + used, kRequestBody - used,
                              "],\"options\":{\"num_predict\":400,\"temperature\":0.7}}");
    if (tail < 0 || tail >= kRequestBody - used) {
        releaseBody();
        fail("Request too large");
        return false;
    }

    char auth[144];
    snprintf(auth, sizeof(auth), "Bearer %s", gKey);
    tdeck_net_set_auth(auth);
    LOG_INFO("[OLLAMA] asking %s (%d char prompt)", gModel, (int)strlen(gPrompt));
    const bool queued = tdeck_net_post(url, body, "application/json");
    releaseBody();
    if (!queued) {
        fail("Could not start the request");
        return false;
    }
    return true;
}
} // namespace

// ---- public ----------------------------------------------------------------------------------
const char *conversationText()
{
    return conversationTextInternal();
}

void ask(const char *prompt)
{
    if (!prompt || !*prompt)
        return;
    gSummaryRequest = false;
    if (gMode == MODE_CALENDAR)
        gCalendarDraftReady = false;
    snprintf(gPrompt, sizeof(gPrompt), "%s", prompt);
    if (replyBuf())
        gReply[0] = 0;
    gRetries = 0;    // a new question gets its own budget of busy-retries
    gRetryAtMs = 0;  // and must never inherit a pending one from the last
    snprintf(gStatus, sizeof(gStatus), "Connecting...");
    gState = WORKING;
    gAskPending = true; // the request itself starts on the next service() tick
}

bool hasConversation()
{
    return gHistoryCount > 0;
}

void saveSummary()
{
    if (gState == WORKING || !hasConversation())
        return;
    gSummaryRequest = true;
    snprintf(gPrompt, sizeof(gPrompt),
             "Summarize the conversation so far as a durable Markdown note. Preserve concrete "
             "names, decisions, important details and next steps.");
    if (replyBuf())
        gReply[0] = 0;
    gRetries = 0;
    gRetryAtMs = 0;
    snprintf(gStatus, sizeof(gStatus), "Preparing note...");
    gState = WORKING;
    gAskPending = true;
}

int state()
{
    return gState;
}
const char *reply()
{
    return gReply;
}
const char *statusText()
{
    return gStatus;
}
void release(void)
{
    // The active chat is already saved after every completed turn. Save once more on teardown so
    // leaving Ollama after a completed answer cannot lose the last state if the UI timer has not
    // reached its normal persistence tick yet. Never write a half-finished request.
    if (gState != WORKING && gHistoryCount > 0)
        saveSession();
    if (gReply) {
        heap_caps_free(gReply);
        gReply = nullptr;
    }
    geminiReleaseWork();
    if (gHistory) {
        if (gHistoryPsram)
            heap_caps_free(gHistory);
        else
            free(gHistory);
        gHistory = nullptr;
        gHistoryPsram = false;
    }
    if (gChatTranscript) {
        if (gChatTranscriptPsram)
            heap_caps_free(gChatTranscript);
        else
            free(gChatTranscript);
        gChatTranscript = nullptr;
        gChatTranscriptPsram = false;
    }
    gHistoryCount = 0;
    gSummaryRequest = false;
    gMode = MODE_CHAT;
    gCalendarDraftReady = false;
    memset(&gCalendarDraft, 0, sizeof(gCalendarDraft));
    gSessionLoaded = false;
}

void clear()
{
    if (replyBuf())
        gReply[0] = 0;
    gStatus[0] = 0;
    gState = IDLE;
    gSummaryRequest = false;
    gMode = MODE_CHAT;
    gCalendarDraftReady = false;
    memset(&gCalendarDraft, 0, sizeof(gCalendarDraft));
    if (gHistory)
        memset(gHistory, 0, sizeof(HistoryTurn) * kMaxHistoryTurns);
    gHistoryCount = 0;
    SDFs.remove(kActiveChatPath);
    SDFs.remove(kActiveChatTempPath);
    gSessionLoaded = true;
    tdeck_net_reset();
}

bool haveConfig()
{
    readConfig();
    return gKey[0] != 0;
}

bool voiceConfig(char *url, size_t urlCap, char *key, size_t keyCap, char *model, size_t modelCap,
                 char *language, size_t languageCap)
{
    readDeepgramConfig();
    if (url && urlCap)
        snprintf(url, urlCap, "%s", gSttUrl);
    if (key && keyCap)
        snprintf(key, keyCap, "%s", gSttKey);
    if (model && modelCap)
        snprintf(model, modelCap, "%s", gSttModel);
    if (language && languageCap)
        snprintf(language, languageCap, "%s", gSttLanguage);
    return gSttUrl[0] && gSttKey[0] && gSttModel[0];
}

int conversationTurns()
{
    return gHistoryCount;
}

void restoreSession()
{
    loadSession();
}

void deleteSession()
{
    if (gState != WORKING)
        clear();
}

void setMode(ConversationMode mode)
{
    if (gState == WORKING)
        return;
    gMode = mode;
    if (gHistoryCount > 0)
        saveSession();
}

ConversationMode mode()
{
    return gMode;
}

bool hasCalendarDraft()
{
    return gCalendarDraftReady;
}

bool saveCalendarDraft()
{
    if (!gCalendarDraftReady)
        return false;
    if (!saveCalendarEvent(gCalendarDraft))
        return false;
    gCalendarDraftReady = false;
    snprintf(gStatus, sizeof(gStatus), "Calendar event saved");
    return true;
}

void refreshModels()
{
    gModelsPending = true;
}

int modelCount()
{
    return gModelCount;
}

const char *modelName()
{
    return gModel;
}

void nextModel()
{
    if (gModelCount <= 0)
        return;
    int current = -1;
    for (int i = 0; i < gModelCount; i++) {
        if (!strcmp(gModels[i], gModel)) {
            current = i;
            break;
        }
    }
    snprintf(gModel, sizeof(gModel), "%s", gModels[(current + 1) % gModelCount]);
}

void service()
{
    // Model discovery is independent from chat. It is deliberately serviced before a question,
    // so a tap on the model button never blocks the UI thread.
    if (gModelsPending && !gModelsActive && gState != WORKING) {
        gModelsPending = false;
        readConfig();
        if (gKey[0]) {
            char auth[144];
            snprintf(auth, sizeof(auth), "Bearer %s", gKey);
            tdeck_net_set_auth(auth);
            gModelsActive = tdeck_net_fetch("https://ollama.com/api/tags");
        }
    }
    if (gModelsActive) {
        const int catalogueNet = tdeck_net_poll();
        if (catalogueNet == POLL_DONE) {
            char *buf = geminiWorkBuf();
            if (buf) {
                const int n = tdeck_net_result(buf, kWorkBuf);
                if (n > 0) {
                    buf[n < kWorkBuf ? n : kWorkBuf - 1] = 0;
                    extractModels(buf);
                }
            }
            tdeck_net_reset();
            gModelsActive = false;
        } else if (catalogueNet == POLL_ERROR) {
            tdeck_net_reset();
            gModelsActive = false;
        }
        // The network door is single-flight. If the user asks while the catalogue is still
        // arriving, leave the question queued and let the catalogue finish first instead of
        // handing a second POST to a busy door (which used to look like a random failed chat).
        if (gModelsActive)
            return;
    }

    if (gState != WORKING)
        return;

    // A 503/429 retry that is waiting out its backoff. Fired from here rather than from inside
    // the error handler for the same reason the 404 retry is: never call the request function
    // from inside a failed request.
    if (gRetryAtMs) {
        if ((int32_t)(millis() - gRetryAtMs) < 0)
            return; // still waiting
        gRetryAtMs = 0;
        startRequest();
        return;
    }

    if (gAskPending) {
        gAskPending = false;
        readConfig();
        if (!gKey[0]) {
            fail("No key - put one in /ollama.txt on the SD card");
            return;
        }
        startRequest();
        return;
    }

    // ⛔ SAY WHICH STAGE IT IS ON. Jake, 2026-09-25: "asking it 'hello' is just stuck at
    // connecting". "Connecting..." was set once in ask() and never touched again until the
    // answer or the error landed - so joining wi-fi, negotiating TLS and waiting on Google all
    // looked identical, and identical to a genuine hang. A request that takes 20 seconds and one
    // that will never finish MUST NOT look the same.
    {
        static int lastNet = -1;
        const int st = tdeck_net_state();
        if (st != lastNet) {
            lastNet = st;
            if (st == RAW_CONNECTING)
                snprintf(gStatus, sizeof(gStatus), "Joining wi-fi...");
            else if (st == RAW_FETCH)
                snprintf(gStatus, sizeof(gStatus), "Asking Ollama...");
        }
    }

    const int net = tdeck_net_poll();
    if (net == POLL_DONE) {
        // ⛔ PSRAM, for the same reason as gReply above: 4KB of internal RAM held permanently
        // for a buffer used only while a Gemini request is in flight. File scope rather than a
        // static local so release() can hand it back when the app is closed.
        char *buf = geminiWorkBuf();
        if (!buf)
            return;
        const int n = tdeck_net_result(buf, kWorkBuf);
        tdeck_net_reset();
        if (n <= 0) {
            fail("Empty answer");
            return;
        }
        buf[n < kWorkBuf ? n : kWorkBuf - 1] = 0;
        char *reply = replyBuf();
        if (!reply) {
            fail("Out of memory reading the answer");
            return;
        }
        if (extractReply(buf, reply, kMaxReply + 1)) {
            if (gSummaryRequest) {
                const bool saved = saveSummaryNote(reply);
                gSummaryRequest = false;
                snprintf(gStatus, sizeof(gStatus), "%s", saved ? "Saved to Notes" : "Could not save note");
            } else if (gMode == MODE_CALENDAR) {
                if (!parseCalendarEvent(reply, gCalendarDraft)) {
                    gCalendarDraftReady = false;
                    snprintf(gStatus, sizeof(gStatus), "Could not read calendar event");
                    reply[0] = 0;
                    gState = FAILED;
                    return;
                }
                gCalendarDraftReady = gCalendarDraft.ready;
                if (gCalendarDraftReady) {
                    formatCalendarPreview(gCalendarDraft, reply, kMaxReply + 1);
                    snprintf(gStatus, sizeof(gStatus), "Draft ready - press Save");
                } else {
                    snprintf(reply, kMaxReply + 1, "%s",
                             gCalendarDraft.question[0] ? gCalendarDraft.question
                                                        : "Please provide date and time.");
                    snprintf(gStatus, sizeof(gStatus), "Calendar needs one detail");
                }
                rememberExchange(gPrompt, reply);
                saveSession();
            } else {
                rememberExchange(gPrompt, reply);
                saveSession();
                snprintf(gStatus, sizeof(gStatus), "");
            }
            gState = DONE;
        } else {
            if (gSummaryRequest && hasConversation()) {
                const bool saved = saveConversationNote();
                gSummaryRequest = false;
                snprintf(gStatus, sizeof(gStatus), "%s",
                         saved ? "Saved chat to Notes (raw Markdown)" : "Could not save note");
                gState = saved ? DONE : FAILED;
                return;
            }
            // extractReply puts Ollama's own explanation in gReply when it refused; show that
            // rather than a generic failure, because it is usually actionable.
            snprintf(gStatus, sizeof(gStatus), "%s", reply[0] ? reply : "Could not read the answer");
            reply[0] = 0;
            gState = FAILED;
        }
        return;
    }

    if (net == POLL_ERROR) {
        const int code = tdeck_net_http_code();
        tdeck_net_reset();
        if (gSummaryRequest && hasConversation()) {
            const bool saved = saveConversationNote();
            gSummaryRequest = false;
            snprintf(gStatus, sizeof(gStatus), "%s",
                     saved ? "Saved chat to Notes (summary offline)" : "Could not save note");
            gState = saved ? DONE : FAILED;
            return;
        }
        // ⛔ A LOOP, NEVER RECURSION. The Max's first version retried by calling the request
        // function from inside itself, mid-TLS, and overflowed the stack - a double exception and
        // an instant reboot on every send. Coming back through service() costs nothing and cannot
        // do that.
        // ⭐ 503 AND 429 ARE "ASK AGAIN", NOT "IT BROKE". Google's own 503 body says so: "This
        // model is currently experiencing high demand. Spikes in demand are usually temporary.
        // Please try again later." Measured from the PC on the same key the same evening - a
        // 503, then an immediate retry returned a normal answer. Handing Jake "Gemini error 503"
        // and stopping makes a busy minute look like a broken feature, and he cannot tell the
        // difference between the two from the screen.
        //
        // Two attempts, not a loop: if Google is genuinely down, hammering it is both rude and
        // useless, and the honest message is better than a spinner that never ends.
        if ((code == 503 || code == 429) && gRetries < 2) {
            gRetries++;
            LOG_INFO("[OLLAMA] HTTP %d (busy) - retry %d of 2", code, gRetries);
            snprintf(gStatus, sizeof(gStatus), "Ollama is busy - trying again...");
            gRetryAtMs = millis() + 2500; // a short breath; startRequest() fires from service()
            return;
        }
        if (code == 400 || code == 401 || code == 403)
            fail("Key rejected - check /ollama.txt");
        else if (code == 429)
            fail("Ollama is busy - try again in a minute");
        else if (code == 503)
            fail("Ollama is busy right now - try again in a minute");
        else if (code <= 0)
            fail("No network - check wi-fi");
        else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Ollama error %d", code);
            fail(msg);
        }
        return;
    }
    // still starting / connecting / fetching - keep waiting
}
} // namespace tdeckgemini
