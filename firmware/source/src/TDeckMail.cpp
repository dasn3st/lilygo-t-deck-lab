#include "configuration.h"

#if HAS_WIFI

#include "TDeckMail.h"
#include "NodeDB.h" // config.network.wifi_ssid / wifi_psk
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "graphics/common/SdCard.h"

// -----------------------------------------------------------------------------------------
// Gmail over IMAP. STEP ONE ONLY: connect, authenticate, report inbox counts.
//
// Jake, 2026-09-22: "could be a simple email send, inbox, drafts, all that Jaz."
// This is the transport underneath that, built and proven first, because two things could have
// sunk the whole feature and neither is about email:
//
//   1. CERTIFICATE VERIFICATION. tdeck_net_fetch() calls setInsecure() - it does not check who
//      it is talking to - with the comment "no room for a CA bundle". For a public read-only
//      API that is a defensible shortcut. For an account password it is not: on a hostile
//      network anything in the middle could present its own certificate and keep the password.
//      ⭐ We do not need a BUNDLE, only the ONE root Gmail chains to, which is ~1.9KB.
//   2. HEAP. Measured on Jake's device: internal free 37352, largest contiguous block 20468,
//      all-time low 18212. The handshake wants ~34KB contiguous (two ~16.7KB record buffers),
//      which is why tdeck_tls_reserve_init() holds 36KB aside at boot. IMAP pays the same cost
//      as HTTPS, so it uses the same reserve - it must not open a session while a fetch is
//      running, and it releases and retakes the reserve exactly as netHttpGet does.
//
// ⛔ EVERYTHING HERE RUNS FROM loop(), NEVER FROM THE UI TASK. tdeck_mail_service() is called
// from main.cpp's loop alongside tdeck_net_service(). The UI only ever records an intent and
// reads a result. This is the same rule as the rest of the device: the "tft" task holds spiLock
// while it runs, and a blocking socket read on it freezes the screen.
// -----------------------------------------------------------------------------------------

void disableBluetooth(); // ⛔ one antenna: wi-fi cannot join while BT holds the radio
extern "C" bool tdeck_wifi_connect_now(const char *ssid, const char *psk);
extern "C" void tdeck_wifi_disconnect_now(void);
extern "C" bool tdeck_wifi_connected(void);
extern "C" void tdeck_tls_reserve_release(void);
extern "C" void tdeck_tls_reserve_take(void);

// GTS Root R1, fetched from https://pki.goog/repo/certs/gtsr1.pem and checked against the chain
// imap.gmail.com actually serves:
//     imap.gmail.com  ->  WR2  ->  GTS Root R1
// ⚠️ The copy in the served chain has a DIFFERENT sha256 to this one and that is correct, not a
// mismatch: the server sends the variant cross-signed by GlobalSign, for older trust stores.
// Both carry the SAME public key (verified: 7a9f8103eacb08443605ff801479654fd23b67e68a75364da
// ffa25405ce08273), which is the part that has to match - mbedTLS verifies WR2's signature with
// that key and the chain completes. Valid to 2036-06-22.
static const char kGtsRootR1[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
    "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
    "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
    "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
    "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
    "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
    "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
    "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
    "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
    "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
    "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
    "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
    "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
    "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
    "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
    "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
    "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
    "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
    "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
    "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
    "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
    "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
    "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
    "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
    "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
    "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
    "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
    "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n"
    ;

static const char *kImapHost = "imap.gmail.com";
static const int kImapPort = 993;
static const char *kSmtpHost = "smtp.gmail.com";
static const int kSmtpPort = 465; // implicit TLS, so the same certificate check applies

// ---- state, all touched only from loop() except the volatile handshake with the UI ----------
enum MailState { MAIL_IDLE = 0, MAIL_START, MAIL_CONNECTING, MAIL_WORK, MAIL_DONE, MAIL_ERROR };
static volatile int s_state = MAIL_IDLE;
static volatile bool s_pending = false;
static bool s_ownWifi = false;
static uint32_t s_deadline = 0;
static int s_total = -1, s_unseen = -1;
static char s_err[96] = {0};

// Credentials live ONLY on the SD card, never compiled in - the same rule as the Gemini key, so
// the installer stays shareable. /gmail.txt is two lines: address, then the 16-character app
// password.
static char s_user[96] = {0};
static char s_pass[64] = {0};

// ⭐ CONNECT-ONLY MODE. The two things that could sink this feature - does certificate
// verification actually work, and does the handshake fit in the heap - have nothing to do with
// anyone's password. This mode does the TLS connect, checks the certificate, reads the IMAP
// greeting and logs out, so the risky half can be PROVEN before a credential exists anywhere.
// It is also the diagnostic to reach for later when mail breaks: it separates "the network or
// the certificate is wrong" from "the login is wrong", which otherwise look identical.
static volatile bool s_connectOnly = false;

// What this session is for. One connection does one job and logs out; keeping a session open
// between actions would mean owning a socket across the UI's lifetime and reconnecting on every
// wi-fi blip anyway.
enum MailOp { OP_CHECK = 0, OP_LIST, OP_READ, OP_SEND };
static volatile int s_op = OP_CHECK;

#define MAIL_PAGE 8 // rows on screen AND the fetch size, deliberately the same number
typedef struct {
    uint32_t seq;
    bool seen;
    char from[54];
    char subj[74];
    char date[18];
} MailHdr;
// PSRAM: a page of headers is ~1.8KB and is only touched while the inbox screen is up.
static MailHdr *s_hdr = nullptr;
static int s_hdrN = 0;
static int s_page = 0;        // 0 = newest page
static uint32_t s_readSeq = 0;
static char *s_bodyBuf = nullptr; // PSRAM - a message body has no business in internal RAM
static const int kBodyCap = 8192; // plenty for reading; a longer mail is truncated with a note
static int s_bodyLen = 0;
static char s_sendTo[96] = {0};
static char s_sendSubj[120] = {0};
static char *s_sendBody = nullptr;

// ⭐ THE MESSAGE BEING READ, as Reply and Forward need it. Taken from the RAW headers of the
// fetched message rather than the inbox list, because the list keeps only 54 characters of the
// From line for display - enough to show a name, not always enough to hold the whole address.
static char s_rdReplyTo[96] = {0};  // Reply-To if the sender set one, otherwise From - the ADDRESS only
static char s_rdFrom[120] = {0};    // the full From line, for "On <date>, <from> wrote:"
static char s_rdDate[64] = {0};
static char s_rdSubj[120] = {0};
static char s_rdMsgId[160] = {0};   // for In-Reply-To, so Gmail threads the reply with the original
static char s_rdRefs[320] = {0};    // References of the original, extended with its Message-ID
// Set for one send: thread it under the message last read.
static bool s_sendAsReply = false;
static char s_sendInReplyTo[160] = {0};
static char s_sendRefs[320] = {0};

// Why the last IMAP command failed, so a DROPPED CONNECTION is never reported as a WRONG
// PASSWORD. imapCmd() used to return one false for both, and LOGIN turned every false into
// "login rejected - check the app password" - Jake was sent to check a password that was fine.
static bool s_imapIoFail = false;   // the connection went quiet or closed mid-command
static char s_imapTagged[120] = {0}; // the server's tagged reply, e.g. "a1 NO [AUTHENTICATIONFAILED] ..."

static void mailFail(const char *why)
{
    strncpy(s_err, why, sizeof(s_err) - 1);
    s_err[sizeof(s_err) - 1] = 0;
    s_state = MAIL_ERROR;
    LOG_INFO("mail: %s", why);
}

// ⭐ GOOGLE SHOWS THE APP PASSWORD AS "abcd efgh ijkl mnop" AND THE SPACES ARE NOT PART OF IT.
// Anyone typing it onto the card copies it the way it is displayed, so strip whitespace rather
// than hand the user a login failure they cannot possibly diagnose.
static void stripSpaces(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != ' ' && *r != '\t' && *r != '\r' && *r != '\n')
            *w++ = *r;
    *w = 0;
}

