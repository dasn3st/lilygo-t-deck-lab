// -----------------------------------------------------------------------------
// T-UI Terminal — a small SSH command terminal for the T-Deck.
//
// The app deliberately keeps its SSH client and transcript outside the LVGL screen. Leaving
// the app therefore does not log out or throw away the conversation. A later open reuses the
// live connection when possible and reconnects when Wi-Fi or the remote session disappeared.
//
// SD configuration (never compiled into the firmware): /ssh.txt
//
//   name=Hermes
//   host=example.local
//   port=22
//   user=your-user
//   password=put-password-here
//
// The current SSH library executes one command per channel while preserving the authenticated
// SSH session. That is enough for reliable agent/server work (ls, systemctl, logs, scripts, ...)
// and leaves room for a later PTY upgrade without changing this app's UI or SD format.
// ----------------------------------------------------------------------------
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "TDeckClipboard.h"
#include "ui.h"

#if HAS_SDCARD && !HAS_SD_MMC && !ARCH_PORTDUINO
#include "graphics/common/SdCard.h"
#define TERMINAL_HAVE_SD 1
#else
#define TERMINAL_HAVE_SD 0
#endif

#include "libssh_esp32.h"
#include <libssh/libssh.h>

extern const lv_font_t ui_font_montserrat_14;
extern const lv_font_t ui_font_montserrat_16;
extern "C" void terminal_open(void);

namespace tdeckvoice
{
int state();
bool voiceEnabled();
void setVoiceEnabled(bool enabled);
bool startRecording();
void setTarget(lv_obj_t *target);
uint32_t recordingElapsedSeconds();
} // namespace tdeckvoice

