// -----------------------------------------------------------------------------
// EmojiText - make emoji somebody sent you actually visible.
//
// Jake's list, 2026-09-18, one word: "emojis". Here is the concrete problem behind it.
//
// This build has NO emoji glyphs in any font, and `LV_USE_FONT_PLACEHOLDER` is 0 in
// lv_conf.h - which means a missing glyph draws NOTHING AT ALL. Not a box, not a
// question mark. So when somebody on the mesh sends "on my way 👍", the T-Deck shows
// "on my way " and the message looks like it lost its ending. That is a bug whichever
// way the one-word request was meant.
//
// The honest fix would be a font containing the glyphs. That is a real piece of work
// (font tooling, a few hundred KB of flash for a useful set) and it needs someone
// looking at the screen to judge. This is the cheap 90%: turn each emoji into a short
// piece of text that renders in the font we already have. ":+1:" is not a thumbs-up,
// but it tells you a thumbs-up was sent, which is the entire difference between a
// message that reads and one that looks broken.
//
// Nothing is transformed on the way OUT. What Jake sends is untouched UTF-8, so a
// phone at the other end still shows a real emoji.
// -----------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" void emoji_to_text(const char *in, char *out, size_t outN);

namespace
{
struct Tag {
    uint32_t cp;
    const char *txt;
};

// Short on purpose. Most emoji are four UTF-8 bytes, so a four-character tag costs no
// space at all - which matters, because the message buffer upstream is a fixed 284 bytes.
const Tag kTags[] = {
    // the ones that actually turn up in conversation
    {0x1F44D, ":+1:"},     {0x1F44E, ":-1:"},     {0x2764, "<3"},        {0x1F499, "<3"},
    {0x1F602, ":'D"},      {0x1F923, ":'D"},      {0x1F600, ":D"},       {0x1F603, ":D"},
    {0x1F604, ":D"},       {0x1F601, ":D"},       {0x1F642, ":)"},       {0x1F60A, ":)"},
    {0x1F609, ";)"},       {0x1F605, ":')"},      {0x1F622, ":'("},      {0x1F62D, ":'("},
    {0x1F641, ":("},       {0x1F614, ":("},       {0x1F610, ":|"},       {0x1F62E, ":o"},
    {0x1F914, ":?"},       {0x1F44B, "[wave]"},   {0x1F64F, "[thx]"},    {0x1F44F, "[clap]"},
    {0x1F525, "[fire]"},   {0x2705, "[ok]"},      {0x274C, "[x]"},       {0x26A0, "[!]"},
    {0x2757, "[!]"},       {0x2753, "[?]"},       {0x1F4AF, "[100]"},    {0x1F60E, "[cool]"},
    // things that come up on a mesh, outdoors
    {0x1F4CD, "[pin]"},    {0x1F5FA, "[map]"},    {0x1F3AF, "[target]"}, {0x1F98C, "[deer]"},
    {0x1F43B, "[bear]"},   {0x1F420, "[fish]"},   {0x1F332, "[tree]"},   {0x26F0, "[mtn]"},
    {0x1F697, "[car]"},    {0x1F6FB, "[truck]"},  {0x1F3D5, "[camp]"},   {0x1F3E0, "[home]"},
    {0x1F50B, "[batt]"},   {0x1F4F6, "[signal]"}, {0x1F4FB, "[radio]"},  {0x1F526, "[light]"},
    {0x2600, "[sun]"},     {0x2601, "[cloud]"},   {0x2744, "[snow]"},    {0x26C8, "[storm]"},
    {0x1F327, "[rain]"},   {0x1F319, "[moon]"},   {0x23F0, "[alarm]"},   {0x1F55B, "[time]"},
    {0x1F4DE, "[call]"},   {0x1F4E9, "[msg]"},    {0x1F6A8, "[alert]"},  {0x1F195, "[new]"},
};

// Decode one UTF-8 character. Returns the codepoint and advances len by its byte count.
// A malformed byte is reported as itself with len 1, so a bad stream still terminates.
uint32_t decodeUtf8(const unsigned char *p, int &len)
{
    if (p[0] < 0x80) {
        len = 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        len = 2;
        return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        len = 3;
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        len = 4;
        return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) |
               (p[3] & 0x3F);
    }
    len = 1;
    return p[0];
}

// Anything in these ranges has no glyph in this build, so it would render as nothing.
bool isInvisible(uint32_t cp)
{
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true; // emoji proper
    if (cp >= 0x2600 && cp <= 0x27BF) return true;   // misc symbols + dingbats
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;   // arrows and shapes
    if (cp >= 0x2190 && cp <= 0x21FF) return true;   // arrows
    if (cp >= 0x2460 && cp <= 0x24FF) return true;   // enclosed alphanumerics
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return true; // regional indicators (flags)
    return false;
}

// Zero-width and modifier codepoints: drop them silently rather than tagging them, or a
// single waving hand with a skin tone would come out as "[wave][emoji]".
bool isModifier(uint32_t cp)
{
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true; // skin tone
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;   // variation selectors
    if (cp == 0x200D) return true;                   // zero-width joiner
    if (cp >= 0xE0020 && cp <= 0xE007F) return true; // tag characters
    return false;
}
} // namespace

// Rewrites `in` into `out`, replacing anything unrenderable with a short readable tag.
// Always NUL-terminates and never writes past outN. Plain ASCII text passes through
// untouched and costs one comparison per character.
extern "C" void emoji_to_text(const char *in, char *out, size_t outN)
{
    if (!out || outN == 0)
        return;
    out[0] = 0;
    if (!in)
        return;

    const unsigned char *p = (const unsigned char *)in;
    size_t w = 0;
    while (*p && w + 1 < outN) {
        if (*p < 0x80) { // the overwhelmingly common case: plain ASCII
            out[w++] = (char)*p++;
            continue;
        }
        int len = 1;
        const uint32_t cp = decodeUtf8(p, len);
        p += len;

        if (isModifier(cp))
            continue; // silently dropped

        if (!isInvisible(cp)) {
            // A non-emoji multi-byte character (an accent, say). The font may not have it
            // either, but guessing a tag for every language is not this function's job -
            // copy it through and let it render or not.
            for (int k = 0; k < len && w + 1 < outN; k++)
                out[w++] = (char)*(p - len + k);
            continue;
        }

        const char *tag = "[emoji]";
        for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); i++)
            if (kTags[i].cp == cp) {
                tag = kTags[i].txt;
                break;
            }
        // Only write the tag if the WHOLE of it fits; half a tag reads worse than none.
        const size_t tl = strlen(tag);
        if (w + tl + 1 > outN)
            break;
        memcpy(out + w, tag, tl);
        w += tl;
    }
    out[w] = 0;
}