static bool loadCreds(void)
{
    if (s_user[0] && s_pass[0])
        return true;
    FsFile f = SDFs.open("/gmail.txt", O_RDONLY);
    if (!f) {
        mailFail("no /gmail.txt on the SD card");
        return false;
    }
    int n = f.fgets(s_user, sizeof(s_user));
    int m = n > 0 ? f.fgets(s_pass, sizeof(s_pass)) : 0;
    f.close();
    stripSpaces(s_user);
    stripSpaces(s_pass);
    if (!s_user[0] || !s_pass[0]) {
        s_user[0] = s_pass[0] = 0;
        mailFail("/gmail.txt needs two lines: address, then app password");
        return false;
    }
    (void)m;
    return true;
}

// Read one CRLF line from the socket. Returns false on timeout so a stalled server cannot wedge
// the loop; every caller treats that as a failed session rather than retrying forever.
static bool imapLine(WiFiClientSecure &c, char *buf, int cap, uint32_t timeoutMs = 8000)
{
    int i = 0;
    uint32_t end = millis() + timeoutMs;
    while ((int32_t)(millis() - end) < 0) {
        if (!c.connected() && !c.available())
            break;
        int ch = c.read();
        if (ch < 0) {
            delay(5);
            continue;
        }
        if (ch == '\n') {
            while (i > 0 && buf[i - 1] == '\r')
                i--;
            buf[i] = 0;
            return true;
        }
        if (i < cap - 1)
            buf[i++] = (char)ch;
    }
    buf[0] = 0;
    return false;
}

// Read exactly n bytes of an IMAP literal, keeping the first cap-1 of them. The rest is still
// CONSUMED - leaving it in the socket would desynchronise every reply after it, which shows up
// as unrelated commands mysteriously failing.
static int readLiteral(WiFiClientSecure &c, int n, char *out, int cap)
{
    int got = 0, kept = 0;
    uint32_t end = millis() + 15000;
    while (got < n && (int32_t)(millis() - end) < 0) {
        int ch = c.read();
        if (ch < 0) {
            if (!c.connected() && !c.available())
                break;
            delay(2);
            continue;
        }
        got++;
        if (kept < cap - 1)
            out[kept++] = (char)ch;
    }
    if (out && cap > 0)
        out[kept < cap ? kept : cap - 1] = 0;
    return kept;
}

static int b64val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int b64decode(const char *in, int len, char *out, int cap)
{
    int n = 0, bits = 0, acc = 0;
    for (int i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0)
            continue;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap - 1)
                out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = 0;
    return n;
}

static void b64encode(const char *in, int len, char *out, int cap)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int n = 0;
    for (int i = 0; i < len; i += 3) {
        int a = (unsigned char)in[i];
        int b = i + 1 < len ? (unsigned char)in[i + 1] : 0;
        int cc = i + 2 < len ? (unsigned char)in[i + 2] : 0;
        if (n + 4 >= cap)
            break;
        out[n++] = T[a >> 2];
        out[n++] = T[((a & 3) << 4) | (b >> 4)];
        out[n++] = i + 1 < len ? T[((b & 15) << 2) | (cc >> 6)] : '=';
        out[n++] = i + 2 < len ? T[cc & 63] : '=';
    }
    out[n] = 0;
}

// ⭐ SUBJECTS ARE OFTEN MIME "ENCODED WORDS": =?UTF-8?B?...?= or =?UTF-8?Q?...?=. Without this
// a third of an inbox reads as gibberish punctuation, which looks like a broken app rather than
// an undecoded header. Only the two common encodings; anything else is left as-is.
static void decodeWords(char *s)
{
    char out[160];
    int o = 0;
    const char *p = s;
    while (*p && o < (int)sizeof(out) - 1) {
        const char *start = strstr(p, "=?");
        if (!start) {
            while (*p && o < (int)sizeof(out) - 1)
                out[o++] = *p++;
            break;
        }
        while (p < start && o < (int)sizeof(out) - 1)
            out[o++] = *p++;
        const char *q1 = strchr(start + 2, '?');
        if (!q1) { out[o++] = *p++; continue; }
        const char *q2 = strchr(q1 + 1, '?');
        if (!q2) { out[o++] = *p++; continue; }
        const char *end = strstr(q2 + 1, "?=");
        if (!end) { out[o++] = *p++; continue; }
        const char enc = (char)toupper((unsigned char)q1[1]);
        const int n = (int)(end - (q2 + 1));
        char raw[200];
        int rn = n < (int)sizeof(raw) - 1 ? n : (int)sizeof(raw) - 1;
        memcpy(raw, q2 + 1, rn);
        raw[rn] = 0;
        char dec[200];
        if (enc == 'B') {
            b64decode(raw, rn, dec, sizeof(dec));
        } else { // Q: like quoted-printable, but '_' is a space
            int d = 0;
            for (int i = 0; i < rn && d < (int)sizeof(dec) - 1; i++) {
                if (raw[i] == '_') dec[d++] = ' ';
                else if (raw[i] == '=' && i + 2 < rn) {
                    char h[3] = {raw[i + 1], raw[i + 2], 0};
                    dec[d++] = (char)strtol(h, nullptr, 16);
                    i += 2;
                } else dec[d++] = raw[i];
            }
            dec[d] = 0;
        }
        for (const char *d = dec; *d && o < (int)sizeof(out) - 1; d++)
            out[o++] = *d;
        p = end + 2;
        while (*p == ' ' && p[1] == '=' && p[2] == '?') // encoded words run together
            p++;
    }
    out[o] = 0;
    strncpy(s, out, o + 1);
}