namespace
{
const size_t kTranscriptBytes = 8192;
const size_t kConfigLineBytes = 160;

struct SshConfig
{
    char name[32] = "SSH";
    char host[64] = {};
    char user[32] = {};
    char password[64] = {};
    uint16_t port = 22;
};

lv_obj_t *screen = nullptr;
lv_obj_t *outputBox = nullptr;
lv_obj_t *outputLbl = nullptr;
lv_obj_t *commandArea = nullptr;
lv_obj_t *targetLbl = nullptr;
lv_obj_t *statusLbl = nullptr;
lv_obj_t *connectBtn = nullptr;
lv_obj_t *menuBox = nullptr;
lv_obj_t *voiceBtn = nullptr;
lv_obj_t *voiceBtnLbl = nullptr;
lv_obj_t *voiceToggleLbl = nullptr;
lv_timer_t *focusGuard = nullptr;
lv_timer_t *voiceUiTimer = nullptr;

ssh_session ssh = nullptr;
bool sshConnected = false;
bool libsshReady = false;
SshConfig config;
bool configLoaded = false;
bool transcriptLoaded = false;
char transcript[kTranscriptBytes] = {};
int outputZoom = 0; // 0=compact, 1=normal, 2=large

void refreshVoiceControls(lv_timer_t * = nullptr)
{
    const int st = tdeckvoice::state();
    const bool visible = tdeckvoice::voiceEnabled() || st == 1 || st == 2;
    if (voiceBtn) {
        if (visible) lv_obj_clear_flag(voiceBtn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(voiceBtn, LV_OBJ_FLAG_HIDDEN);
        char label[16];
        if (st == 1)
            snprintf(label, sizeof(label), "%02u:%02u",
                     (unsigned)(tdeckvoice::recordingElapsedSeconds() / 60),
                     (unsigned)(tdeckvoice::recordingElapsedSeconds() % 60));
        else
            snprintf(label, sizeof(label), st == 2 ? "..." : "Mic");
        lv_label_set_text(voiceBtnLbl, label);
        lv_obj_set_style_bg_color(voiceBtn, lv_color_hex(st == 1 ? 0xff453a : st == 2 ? 0x0a84ff : 0x30a46c), LV_PART_MAIN);
        if (st == 2) lv_obj_add_state(voiceBtn, LV_STATE_DISABLED);
        else lv_obj_clear_state(voiceBtn, LV_STATE_DISABLED);
    }
    if (commandArea)
        lv_obj_set_width(commandArea, visible ? 204 : 252);
    if (voiceToggleLbl)
        lv_label_set_text(voiceToggleLbl, tdeckvoice::voiceEnabled() ? "Sprache: AN" : "Sprache: AUS");
}

void toggleVoiceEnabled(lv_event_t *)
{
    const int st = tdeckvoice::state();
    if (st == 1 || st == 2)
        return;
    tdeckvoice::setVoiceEnabled(!tdeckvoice::voiceEnabled());
    refreshVoiceControls();
}

void onVoiceButton(lv_event_t *)
{
    tdeckvoice::setTarget(commandArea);
    tdeckvoice::startRecording();
    refreshVoiceControls();
}

char *trim(char *s)
{
    while (*s && std::isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && std::isspace((unsigned char)end[-1]))
        *--end = 0;
    return s;
}

void setStatus(const char *text, uint32_t color = 0x8e8e93)
{
    if (!statusLbl)
        return;
    lv_label_set_text(statusLbl, text ? text : "");
    lv_obj_set_style_text_color(statusLbl, lv_color_hex(color), LV_PART_MAIN);
}

void closeMenu(void)
{
    if (menuBox)
        lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
}

void toggleMenu(lv_event_t *)
{
    if (!menuBox)
        return;
    if (lv_obj_has_flag(menuBox, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menuBox);
    } else {
        closeMenu();
    }
}

void refreshOutput(void)
{
    if (!outputLbl)
        return;
    lv_label_set_text(outputLbl, transcript[0] ? transcript : "Terminal bereit.\n");
    lv_obj_update_layout(outputBox);
    lv_obj_scroll_to_y(outputBox, LV_COORD_MAX, LV_ANIM_OFF);
}

void applyOutputZoom(void)
{
    if (!outputLbl)
        return;
    const lv_font_t *font = outputZoom == 0 ? &ui_font_montserrat_12
                             : outputZoom == 1 ? &ui_font_montserrat_14
                                                : &ui_font_montserrat_16;
    lv_obj_set_style_text_font(outputLbl, font, LV_PART_MAIN);
    refreshOutput();
}

void zoomOut(void)
{
    if (outputZoom > 0)
        outputZoom--;
    applyOutputZoom();
    setStatus(outputZoom == 0 ? "Zoom klein" : "Zoom normal", 0x30d158);
}

void zoomIn(void)
{
    if (outputZoom < 2)
        outputZoom++;
    applyOutputZoom();
    setStatus(outputZoom == 2 ? "Zoom gross" : "Zoom normal", 0x30d158);
}

void appendRaw(const char *text)
{
    if (!text || !*text)
        return;
    size_t n = strlen(text);
    if (n >= sizeof(transcript)) {
        text += n - sizeof(transcript) + 1;
        n = sizeof(transcript) - 1;
    }
    size_t have = strlen(transcript);
    if (have + n >= sizeof(transcript)) {
        size_t drop = have + n - sizeof(transcript) + 1;
        memmove(transcript, transcript + drop, have - drop);
        have -= drop;
        transcript[have] = 0;
    }
    memcpy(transcript + have, text, n);
    transcript[have + n] = 0;
}

void appendLine(const char *text)
{
    appendRaw(text ? text : "");
    if (text && *text && text[strlen(text) - 1] != '\n')
        appendRaw("\n");
    refreshOutput();
}

void saveLog(const char *text)
{
#if TERMINAL_HAVE_SD
    if (!text || !*text)
        return;
    FsFile f = SDFs.open("/terminal.log", O_WRONLY | O_CREAT | O_APPEND);
    if (f) {
        f.print(text);
        f.close();
    }
#else
    (void)text;
#endif
}

void appendAndSave(const char *text)
{
    appendLine(text);
    if (text && *text) {
        saveLog(text);
        if (text[strlen(text) - 1] != '\n')
            saveLog("\n");
    }
}

void loadTranscript(void)
{
    if (transcriptLoaded)
        return;
    transcriptLoaded = true;
    transcript[0] = 0;
#if TERMINAL_HAVE_SD
    FsFile f = SDFs.open("/terminal.log", O_RDONLY);
    if (!f)
        return;
    uint32_t size = (uint32_t)f.size();
    uint32_t start = size > sizeof(transcript) - 1 ? size - (sizeof(transcript) - 1) : 0;
    if (start)
        f.seek(start);
    int n = f.read((uint8_t *)transcript, sizeof(transcript) - 1);
    if (n < 0)
        n = 0;
    transcript[n] = 0;
    f.close();
#endif
}

bool loadConfig(void)
{
    config = SshConfig();
#if !TERMINAL_HAVE_SD
    configLoaded = false;
    return false;
#else
    FsFile f = SDFs.open("/ssh.txt", O_RDONLY);
    if (!f) {
        configLoaded = false;
        return false;
    }
    char line[kConfigLineBytes];
    size_t used = 0;
    while (true) {
        int c = f.read();
        if (c < 0)
            break;
        if (c == '\r')
            continue;
        if (c != '\n' && used + 1 < sizeof(line)) {
            line[used++] = (char)c;
            continue;
        }
        line[used] = 0;
        char *p = trim(line);
        if (*p && *p != '#') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq++ = 0;
                char *key = trim(p);
                char *value = trim(eq);
                if (!strcmp(key, "name"))
                    snprintf(config.name, sizeof(config.name), "%s", value);
                else if (!strcmp(key, "host"))
                    snprintf(config.host, sizeof(config.host), "%s", value);
                else if (!strcmp(key, "port"))
                    config.port = (uint16_t)atoi(value);
                else if (!strcmp(key, "user"))
                    snprintf(config.user, sizeof(config.user), "%s", value);
                else if (!strcmp(key, "password") || !strcmp(key, "pass"))
                    snprintf(config.password, sizeof(config.password), "%s", value);
            }
        }
        used = 0;
        if (c != '\n')
            break;
    }
    f.close();
    configLoaded = config.host[0] && config.user[0] && config.password[0];
    return configLoaded;
#endif
}

