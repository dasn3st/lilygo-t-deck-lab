// -----------------------------------------------------------------------------
// T-Deck launcher: add Meshtastic channels from a text file on the SD card.
//
// A shared channel is a link like:
//
//     https://meshtastic.org/e/#CgMSAQEKDhIIx4V...
//
// Everything after the '#' IS the channel set — name, encryption key and LoRa
// settings, base64url-encoded protobuf. Nothing is downloaded; the link is the data.
// So importing one is: decode the text, and apply what's inside.
//
// The T-Deck has no camera for the usual QR route, and hand-typing 150 characters of
// base64 on a thumb keyboard is miserable and silently fails on one wrong character.
// Putting the link in a file on the card sidesteps all of that.
//
// THREADING, the hard-won part (see TDeckTimeZone.cpp for the freeze this pattern
// prevents): the UI task owns the SD card, and the main loop owns config + radio. So
// the UI reads the file and hands the text over here; the service below decodes and
// applies it from loop(). Neither one reaches into the other's territory.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/NodeDB.h"
#include "mesh/generated/meshtastic/apponly.pb.h"
#include <pb_decode.h>
#include <string.h>

// Status codes shared with the UI.
#define TDECK_CH_IDLE 0
#define TDECK_CH_PENDING 1
#define TDECK_CH_OK 2
#define TDECK_CH_ERR_NO_LINK -1  // no '#' — this doesn't look like a channel link
#define TDECK_CH_ERR_BASE64 -2   // the encoded part isn't valid
#define TDECK_CH_ERR_DECODE -3   // decoded, but not a channel set
#define TDECK_CH_ERR_EMPTY -4    // valid, but contained no channels

static char s_linkBuf[512];
static volatile int s_status = TDECK_CH_IDLE;
static volatile int s_applied = 0;

// Called from the UI task with the file's contents. Copies and marks it pending; the
// real work happens on the main loop.
extern "C" void tdeck_channel_import_submit(const char *text, int len)
{
    if (!text || len <= 0)
        return;
    if (len > (int)sizeof(s_linkBuf) - 1)
        len = (int)sizeof(s_linkBuf) - 1;
    memcpy(s_linkBuf, text, len);
    s_linkBuf[len] = 0;
    s_applied = 0;
    s_status = TDECK_CH_PENDING;
}

extern "C" int tdeck_channel_import_status(void)
{
    return s_status;
}

extern "C" int tdeck_channel_import_count(void)
{
    return s_applied;
}

extern "C" void tdeck_channel_import_clear(void)
{
    s_status = TDECK_CH_IDLE;
    s_applied = 0;
}

// base64url -> bytes. Accepts the standard alphabet too, ignores padding and any
// whitespace/newlines the file picked up from being emailed around.
static int b64urlDecode(const char *in, uint8_t *out, int outCap)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    uint32_t acc = 0;
    int bits = 0, n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')
            continue;
        int v = val(*p);
        if (v < 0)
            return -1; // a character that has no business being in a link
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= outCap)
                return -1;
            out[n++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return n;
}

// Called from the main Meshtastic loop() — the safe place to touch channels and flash.
extern "C" void tdeck_channel_import_service(void)
{
    if (s_status != TDECK_CH_PENDING)
        return;

    // Take the part after '#'. Accept a bare encoded blob too, in case someone saved
    // only the tail of the link.
    const char *enc = strchr(s_linkBuf, '#');
    enc = enc ? enc + 1 : s_linkBuf;
    while (*enc == '\r' || *enc == '\n' || *enc == ' ' || *enc == '\t')
        enc++;
    if (!*enc) {
        s_status = TDECK_CH_ERR_NO_LINK;
        return;
    }

    uint8_t raw[384];
    int rawLen = b64urlDecode(enc, raw, sizeof(raw));
    if (rawLen <= 0) {
        s_status = TDECK_CH_ERR_BASE64;
        return;
    }

    meshtastic_ChannelSet cs = meshtastic_ChannelSet_init_default;
    pb_istream_t stream = pb_istream_from_buffer(raw, (size_t)rawLen);
    if (!pb_decode(&stream, &meshtastic_ChannelSet_msg, &cs)) {
        s_status = TDECK_CH_ERR_DECODE;
        return;
    }
    if (cs.settings_count == 0) {
        s_status = TDECK_CH_ERR_EMPTY;
        return;
    }

    // ADD the shared channels; don't overwrite what's already here.
    //
    // The first version wrote the incoming set starting at slot 0, so a link whose primary
    // was "Howe group" landed on top of the device's own primary — LongFast appeared to be
    // *renamed*, taking its whole message history with it (messages are keyed by channel
    // index, so the chats stayed put while the label changed underneath them).
    //
    // So: match by name and update in place, otherwise take the first free slot. The
    // device keeps its own primary — a channel someone shares with you is one you're
    // joining, not a replacement for the one you're already on.
    for (pb_size_t i = 0; i < cs.settings_count && i < 8; i++) {
        const meshtastic_ChannelSettings &incoming = cs.settings[i];

        int target = -1;
        bool replacing = false;
        for (uint8_t c = 0; c < channels.getNumChannels(); c++) {
            meshtastic_Channel &existing = channels.getByIndex(c);
            if (existing.role != meshtastic_Channel_Role_DISABLED && existing.has_settings &&
                strcmp(existing.settings.name, incoming.name) == 0) {
                target = c; // same name: refresh its settings rather than making a duplicate
                replacing = true;
                break;
            }
        }
        if (target < 0) {
            for (uint8_t c = 0; c < channels.getNumChannels(); c++) {
                if (channels.getByIndex(c).role == meshtastic_Channel_Role_DISABLED) {
                    target = c;
                    break;
                }
            }
        }
        if (target < 0) {
            LOG_WARN("T-Deck: no free channel slot for '%s'", incoming.name);
            continue; // all 8 in use — better to skip than to clobber one
        }

        meshtastic_Channel ch = meshtastic_Channel_init_default;
        ch.index = (int8_t)target;
        ch.settings = incoming;
        ch.has_settings = true;
        // Only slot 0 may be primary, and only if it already was; everything else joins
        // as a secondary so the device's existing primary survives the import.
        ch.role = (target == 0) ? meshtastic_Channel_Role_PRIMARY : meshtastic_Channel_Role_SECONDARY;
        channels.setChannel(ch);
        if (!replacing || target != 0)
            s_applied++;
        LOG_INFO("T-Deck: channel '%s' -> slot %d (%s)", incoming.name, target, replacing ? "updated" : "added");
    }

    // The link carries the sender's LoRa settings too. Only take the modem preset, and only
    // when this device is still on the default — the preset decides which airwaves everyone
    // shares, so matching it is the point of joining. Region stays local (legal/band plan),
    // and nothing else is worth letting a shared file rewrite behind the user's back.
    if (cs.has_lora_config && cs.lora_config.modem_preset != config.lora.modem_preset &&
        config.lora.modem_preset == meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST) {
        LOG_INFO("T-Deck: adopting modem preset %d from the shared channel", (int)cs.lora_config.modem_preset);
        config.lora.modem_preset = cs.lora_config.modem_preset;
    }

    channels.onConfigChanged();
    if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS | SEGMENT_CONFIG);

    LOG_INFO("T-Deck: imported %d channel(s) from the SD card", s_applied);
    s_status = TDECK_CH_OK;
}