// Pull one header out of a fetched header blob, unfolding continuation lines. `decode` turns
// MIME encoded words into text - right for a subject or a name, wrong for an id.
static void hdrField(const char *blob, const char *name, char *out, int cap, bool decode = true)
{
    out[0] = 0;
    const int nlen = (int)strlen(name);
    for (const char *p = blob; *p;) {
        if (!strncasecmp(p, name, nlen)) {
            p += nlen;
            while (*p == ' ' || *p == '\t')
                p++;
            // ⛔ DECODE FIRST, SHORTEN AFTER. A subject is often one long MIME encoded word
            // ("=?UTF-8?Q?...?="), and cutting it to the display width first chopped off the
            // closing "?=" - so decodeWords() could not find the end and the inbox showed the raw
            // "=?UT..." instead of the words. Seen in Jake's inbox 2026-09-29.
            char tmp[400];
            int o = 0;
            while (*p && o < (int)sizeof(tmp) - 1) {
                if (*p == '\r') { p++; continue; }
                if (*p == '\n') {
                    if (p[1] == ' ' || p[1] == '\t') { p++; tmp[o++] = ' '; continue; } // folded
                    break;
                }
                tmp[o++] = *p++;
            }
            tmp[o] = 0;
            if (decode)
                decodeWords(tmp);
            int n = (int)strlen(tmp);
            if (n > cap - 1) {
                n = cap - 1;
                // and never split a UTF-8 character: back up over continuation bytes
                while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80)
                    n--;
            }
            memcpy(out, tmp, n);
            out[n] = 0;
            return;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
}

// Send "<tag> <cmd>" and read until the line that starts with that tag. Returns true on OK.
// `capture`, if given, keeps the first untagged line containing `want`, for the STATUS reply.
static bool imapCmd(WiFiClientSecure &c, const char *tag, const char *cmd, const char *want = nullptr,
                    char *capture = nullptr, int capN = 0)
{
    c.printf("%s %s\r\n", tag, cmd);
    char line[320];
    s_imapIoFail = false;
    s_imapTagged[0] = 0;
    for (int guard = 0; guard < 60; guard++) {
        if (!imapLine(c, line, sizeof(line))) {
            s_imapIoFail = true;
            return false;
        }
        if (want && capture && strstr(line, want)) {
            strncpy(capture, line, capN - 1);
            capture[capN - 1] = 0;
        }
        // The tagged response ends the command. Anything before it is untagged chatter.
        if (!strncmp(line, tag, strlen(tag)) && line[strlen(tag)] == ' ') {
            strncpy(s_imapTagged, line, sizeof(s_imapTagged) - 1);
            s_imapTagged[sizeof(s_imapTagged) - 1] = 0;
            return strstr(line + strlen(tag), " OK") == line + strlen(tag);
        }
    }
    s_imapIoFail = true; // 60 lines and never the tagged reply: the conversation is out of step
    return false;
}

// One whole IMAP session, blocking, with short timeouts. Wi-Fi is already up when this runs.
// ---- list one page of the inbox -------------------------------------------------------------
//
// ⭐ ASKS THE SERVER FOR A RANGE, never for everything. With 7,000 messages the difference is
// not performance, it is whether the device survives at all: the newest twelve are sequence
// numbers 6989..7000, the page after that 6977..6988. Memory cost is identical at any inbox size.
static bool imapList(WiFiClientSecure &c)
{
    char st[400] = {0};
    if (!imapCmd(c, "b1", "SELECT INBOX", "EXISTS", st, sizeof(st))) {
        mailFail("could not open the inbox");
        return false;
    }
    if (!s_hdr) {
        s_hdr = (MailHdr *)heap_caps_calloc(MAIL_PAGE, sizeof(MailHdr), MALLOC_CAP_SPIRAM);
        if (!s_hdr)
            s_hdr = (MailHdr *)calloc(MAIL_PAGE, sizeof(MailHdr));
        if (!s_hdr) {
            mailFail("out of memory for the message list");
            return false;
        }
    }
    const int total = atoi(st + 2); // "* 6994 EXISTS"
    s_total = total;
    s_hdrN = 0;
    if (total <= 0)
        return true;

    int hi = total - s_page * MAIL_PAGE;
    if (hi <= 0)
        return true; // paged past the oldest message
    int lo = hi - MAIL_PAGE + 1;
    if (lo < 1)
        lo = 1;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "b2 FETCH %d:%d (FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])\r\n", lo, hi);
    c.print(cmd);

    char line[420];
    while (imapLine(c, line, sizeof(line), 12000)) {
        if (!strncmp(line, "b2 ", 3))
            break; // tagged response: this command is finished
        if (line[0] != '*')
            continue;
        const char *brace = strrchr(line, '{');
        if (!brace)
            continue; // an untagged line with no literal - not one of our FETCH replies
        const uint32_t seq = (uint32_t)atoi(line + 2);
        const bool seen = strstr(line, "\\Seen") != nullptr;
        char hb[560];
        readLiteral(c, atoi(brace + 1), hb, sizeof(hb));
        if (s_hdrN < MAIL_PAGE) {
            MailHdr *h = &s_hdr[s_hdrN++];
            h->seq = seq;
            h->seen = seen;
            hdrField(hb, "From:", h->from, sizeof(h->from));
            hdrField(hb, "Subject:", h->subj, sizeof(h->subj));
            hdrField(hb, "Date:", h->date, sizeof(h->date));
            if (!h->subj[0])
                strncpy(h->subj, "(no subject)", sizeof(h->subj) - 1);
        }
    }
    // The server returns lo..hi ascending, so the newest is last. Reverse it: a mail app that
    // shows the oldest message of the page first is simply wrong.
    for (int i = 0, j = s_hdrN - 1; i < j; i++, j--) {
        MailHdr t = s_hdr[i];
        s_hdr[i] = s_hdr[j];
        s_hdr[j] = t;
    }
    LOG_INFO("mail: page %d -> %d headers of %d", s_page, s_hdrN, total);
    return true;
}