void updateTargetLabel(void)
{
    if (!targetLbl)
        return;
    if (configLoaded)
        lv_label_set_text_fmt(targetLbl, "%s  %s:%u", config.name, config.host, config.port);
    else
        lv_label_set_text(targetLbl, "SSH-Ziel nicht eingerichtet");
}

bool ensureClient(void)
{
    if (!libsshReady) {
        libssh_begin();
        libsshReady = true;
    }
    return true;
}

void updateConnectButton(void)
{
    if (!connectBtn)
        return;
    lv_obj_t *label = lv_obj_get_child(connectBtn, 0);
    if (label)
        lv_label_set_text(label, sshConnected ? "Trennen" : "Verbinden");
    lv_obj_set_style_bg_color(connectBtn, lv_color_hex(sshConnected ? 0xff453a : 0x30d158),
                               LV_PART_MAIN);
}

bool connectNow(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        setStatus("Wi-Fi nicht verbunden", 0xff9f0a);
        return false;
    }
    if (!loadConfig()) {
        updateTargetLabel();
        setStatus("/ssh.txt auf SD fehlt oder unvollstaendig", 0xff9f0a);
        appendAndSave("[terminal] /ssh.txt fehlt oder ist unvollstaendig");
        return false;
    }
    updateTargetLabel();
    if (!ensureClient()) {
        setStatus("SSH-Client konnte nicht starten", 0xff453a);
        return false;
    }
    if (sshConnected && ssh) {
        updateConnectButton();
        setStatus("SSH verbunden", 0x30d158);
        return true;
    }
    setStatus("Verbinde mit SSH ...", 0xff9f0a);
    ensureClient();
    ssh = ssh_new();
    if (!ssh) {
        setStatus("SSH-Session konnte nicht angelegt werden", 0xff453a);
        return false;
    }
    long timeoutSeconds = 12;
    int port = config.port;
    if (ssh_options_set(ssh, SSH_OPTIONS_HOST, config.host) != SSH_OK ||
        ssh_options_set(ssh, SSH_OPTIONS_USER, config.user) != SSH_OK ||
        ssh_options_set(ssh, SSH_OPTIONS_PORT, &port) != SSH_OK ||
        ssh_options_set(ssh, SSH_OPTIONS_TIMEOUT, &timeoutSeconds) != SSH_OK ||
        ssh_connect(ssh) != SSH_OK) {
        setStatus("SSH-Verbindung fehlgeschlagen", 0xff453a);
        if (ssh) {
            ssh_disconnect(ssh);
            ssh_free(ssh);
            ssh = nullptr;
        }
        appendAndSave("[terminal] SSH-Verbindung fehlgeschlagen");
        updateConnectButton();
        return false;
    }
    // No password is ever printed to the UI or log. Authentication stays on the live session.
    const int rc = ssh_userauth_password(ssh, config.user, config.password);
    if (rc != SSH_AUTH_SUCCESS) {
        const char *error = ssh_get_error(ssh);
        setStatus("SSH-Anmeldung fehlgeschlagen", 0xff453a);
        if (error)
            appendAndSave(error);
        ssh_disconnect(ssh);
        ssh_free(ssh);
        ssh = nullptr;
        appendAndSave("[terminal] SSH-Anmeldung fehlgeschlagen");
        updateConnectButton();
        return false;
    }
    sshConnected = true;
    setStatus("SSH verbunden", 0x30d158);
    appendAndSave("[terminal] SSH verbunden");
    updateConnectButton();
    return true;
}