// Turn a raw RFC822 message into something readable. Handles the three cases that cover almost
// everything: plain text, multipart with a text/plain part, and HTML-only.
static void extractText(char *raw, int len)
{
    if (!s_bodyBuf)
        return;
    // Find the MIME boundary, if this is a multipart message.
    char boundary[80] = {0};
    const char *b = strcasestr(raw, "boundary=");
    if (b) {
        b += 9;
        if (*b == '"')
            b++;
        int i = 0;
        while (*b && *b != '"' && *b != '\r' && *b != '\n' && *b != ';' && i < (int)sizeof(boundary) - 1)
            boundary[i++] = *b++;
        boundary[i] = 0;
    }

    const char *text = nullptr;
    int textLen = 0;
    bool base64 = false, qp = false, html = false;
    int attachments = 0;

    if (boundary[0]) {
        char sep[88];
        snprintf(sep, sizeof(sep), "--%s", boundary);
        char *p = raw;
        while ((p = strstr(p, sep)) != nullptr) {
            p += strlen(sep);
            char *hdrEnd = strstr(p, "\r\n\r\n");
            if (!hdrEnd)
                break;
            *hdrEnd = 0; // temporarily terminate this part's headers so the searches below stop
            const bool isPlain = strcasestr(p, "text/plain") != nullptr;
            const bool isHtml = strcasestr(p, "text/html") != nullptr;
            const bool isB64 = strcasestr(p, "base64") != nullptr;
            const bool isQp = strcasestr(p, "quoted-printable") != nullptr;
            const bool isAttach = strcasestr(p, "attachment") != nullptr || strcasestr(p, "filename=") != nullptr;
            *hdrEnd = '\r';
            if (isAttach)
                attachments++;
            if (!text && (isPlain || (isHtml && !isPlain))) {
                char *bodyStart = hdrEnd + 4;
                char *next = strstr(bodyStart, sep);
                text = bodyStart;
                textLen = next ? (int)(next - bodyStart) : (int)(len - (bodyStart - raw));
                base64 = isB64;
                qp = isQp;
                html = isHtml && !isPlain;
                if (isPlain)
                    continue; // a plain part beats a later html one, but keep counting attachments
            }
            p = hdrEnd + 4;
        }
    }
    if (!text) {
        // Not multipart (or nothing matched): the body is whatever follows the blank line.
        char *bodyStart = strstr(raw, "\r\n\r\n");
        text = bodyStart ? bodyStart + 4 : raw;
        textLen = len - (int)(text - raw);
        base64 = strcasestr(raw, "base64") != nullptr;
        qp = strcasestr(raw, "quoted-printable") != nullptr;
        html = strcasestr(raw, "text/html") != nullptr;
    }
    if (textLen < 0)
        textLen = 0;

    int o = 0;
    if (base64) {
        o = b64decode(text, textLen, s_bodyBuf, kBodyCap);
    } else if (qp) {
        for (int i = 0; i < textLen && o < kBodyCap - 1; i++) {
            if (text[i] == '=' && i + 1 < textLen && (text[i + 1] == '\r' || text[i + 1] == '\n')) {
                while (i + 1 < textLen && (text[i + 1] == '\r' || text[i + 1] == '\n'))
                    i++;
                continue; // soft line break: the '=' and the newline both vanish
            }
            if (text[i] == '=' && i + 2 < textLen) {
                char h[3] = {text[i + 1], text[i + 2], 0};
                s_bodyBuf[o++] = (char)strtol(h, nullptr, 16);
                i += 2;
                continue;
            }
            s_bodyBuf[o++] = text[i];
        }
    } else {
        for (int i = 0; i < textLen && o < kBodyCap - 1; i++)
            s_bodyBuf[o++] = text[i];
    }
    s_bodyBuf[o] = 0;

    if (html) {
        // Crudely de-tag. Jake asked for "simple text" and a real renderer is not on the table;
        // this at least turns a marketing email into readable sentences rather than markup.
        int w = 0;
        bool in = false;
        for (int i = 0; s_bodyBuf[i]; i++) {
            if (s_bodyBuf[i] == '<') { in = true; continue; }
            if (s_bodyBuf[i] == '>') { in = false; s_bodyBuf[w++] = ' '; continue; }
            if (!in)
                s_bodyBuf[w++] = s_bodyBuf[i];
        }
        s_bodyBuf[w] = 0;
        o = w;
    }
    if (attachments && o < kBodyCap - 40)
        o += snprintf(s_bodyBuf + o, kBodyCap - o, "\n\n[%d attachment%s - not viewable here]",
                      attachments, attachments == 1 ? "" : "s");
    s_bodyLen = o;
}

// The address inside "Name <addr>", or the whole thing if there are no angle brackets.
static void bareAddress(const char *in, char *out, int cap)
{
    const char *lt = strchr(in, '<');
    const char *gt = lt ? strchr(lt, '>') : nullptr;
    const char *s = lt && gt ? lt + 1 : in;
    int n = lt && gt ? (int)(gt - s) : (int)strlen(in);
    while (n > 0 && (*s == ' ' || *s == '"')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"'))
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

// ⭐ HEADERS ONLY. The search stops at the blank line that ends them: a forwarded message quoted
// in the BODY has its own "From:" lines, and replying to one of those instead of the sender would
// be a quiet, embarrassing misdelivery.
static void captureReplyHeaders(char *raw)
{
    char *end = strstr(raw, "\r\n\r\n");
    if (!end)
        end = strstr(raw, "\n\n");
    char saved = 0;
    if (end) {
        saved = *end;
        *end = 0;
    }
    char tmp[160];
    hdrField(raw, "Reply-To:", tmp, sizeof(tmp));
    if (!tmp[0])
        hdrField(raw, "From:", tmp, sizeof(tmp));
    bareAddress(tmp, s_rdReplyTo, sizeof(s_rdReplyTo));
    hdrField(raw, "From:", s_rdFrom, sizeof(s_rdFrom));
    hdrField(raw, "Date:", s_rdDate, sizeof(s_rdDate));
    hdrField(raw, "Subject:", s_rdSubj, sizeof(s_rdSubj));
    // ⛔ RAW, NOT DECODED: hdrField() runs every value through decodeWords(), whose buffer is 160
    // characters - a long References chain came back cut off mid-id, and a broken id threads
    // nothing. These are ids, never encoded words, so they are copied as they are.
    hdrField(raw, "Message-ID:", s_rdMsgId, sizeof(s_rdMsgId), false);
    char refs[320];
    hdrField(raw, "References:", refs, sizeof(refs), false);
    // References = the original's References plus its own Message-ID. If the chain is too long
    // to carry whole, the parent's id alone is what RFC 5322 falls back to - a truncated chain
    // would have lost the NEWEST ids, which are the ones threading matches on.
    if (strlen(refs) < sizeof(refs) - 2 && strlen(refs) + strlen(s_rdMsgId) + 2 < sizeof(s_rdRefs))
        snprintf(s_rdRefs, sizeof(s_rdRefs), "%s%s%s", refs, refs[0] ? " " : "", s_rdMsgId);
    else
        strncpy(s_rdRefs, s_rdMsgId, sizeof(s_rdRefs) - 1);
    if (end)
        *end = saved;
}

static bool imapReadOne(WiFiClientSecure &c)
{
    s_rdReplyTo[0] = s_rdFrom[0] = s_rdDate[0] = s_rdSubj[0] = s_rdMsgId[0] = s_rdRefs[0] = 0;
    if (!imapCmd(c, "c1", "SELECT INBOX")) {
        mailFail("could not open the inbox");
        return false;
    }
    if (!s_bodyBuf) {
        s_bodyBuf = (char *)heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM);
        if (!s_bodyBuf)
            s_bodyBuf = (char *)malloc(kBodyCap);
        if (!s_bodyBuf) {
            mailFail("out of memory for the message");
            return false;
        }
    }
    // BODY.PEEK, not BODY: PEEK does not set the \Seen flag. Opening a message on the T-Deck
    // should not silently mark 7,000 unread emails as read one careless tap at a time.
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "c2 FETCH %u (BODY.PEEK[])\r\n", (unsigned)s_readSeq);
    c.print(cmd);

    char line[420];
    bool got = false;
    while (imapLine(c, line, sizeof(line), 15000)) {
        if (!strncmp(line, "c2 ", 3))
            break;
        if (line[0] != '*' || got)
            continue;
        const char *brace = strrchr(line, '{');
        if (!brace)
            continue;
        const int n = atoi(brace + 1);
        char *raw = (char *)heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM);
        if (!raw) {
            mailFail("out of memory for the message");
            return false;
        }
        const int kept = readLiteral(c, n, raw, kBodyCap);
        captureReplyHeaders(raw);
        extractText(raw, kept);
        heap_caps_free(raw);
        if (n > kBodyCap)
            LOG_INFO("mail: message %u is %d bytes, showing the first %d", (unsigned)s_readSeq, n, kBodyCap);
        got = true;
    }
    if (!got) {
        mailFail("could not read that message");
        return false;
    }
    LOG_INFO("mail: read message %u, %d chars of text", (unsigned)s_readSeq, s_bodyLen);
    return true;
}

// ---- sending -------------------------------------------------------------------------------
// A separate connection to a separate host, so it does not share imapCheck's session. Same
// certificate: smtp.gmail.com chains to the same Google root as imap.gmail.com.
static bool smtpReply(WiFiClientSecure &c, int want)
{
    // SMTP replies can be multi-line: "250-SIZE" then "250-8BITMIME" then "250 HELP". Only the
    // line with a SPACE after the code is the last one - stopping at the first line makes every
    // command after EHLO read the wrong reply.
    char line[300];
    for (int guard = 0; guard < 40; guard++) {
        if (!imapLine(c, line, sizeof(line), 12000))
            return false;
        if (strlen(line) < 4)
            continue;
        if (line[3] == ' ') {
            const int code = atoi(line);
            if (code != want)
                LOG_INFO("smtp: expected %d, got: %.80s", want, line);
            return code == want;
        }
    }
    return false;
}

static bool smtpSend(void)
{
    if (!loadCreds())
        return false;
    tdeck_tls_reserve_release();
    bool ok = false;
    WiFiClientSecure *c = new WiFiClientSecure();
    if (!c) {
        mailFail("out of memory for the TLS client");
        tdeck_tls_reserve_take();
        return false;
    }
    c->setCACert(kGtsRootR1);
    c->setTimeout(10000);
    c->setHandshakeTimeout(15); // library default is 120s, past the ~90s watchdog - see TDeckNet.cpp
    LOG_INFO("smtp: connecting to %s:%d", kSmtpHost, kSmtpPort);
    if (!c->connect(kSmtpHost, kSmtpPort)) {
        mailFail("could not reach the mail server");
        goto done;
    }
    if (!smtpReply(*c, 220)) {
        mailFail("no greeting from the mail server");
        goto done;
    }
    c->print("EHLO tdeck\r\n");
    if (!smtpReply(*c, 250)) {
        mailFail("mail server refused EHLO");
        goto done;
    }
    {
        char b64[160];
        c->print("AUTH LOGIN\r\n");
        if (!smtpReply(*c, 334)) {
            mailFail("mail server refused AUTH");
            goto done;
        }
        b64encode(s_user, (int)strlen(s_user), b64, sizeof(b64));
        c->printf("%s\r\n", b64);
        if (!smtpReply(*c, 334)) {
            mailFail("mail server rejected the address");
            goto done;
        }
        // ⛔ The password goes over the wire here and NOWHERE ELSE. Not logged, and the base64
        // of it is wiped off the stack immediately - it is trivially reversible, so it is just
        // the password in another coat.
        b64encode(s_pass, (int)strlen(s_pass), b64, sizeof(b64));
        c->printf("%s\r\n", b64);
        const bool authed = smtpReply(*c, 235);
        memset(b64, 0, sizeof(b64));
        if (!authed) {
            mailFail("login rejected when sending");
            goto done;
        }
    }
    c->printf("MAIL FROM:<%s>\r\n", s_user);
    if (!smtpReply(*c, 250)) {
        mailFail("sender rejected");
        goto done;
    }
    c->printf("RCPT TO:<%s>\r\n", s_sendTo);
    if (!smtpReply(*c, 250)) {
        mailFail("that address was rejected");
        goto done;
    }
    c->print("DATA\r\n");
    if (!smtpReply(*c, 354)) {
        mailFail("server would not accept the message");
        goto done;
    }
    c->printf("From: <%s>\r\n", s_user);
    c->printf("To: <%s>\r\n", s_sendTo);
    {
        // ⚠️ A HEADER MUST BE 7-BIT. A reply copies the original subject, and that is often
        // UTF-8 (an emoji, an accented name); sent raw, some servers reject it and some mangle it.
        // RFC 2047 "encoded word" when anything is outside ASCII - the same form decodeWords()
        // reads on the way in.
        bool ascii = true;
        for (const char *p = s_sendSubj; *p; p++)
            if ((unsigned char)*p > 126)
                ascii = false;
        if (ascii) {
            c->printf("Subject: %s\r\n", s_sendSubj);
        } else {
            char b64[180];
            b64encode(s_sendSubj, (int)strlen(s_sendSubj), b64, sizeof(b64));
            c->printf("Subject: =?UTF-8?B?%s?=\r\n", b64);
        }
    }
    if (s_sendAsReply && s_sendInReplyTo[0]) {
        // What makes Gmail file the reply in the same conversation as the original.
        c->printf("In-Reply-To: %s\r\n", s_sendInReplyTo);
        c->printf("References: %s\r\n", s_sendRefs[0] ? s_sendRefs : s_sendInReplyTo);
    }
    c->print("MIME-Version: 1.0\r\n");
    c->print("Content-Type: text/plain; charset=utf-8\r\n");
    c->print("Content-Transfer-Encoding: 8bit\r\n\r\n");
    if (s_sendBody) {
        // ⚠️ DOT-STUFFING. A line consisting of a single "." ends the message, so a body
        // containing one would truncate the mail and leave the rest as garbage commands. Any
        // line starting with "." gets a second one, which the receiver strips. Rare, and
        // silently corrupting when missed.
        //
        // ⚠️ AND CRLF. The textarea gives bare "\n", and SMTP lines end "\r\n". Gmail has been
        // forgiving; plenty of servers now reject a bare LF outright (it is how SMTP smuggling
        // works), and a reply goes wherever the other person's mail lives.
        const char *p = s_sendBody;
        bool lineStart = true;
        char prev = 0;
        while (*p) {
            if (lineStart && *p == '.')
                c->print('.');
            if (*p == '\n' && prev != '\r')
                c->print('\r');
            c->print(*p);
            lineStart = (*p == '\n');
            prev = *p;
            p++;
        }
    }
    c->print("\r\n.\r\n");
    if (!smtpReply(*c, 250)) {
        mailFail("the server did not accept the message");
        goto done;
    }
    c->print("QUIT\r\n");
    ok = true;
    LOG_INFO("smtp: sent to %s", s_sendTo);
done:
    c->stop();
    delete c;
    tdeck_tls_reserve_take();
    return ok;
}