void disconnectNow(void)
{
    if (ssh) {
        ssh_disconnect(ssh);
        ssh_free(ssh);
        ssh = nullptr;
    }
    sshConnected = false;
    setStatus("SSH getrennt", 0x8e8e93);
    updateConnectButton();
}

void sendCommand(void)
{
    if (!commandArea)
        return;
    const char *cmd = lv_textarea_get_text(commandArea);
    if (!cmd || !*cmd)
        return;
    std::string command(cmd);
    lv_textarea_set_text(commandArea, "");
    const std::string shown = "$ " + command;
    appendAndSave(shown.c_str());
    if (!connectNow())
        return;
    setStatus("Befehl wird ausgefuehrt ...", 0xff9f0a);
    ssh_channel channel = ssh_channel_new(ssh);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK ||
        ssh_channel_request_exec(channel, command.c_str()) != SSH_OK) {
        setStatus("SSH-Befehl fehlgeschlagen", 0xff453a);
        appendAndSave("[terminal] Befehl fehlgeschlagen");
        if (channel) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        }
        updateConnectButton();
        return;
    }
    char chunk[384];
    int n;
    while ((n = ssh_channel_read(channel, chunk, sizeof(chunk) - 1, 0)) > 0) {
        chunk[n] = 0;
        appendRaw(chunk);
        saveLog(chunk);
    }
    while ((n = ssh_channel_read(channel, chunk, sizeof(chunk) - 1, 1)) > 0) {
        chunk[n] = 0;
        appendRaw(chunk);
        saveLog(chunk);
    }
    refreshOutput();
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    if (n < 0) {
        appendAndSave("[terminal] Fehler beim Lesen der SSH-Ausgabe");
        setStatus("SSH-Ausgabe fehlgeschlagen", 0xff453a);
        return;
    }
    setStatus("Bereit", 0x30d158);
}

void clearTranscript(void)
{
    transcript[0] = 0;
#if TERMINAL_HAVE_SD
    if (SDFs.exists("/terminal.log"))
        SDFs.remove("/terminal.log");
#endif
    refreshOutput();
    setStatus("Verlauf geloescht", 0x8e8e93);
}

void readFromSd(void)
{
    configLoaded = loadConfig();
    updateTargetLabel();
    transcriptLoaded = false;
    loadTranscript();
    refreshOutput();
    updateConnectButton();
    setStatus("SD-Daten gelesen", 0x30d158);
}