static bool imapCheck(void)
{
    // Same reserve the HTTPS path uses, for the same reason and with the same ordering: release
    // before the handshake, retake only after the client is DELETED, because the handshake
    // buffers are held by the client and re-reserving early just fails.
    tdeck_tls_reserve_release();
    bool ok = false;
    WiFiClientSecure *c = new WiFiClientSecure();
    if (!c) {
        mailFail("out of memory for the TLS client");
        tdeck_tls_reserve_take();
        return false;
    }
    // ⭐ THE WHOLE POINT. Not setInsecure(): a password is going over this socket.
    c->setCACert(kGtsRootR1);
    c->setTimeout(8000);
    c->setHandshakeTimeout(15); // library default is 120s, past the ~90s watchdog - see TDeckNet.cpp
    // ⭐ RETRY THE CONNECT. The FIRST TLS connect after wi-fi has just come up fails often enough
    // that one attempt is not a fair test of anything - measured on this device with Get Apps,
    // where attempt 1 returned -11 and attempt 2 returned the catalog, seconds apart. Jake hit
    // the same thing on battery: "could not connect or certificate rejected" on a device with
    // plenty of memory and a working network.
    //
    // DHCP and DNS are up before this runs, but "up" and "ready to complete a handshake" are not
    // the same instant, and on battery the CPU is slower getting there. Three tries across ~5
    // seconds costs nothing when the first succeeds and turns a spurious failure into a working
    // mailbox when it does not.
    //
    // ⚠️ A FRESH CLIENT EACH TIME. A WiFiClientSecure that has failed a handshake is not
    // guaranteed to be reusable, and retrying on the same object is the kind of thing that works
    // on the bench and fails in the field.
    bool connected = false;
    for (int attempt = 1; attempt <= 3 && !connected; attempt++) {
        if (attempt > 1) {
            delete c;
            delay(1500);
            c = new WiFiClientSecure();
            if (!c) {
                mailFail("out of memory for the TLS client");
                tdeck_tls_reserve_take();
                return false;
            }
            c->setCACert(kGtsRootR1);
            c->setTimeout(8000);
            c->setHandshakeTimeout(15);
        }
        LOG_INFO("mail: connecting to %s:%d, attempt %d (largest usable internal block %u)", kImapHost, kImapPort, attempt,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        connected = c->connect(kImapHost, kImapPort);
        if (!connected)
            LOG_INFO("mail: attempt %d did not connect", attempt);
    }
    if (!connected) {
        // A certificate failure and a network failure look the same from here, so say both
        // rather than send the user hunting the wrong one.
        //
        // ⛔ THIS ADVICE WAS WRONG AND JAKE CAUGHT IT: "it says 'not enough memory, close other
        // apps to continue' - shouldn't the mail app be the only app open?" He was right to
        // doubt it, and the message was wrong in three separate ways:
        //
        //  1. "close other apps" CANNOT HELP. LVGL allocates its screens from PSRAM
        //     (LV_MEM_POOL_ALLOC -> MALLOC_CAP_SPIRAM), so leaving Maps or Settings open costs
        //     no internal RAM at all. The one real exception is chess, which holds a 10KB task
        //     stack. Telling him to close apps sent him to do something that does nothing.
        //  2. The 34KB threshold came from believing the handshake needs one 34KB block. It
        //     needs TWO of about 16.7KB - see the reserve in TDeckNet.cpp. The largest block on
        //     a settled heap is ~16KB, so this test was true on virtually every failure and
        //     relabelled ordinary network errors as memory errors.
        //  3. It measured MALLOC_CAP_INTERNAL while the allocation uses INTERNAL|8BIT. Some
        //     internal RAM is 32-bit-only, so the number was flattering.
        //
        // Now: the right caps, a threshold that matches one record buffer, and advice the user
        // can actually act on - the reserve re-acquires itself within a few seconds, so waiting
        // really is the fix.
        {
            const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            LOG_INFO("mail: connect failed, largest usable internal block %u", (unsigned)largest);
            if (largest < 17u * 1024u)
                mailFail("not enough memory just now - wait a few seconds and try again");
            else
                mailFail("could not connect (network, or certificate rejected)");
        }
        goto done;
    }
    {
        char line[320];
        if (!imapLine(*c, line, sizeof(line)) || strncmp(line, "* OK", 4)) {
            mailFail("no IMAP greeting");
            goto done;
        }
        LOG_INFO("mail: connected, TLS up and certificate verified");
        if (s_connectOnly) {
            // The point of this mode: the certificate and the heap are now proven. Stop here
            // rather than touching an account at all.
            imapCmd(*c, "a1", "LOGOUT");
            s_total = s_unseen = -1;
            ok = true;
            LOG_INFO("mail: CONNECT-ONLY OK - certificate verified, handshake fits");
            goto done;
        }

        // ⛔ NEVER LOG THE PASSWORD, and never log the command that carries it. That is why this
        // builds the LOGIN line separately instead of going through a logging helper.
        {
            char cmd[192];
            snprintf(cmd, sizeof(cmd), "LOGIN \"%s\" \"%s\"", s_user, s_pass);
            bool li = imapCmd(*c, "a1", cmd);
            memset(cmd, 0, sizeof(cmd)); // do not leave it on the stack
            if (!li) {
                // ⛔ THREE DIFFERENT FAILURES, THREE DIFFERENT MESSAGES. Only Gmail saying
                // AUTHENTICATIONFAILED means the password is wrong; a connection that dropped
                // mid-login is a retry, and anything else is Gmail's own words, which say more
                // than a guess would. (The tagged reply never contains the password - it is the
                // SERVER's line, "a1 NO [...] text".)
                if (s_imapIoFail) {
                    mailFail("connection dropped while signing in - try again");
                } else if (strstr(s_imapTagged, "AUTHENTICATIONFAILED")) {
                    mailFail("login rejected - check the app password, and that 2FA is on");
                } else {
                    char why[96];
                    const char *t = strchr(s_imapTagged, ' ');
                    snprintf(why, sizeof(why), "Gmail said:%s", t ? t : " (nothing)");
                    mailFail(why);
                }
                goto done;
            }
        }
        LOG_INFO("mail: logged in");

        // One session, one job, then log out.
        if (s_op == OP_LIST) {
            ok = imapList(*c);
        } else if (s_op == OP_READ) {
            ok = imapReadOne(*c);
        } else {
            // STATUS rather than SELECT: one round trip, gives both numbers, and does not mark
            // anything read. RFC 3501 says not to STATUS the mailbox you have SELECTed - we have
            // selected nothing, so this is the clean order.
            char st[320] = {0};
            if (!imapCmd(*c, "a2", "STATUS INBOX (MESSAGES UNSEEN)", "STATUS", st, sizeof(st))) {
                mailFail("STATUS failed");
                goto done;
            }
            const char *m = strstr(st, "MESSAGES ");
            const char *u = strstr(st, "UNSEEN ");
            s_total = m ? atoi(m + 9) : -1;
            s_unseen = u ? atoi(u + 7) : -1;
            ok = true;
            LOG_INFO("mail: INBOX total=%d unseen=%d", s_total, s_unseen);
        }
        imapCmd(*c, "a9", "LOGOUT");
    }
done:
    c->stop();
    delete c;
    tdeck_tls_reserve_take();
    return ok;
}

// ---- app-facing, called from the UI task: record intent, read results -----------------------

// The setup form just rewrote /gmail.txt, so the cached copy is stale. Also the safe thing to
// call after a failed login: it forces a fresh read rather than retrying the same wrong pair.
extern "C" void tdeck_mail_forget_creds(void)
{
    memset(s_user, 0, sizeof(s_user));
    memset(s_pass, 0, sizeof(s_pass)); // wiped, not just marked empty
}

extern "C" bool tdeck_mail_check(void)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_op = OP_CHECK;
    s_pending = true;
    return true;
}