lv_obj_t *button(lv_obj_t *parent, const char *text, int w, int x, int y, uint32_t color,
                 lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 26);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void addHamburgerIcon(lv_obj_t *btn)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *line = lv_obj_create(btn);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 14, 2);
        lv_obj_align(line, LV_ALIGN_CENTER, 0, (i - 1) * 5);
        lv_obj_set_style_bg_color(line, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void buildScreen(void)
{
    screen = lv_obj_create(nullptr);
    tui_statusbar_reserve(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    targetLbl = lv_label_create(screen);
    lv_obj_set_pos(targetLbl, 6, 5);
    lv_obj_set_width(targetLbl, 270);
    lv_obj_set_height(targetLbl, 16);
    lv_label_set_long_mode(targetLbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(targetLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(targetLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(targetLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(targetLbl, lv_color_hex(0xffffff), LV_PART_MAIN);
    updateTargetLabel();

    // Ollama-style header: the global status-bar click remains the way out; the app itself only
    // exposes a compact menu button so the terminal gets the whole screen for output.
    lv_obj_t *menuBtn = button(screen, "", 28, 288, 0, 0x2c2c2e, toggleMenu);
    lv_obj_set_size(menuBtn, 28, 28);
    addHamburgerIcon(menuBtn);

    outputBox = lv_obj_create(screen);
    lv_obj_set_pos(outputBox, 2, 28);
    lv_obj_set_size(outputBox, 316, 154);
    lv_obj_set_style_bg_color(outputBox, lv_color_hex(0x111113), LV_PART_MAIN);
    lv_obj_set_style_border_width(outputBox, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(outputBox, lv_color_hex(0x2c2c2e), LV_PART_MAIN);
    lv_obj_set_style_radius(outputBox, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(outputBox, 5, LV_PART_MAIN);
    lv_obj_set_scroll_dir(outputBox, LV_DIR_VER);
    outputLbl = lv_label_create(outputBox);
    lv_obj_set_width(outputLbl, 298);
    lv_obj_set_style_text_color(outputLbl, lv_color_hex(0xf2f2f7), LV_PART_MAIN);
    lv_obj_set_style_text_font(outputLbl, &ui_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(outputLbl, 0, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(outputLbl, 0, LV_PART_MAIN);
    lv_label_set_long_mode(outputLbl, LV_LABEL_LONG_WRAP);
    refreshOutput();

    commandArea = lv_textarea_create(screen);
    lv_textarea_set_one_line(commandArea, false);
    lv_textarea_set_max_length(commandArea, 4096);
    lv_textarea_set_text_selection(commandArea, true);
    lv_textarea_set_placeholder_text(commandArea, "Nachricht eingeben ...");
    lv_obj_set_pos(commandArea, 4, 184);
    lv_obj_set_size(commandArea, 252, 48);
    lv_obj_set_style_bg_color(commandArea, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_text_color(commandArea, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_width(commandArea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(commandArea, 7, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(commandArea, 0, LV_PART_CURSOR);
    lv_anim_delete(commandArea, nullptr);
    if (lv_group_get_default())
        lv_group_add_obj(lv_group_get_default(), commandArea);

    voiceBtn = button(screen, "Mic", 44, 212, 186, 0x30a46c, onVoiceButton);
    lv_obj_set_pos(voiceBtn, 212, 195);
    voiceBtnLbl = lv_obj_get_child(voiceBtn, 0);

    button(screen, ">", 56, 260, 195, 0x0a84ff, [](lv_event_t *) { sendCommand(); });

    // All controls that are useful but not part of the conversation live here, just like in
    // Ollama. The menu intentionally stays open after Connect, Read and Clear so the user can
    // configure several things without reopening it.
    menuBox = lv_obj_create(screen);
    lv_obj_set_pos(menuBox, 72, 22);
    lv_obj_set_size(menuBox, 244, 194);
    lv_obj_set_style_bg_color(menuBox, lv_color_hex(0x1c1c1e), LV_PART_MAIN);
    lv_obj_set_style_border_width(menuBox, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(menuBox, lv_color_hex(0x3a3a3c), LV_PART_MAIN);
    lv_obj_set_style_radius(menuBox, 8, LV_PART_MAIN);
    lv_obj_add_flag(menuBox, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *menuTitle = lv_label_create(menuBox);
        lv_label_set_text(menuTitle, "Terminal");
        lv_obj_set_pos(menuTitle, 10, 6);
        lv_obj_set_style_text_color(menuTitle, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_t *voiceToggle = button(menuBox, "Sprache: AUS", 122, 108, 3, 0x2c2c2e, toggleVoiceEnabled);
        voiceToggleLbl = lv_obj_get_child(voiceToggle, 0);

        connectBtn = button(menuBox, "Verbinden", 220, 10, 28, 0x30d158, [](lv_event_t *) {
            if (sshConnected)
                disconnectNow();
            else
                connectNow();
        });

        lv_obj_t *readBtn = button(menuBox, "SD lesen", 106, 10, 60, 0x2c2c2e,
                                   [](lv_event_t *) { readFromSd(); });
        (void)readBtn;
        lv_obj_t *clearBtn = button(menuBox, "Leeren", 106, 124, 60, 0x3a3a3c,
                                    [](lv_event_t *) { clearTranscript(); });
        (void)clearBtn;
        lv_obj_t *zoomLbl = lv_label_create(menuBox);
        lv_label_set_text(zoomLbl, "Schrift");
        lv_obj_set_pos(zoomLbl, 10, 94);
        lv_obj_set_style_text_color(zoomLbl, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
        lv_obj_t *zoomMinus = button(menuBox, "−", 48, 62, 90, 0x2c2c2e,
                                     [](lv_event_t *) { zoomOut(); });
        (void)zoomMinus;
        lv_obj_t *zoomPlus = button(menuBox, "+", 48, 114, 90, 0x2c2c2e,
                                    [](lv_event_t *) { zoomIn(); });
        (void)zoomPlus;
        button(menuBox, "Verlauf kopieren", 220, 10, 126, 0x2c2c2e,
               [](lv_event_t *) {
                   const bool copied = tdeckclipboard::copyText(transcript);
                   setStatus(copied ? "Verlauf kopiert" : "Verlauf leer", copied ? 0x30d158 : 0xff9f0a);
               });

        statusLbl = lv_label_create(menuBox);
        lv_obj_set_pos(statusLbl, 10, 160);
        lv_obj_set_width(statusLbl, 220);
        lv_label_set_long_mode(statusLbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(statusLbl, lv_color_hex(0xc7c7cc), LV_PART_MAIN);
        lv_obj_set_style_text_font(statusLbl, &ui_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(statusLbl, 0, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(statusLbl, 0, LV_PART_MAIN);
        lv_label_set_text(statusLbl, "Bereit");
    }
    applyOutputZoom();
    refreshVoiceControls();
    voiceUiTimer = lv_timer_create(refreshVoiceControls, 250, nullptr);

    focusGuard = lv_timer_create(
        [](lv_timer_t *) {
            lv_group_t *g = lv_group_get_default();
            if (g && commandArea && lv_group_get_focused(g) != commandArea)
                lv_group_focus_obj(commandArea);
        },
        300, nullptr);
    lv_timer_pause(focusGuard);
    lv_obj_add_event_cb(
        screen,
        [](lv_event_t *) {
            lv_timer_resume(focusGuard);
            if (voiceUiTimer)
                lv_timer_resume(voiceUiTimer);
            if (lv_group_get_default())
                lv_group_focus_obj(commandArea);
        },
        LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(screen, [](lv_event_t *) {
        lv_timer_pause(focusGuard);
        if (voiceUiTimer)
            lv_timer_pause(voiceUiTimer);
    }, LV_EVENT_SCREEN_UNLOADED, nullptr);
}
} // namespace

extern "C" void terminal_open(void)
{
    if (!screen)
        buildScreen();
    loadConfig();
    loadTranscript();
    updateTargetLabel();
    refreshOutput();
    updateConnectButton();
    closeMenu();
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}