extern "C" bool tdeck_mail_connect_test(void)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = true;
    s_op = OP_CHECK;
    s_pending = true;
    return true;
}

extern "C" bool tdeck_mail_list(int page)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_op = OP_LIST;
    s_page = page < 0 ? 0 : page;
    s_pending = true;
    return true;
}

extern "C" int tdeck_mail_list_count(void)
{
    return s_hdrN;
}

extern "C" int tdeck_mail_total(void)
{
    return s_total;
}

extern "C" int tdeck_mail_page(void)
{
    return s_page;
}

extern "C" bool tdeck_mail_item(int i, unsigned *seq, const char **from, const char **subj, const char **date,
                                bool *seen)
{
    if (!s_hdr || i < 0 || i >= s_hdrN)
        return false;
    if (seq)
        *seq = s_hdr[i].seq;
    if (from)
        *from = s_hdr[i].from;
    if (subj)
        *subj = s_hdr[i].subj;
    if (date)
        *date = s_hdr[i].date;
    if (seen)
        *seen = s_hdr[i].seen;
    return true;
}

extern "C" bool tdeck_mail_read(unsigned seq)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_op = OP_READ;
    s_readSeq = seq;
    s_bodyLen = 0;
    s_pending = true;
    return true;
}

extern "C" const char *tdeck_mail_body(void)
{
    return (s_bodyBuf && s_bodyLen) ? s_bodyBuf : "";
}

// ⭐ SENDS ONLY TO THE SIGNED-IN ACCOUNT'S OWN ADDRESS, and that is deliberate. This exists so
// the SMTP path can be proven over the cable, and a diagnostic that could send to an arbitrary
// address would be a way to send mail as the owner from anything that can reach this port.
// It takes no recipient, so there is nothing to point somewhere else.
extern "C" bool tdeck_mail_send_selftest(void)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    if (!loadCreds())
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_op = OP_SEND;
    strncpy(s_sendTo, s_user, sizeof(s_sendTo) - 1); // to yourself, always
    s_sendTo[sizeof(s_sendTo) - 1] = 0;
    strncpy(s_sendSubj, "T-Deck test", sizeof(s_sendSubj) - 1);
    if (s_sendBody)
        free(s_sendBody);
    s_sendBody = strdup("Sent from the T-Deck.\r\n\r\n"
                        "If you are reading this, sending works: SMTP over TLS to Gmail, "
                        "authenticated with the app password on the SD card, with the server's "
                        "certificate checked against Google's own root.\r\n");
    s_pending = true;
    return true;
}

extern "C" bool tdeck_mail_send_ex(const char *to, const char *subject, const char *body, bool asReply)
{
    if (s_state == MAIL_START || s_state == MAIL_CONNECTING || s_state == MAIL_WORK)
        return false;
    if (!to || !strchr(to, '@'))
        return false;
    s_err[0] = 0;
    s_connectOnly = false;
    s_op = OP_SEND;
    strncpy(s_sendTo, to, sizeof(s_sendTo) - 1);
    s_sendTo[sizeof(s_sendTo) - 1] = 0;
    // A pasted or typed address can carry spaces or a trailing newline; RCPT TO will not have them.
    {
        char *w = s_sendTo;
        for (const char *r = s_sendTo; *r; r++)
            if (*r != ' ' && *r != '\r' && *r != '\n' && *r != '<' && *r != '>')
                *w++ = *r;
        *w = 0;
    }
    strncpy(s_sendSubj, subject ? subject : "", sizeof(s_sendSubj) - 1);
    s_sendSubj[sizeof(s_sendSubj) - 1] = 0;
    s_sendAsReply = asReply && s_rdMsgId[0];
    strncpy(s_sendInReplyTo, s_sendAsReply ? s_rdMsgId : "", sizeof(s_sendInReplyTo) - 1);
    strncpy(s_sendRefs, s_sendAsReply ? s_rdRefs : "", sizeof(s_sendRefs) - 1);
    if (s_sendBody)
        free(s_sendBody);
    // PSRAM: a reply quoting the original is a few KB, which malloc would put in the internal heap.
    const size_t n = strlen(body ? body : "") + 1;
    s_sendBody = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!s_sendBody)
        s_sendBody = (char *)malloc(n);
    if (!s_sendBody)
        return false;
    memcpy(s_sendBody, body ? body : "", n);
    s_pending = true;
    return true;
}

extern "C" bool tdeck_mail_send(const char *to, const char *subject, const char *body)
{
    return tdeck_mail_send_ex(to, subject, body, false);
}

// The message last opened, for Reply and Forward. Empty strings until one has been read.
extern "C" const char *tdeck_mail_read_reply_addr(void) { return s_rdReplyTo; }
extern "C" const char *tdeck_mail_read_from(void) { return s_rdFrom; }
extern "C" const char *tdeck_mail_read_date(void) { return s_rdDate; }
extern "C" const char *tdeck_mail_read_subject(void) { return s_rdSubj; }

extern "C" int tdeck_mail_poll(void)
{
    if (s_state == MAIL_DONE)
        return 1;
    if (s_state == MAIL_ERROR)
        return -1;
    return 0;
}

extern "C" void tdeck_mail_counts(int *total, int *unseen)
{
    if (total)
        *total = s_total;
    if (unseen)
        *unseen = s_unseen;
}

extern "C" const char *tdeck_mail_error(void)
{
    return s_err;
}

// ---- the worker, called from loop() ---------------------------------------------------------

extern "C" void tdeck_mail_service(void)
{
    if (s_pending && (s_state == MAIL_IDLE || s_state == MAIL_DONE || s_state == MAIL_ERROR)) {
        s_pending = false;
        s_state = MAIL_START;
    }

    switch (s_state) {
    case MAIL_START: {
        if (!s_connectOnly && !loadCreds())
            break; // loadCreds already set the error and the state
        if (tdeck_wifi_connected()) {
            s_state = MAIL_WORK;
            break;
        }
        // ⛔ DROP BLUETOOTH FIRST. One antenna - wi-fi will not join while BT has the radio, and
        // the only symptom is a 15-second timeout and "wi-fi did not connect", which reads like a
        // router problem. TDeckNet does this for its own fetches; mail did not, so mail worked
        // ONLY when something else (Weather, Gemini) had already torn BT down. Latched, because
        // repeatedly deinit-ing an already-down stack is a place double-frees hide. BT stays down
        // until the next reboot either way - the same trade Jake already accepted for Gemini.
        {
            static bool s_btDown = false;
            if (!s_btDown) {
                LOG_INFO("mail: dropping Bluetooth so wi-fi can have the radio");
                disableBluetooth();
                s_btDown = true;
            }
        }
        if (!tdeck_wifi_connect_now(config.network.wifi_ssid, config.network.wifi_psk)) {
            mailFail("no wi-fi configured");
            break;
        }
        s_ownWifi = true;
        s_deadline = millis() + 15000;
        s_state = MAIL_CONNECTING;
        break;
    }
    case MAIL_CONNECTING: {
        if (tdeck_wifi_connected()) {
            delay(500); // let DHCP/DNS settle before the handshake
            s_state = MAIL_WORK;
        } else if ((int32_t)(millis() - s_deadline) > 0) {
            tdeck_wifi_disconnect_now();
            s_ownWifi = false;
            mailFail("wi-fi did not connect");
        }
        break;
    }
    case MAIL_WORK: {
        // Sending talks to a different host entirely, so it does not go through the IMAP session.
        bool ok = (s_op == OP_SEND) ? smtpSend() : imapCheck();
        if (s_ownWifi) {
            tdeck_wifi_disconnect_now();
            s_ownWifi = false;
        }
        if (ok)
            s_state = MAIL_DONE;
        else if (s_state != MAIL_ERROR)
            mailFail("mail check failed");
        break;
    }
    default:
        break;
    }
}

#else // !HAS_WIFI
extern "C" bool tdeck_mail_check(void) { return false; }
extern "C" bool tdeck_mail_connect_test(void) { return false; }
extern "C" void tdeck_mail_forget_creds(void) {}
extern "C" bool tdeck_mail_list(int) { return false; }
extern "C" int tdeck_mail_list_count(void) { return 0; }
extern "C" int tdeck_mail_total(void) { return 0; }
extern "C" int tdeck_mail_page(void) { return 0; }
extern "C" bool tdeck_mail_item(int, unsigned *, const char **, const char **, const char **, bool *) { return false; }
extern "C" bool tdeck_mail_read(unsigned) { return false; }
extern "C" const char *tdeck_mail_body(void) { return ""; }
extern "C" bool tdeck_mail_send(const char *, const char *, const char *) { return false; }
extern "C" bool tdeck_mail_send_ex(const char *, const char *, const char *, bool) { return false; }
extern "C" const char *tdeck_mail_read_reply_addr(void) { return ""; }
extern "C" const char *tdeck_mail_read_from(void) { return ""; }
extern "C" const char *tdeck_mail_read_date(void) { return ""; }
extern "C" const char *tdeck_mail_read_subject(void) { return ""; }
extern "C" bool tdeck_mail_send_selftest(void) { return false; }
extern "C" int tdeck_mail_poll(void) { return -1; }
extern "C" void tdeck_mail_counts(int *t, int *u)
{
    if (t)
        *t = -1;
    if (u)
        *u = -1;
}
extern "C" const char *tdeck_mail_error(void) { return "no wi-fi on this board"; }
extern "C" void tdeck_mail_service(void) {}
#endif
