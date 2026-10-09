// Local T-Deck audio controls and microphone diagnostic; never uploads test audio.
#include "graphics/view/TFT/TuiStatusBar.h"
#include "lvgl.h"
#include "TDeckVoice.h"
#include <cstdio>

void playBeep();

namespace
{
lv_obj_t *screen = nullptr;
lv_obj_t *mic1Bar = nullptr;
lv_obj_t *mic2Bar = nullptr;
lv_obj_t *mic1Level = nullptr;
lv_obj_t *mic2Level = nullptr;
lv_obj_t *mic1GainSlider = nullptr;
lv_obj_t *mic2GainSlider = nullptr;
lv_obj_t *speakerSlider = nullptr;
lv_obj_t *inputDropdown = nullptr;
lv_obj_t *mic1GainText = nullptr;
lv_obj_t *mic2GainText = nullptr;
lv_obj_t *speakerText = nullptr;
lv_obj_t *stateText = nullptr;
lv_obj_t *toggleButton = nullptr;
lv_obj_t *toggleLabel = nullptr;
lv_obj_t *playButton = nullptr;
lv_obj_t *prevScreen = nullptr;
lv_timer_t *tick = nullptr;

void refresh(void)
{
    if (!screen || lv_screen_active() != screen)
        return;
    const int state = tdeckvoice::state();
    const bool active = state == tdeckvoice::RECORDING;
    const uint8_t level1 = tdeckvoice::inputLevelMic1();
    const uint8_t level2 = tdeckvoice::inputLevelMic2();
    lv_bar_set_value(mic1Bar, level1, LV_ANIM_OFF);
    lv_bar_set_value(mic2Bar, level2, LV_ANIM_OFF);
    char value[24];
    snprintf(value, sizeof(value), "%u%%", (unsigned)level1);
    lv_label_set_text(mic1Level, value);
    snprintf(value, sizeof(value), "%u%%", (unsigned)level2);
    lv_label_set_text(mic2Level, value);
    snprintf(value, sizeof(value), "Mic 1 gain: %.1f dB", tdeckvoice::micGainDb(1));
    lv_label_set_text(mic1GainText, value);
    snprintf(value, sizeof(value), "Mic 2 gain: %.1f dB", tdeckvoice::micGainDb(2));
    lv_label_set_text(mic2GainText, value);
    snprintf(value, sizeof(value), "Speaker: %u%%", (unsigned)tdeckvoice::speakerVolume());
    lv_label_set_text(speakerText, value);
    lv_slider_set_value(mic1GainSlider, tdeckvoice::micGainIndex(1), LV_ANIM_OFF);
    lv_slider_set_value(mic2GainSlider, tdeckvoice::micGainIndex(2), LV_ANIM_OFF);
    lv_slider_set_value(speakerSlider, tdeckvoice::speakerVolume(), LV_ANIM_OFF);
    lv_dropdown_set_selected(inputDropdown, tdeckvoice::inputMode());
    lv_label_set_text(stateText, active ?
                      (tdeckvoice::inputLevel() >= 4 ? "Signal wird empfangen" : "Warte auf Mikrofonsignal...") :
                      tdeckvoice::statusText());
    lv_label_set_text(toggleLabel, active ? "Test stoppen" : "Pegeltest starten");
    lv_obj_set_style_bg_color(toggleButton, lv_color_hex(active ? 0xff453a : 0x30d158), LV_PART_MAIN);
    if (active) {
        lv_obj_add_state(mic1GainSlider, LV_STATE_DISABLED);
        lv_obj_add_state(mic2GainSlider, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(mic1GainSlider, LV_STATE_DISABLED);
        lv_obj_clear_state(mic2GainSlider, LV_STATE_DISABLED);
    }
    if (tdeckvoice::diagnosticPlaybackAvailable())
        lv_obj_clear_state(playButton, LV_STATE_DISABLED);
    else
        lv_obj_add_state(playButton, LV_STATE_DISABLED);
}

void closeScreen(void)
{
    if (tdeckvoice::state() == tdeckvoice::RECORDING)
        tdeckvoice::startMonitor();
    if (tick)
        lv_timer_pause(tick);
    if (prevScreen)
        lv_screen_load_anim(prevScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

void toggleTest(lv_event_t *)
{
    tdeckvoice::startMonitor();
    refresh();
}

void micGainChanged(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    const uint8_t channel = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    tdeckvoice::setMicGainIndex(channel, (uint8_t)lv_slider_get_value(slider));
    refresh();
}

void micGainReleased(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target_obj(event);
    const uint8_t channel = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    tdeckvoice::setMicGainIndex(channel, (uint8_t)lv_slider_get_value(slider), true);
}

void speakerChanged(lv_event_t *event)
{
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    tdeckvoice::setSpeakerVolume((uint8_t)value);
    refresh();
}

void speakerReleased(lv_event_t *event)
{
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    tdeckvoice::setSpeakerVolume((uint8_t)value, true);
}

void inputModeChanged(lv_event_t *event)
{
    tdeckvoice::setInputMode((uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(event)));
}

void inputModeReleased(lv_event_t *event)
{
    tdeckvoice::setInputMode((uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(event)), true);
}

void playRecording(lv_event_t *)
{
    if (!tdeckvoice::playDiagnosticRecording())
        lv_label_set_text(stateText, "Keine Aufnahme oder Lautsprecher belegt");
    refresh();
}

void pingSpeaker(lv_event_t *)
{
    if (tdeckvoice::state() == tdeckvoice::RECORDING) {
        lv_label_set_text(stateText, "Ping während Aufnahme deaktiviert");
        return;
    }
    playBeep();
    lv_label_set_text(stateText, "Ping mit aktuellem Speaker-Pegel gesendet");
}

lv_obj_t *makeBar(lv_obj_t *parent, int y, uint32_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 194, 12);
    lv_obj_set_pos(bar, 58, y);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x34343a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    return bar;
}

void build(void)
{
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101114), LV_PART_MAIN);
    tui_statusbar_reserve(screen);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Audio / Mic");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_pos(title, 8, 5);

    inputDropdown = lv_dropdown_create(screen);
    lv_dropdown_set_options(inputDropdown, "Auto\nMic 1\nMic 2");
    lv_obj_set_size(inputDropdown, 112, 24);
    lv_obj_set_pos(inputDropdown, 201, 2);
    lv_obj_add_event_cb(inputDropdown, inputModeChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(inputDropdown, inputModeReleased, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *mic1Name = lv_label_create(screen);
    lv_label_set_text(mic1Name, "Mic 1");
    lv_obj_set_pos(mic1Name, 8, 31);
    mic1Bar = makeBar(screen, 37, 0x30d158);
    mic1Level = lv_label_create(screen);
    lv_label_set_text(mic1Level, "0%");
    lv_obj_set_pos(mic1Level, 260, 31);

    lv_obj_t *mic2Name = lv_label_create(screen);
    lv_label_set_text(mic2Name, "Mic 2");
    lv_obj_set_pos(mic2Name, 8, 54);
    mic2Bar = makeBar(screen, 60, 0x0a84ff);
    mic2Level = lv_label_create(screen);
    lv_label_set_text(mic2Level, "0%");
    lv_obj_set_pos(mic2Level, 260, 54);

    mic1GainText = lv_label_create(screen);
    lv_obj_set_pos(mic1GainText, 8, 79);
    mic1GainSlider = lv_slider_create(screen);
    lv_obj_set_size(mic1GainSlider, 270, 12);
    lv_obj_set_pos(mic1GainSlider, 25, 98);
    lv_slider_set_range(mic1GainSlider, 0, 14);
    lv_obj_add_event_cb(mic1GainSlider, micGainChanged, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)1);
    lv_obj_add_event_cb(mic1GainSlider, micGainReleased, LV_EVENT_RELEASED, (void *)(uintptr_t)1);

    mic2GainText = lv_label_create(screen);
    lv_obj_set_pos(mic2GainText, 8, 112);
    mic2GainSlider = lv_slider_create(screen);
    lv_obj_set_size(mic2GainSlider, 270, 12);
    lv_obj_set_pos(mic2GainSlider, 25, 131);
    lv_slider_set_range(mic2GainSlider, 0, 14);
    lv_obj_add_event_cb(mic2GainSlider, micGainChanged, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)2);
    lv_obj_add_event_cb(mic2GainSlider, micGainReleased, LV_EVENT_RELEASED, (void *)(uintptr_t)2);

    speakerText = lv_label_create(screen);
    lv_obj_set_pos(speakerText, 8, 145);
    speakerSlider = lv_slider_create(screen);
    lv_obj_set_size(speakerSlider, 270, 12);
    lv_obj_set_pos(speakerSlider, 25, 164);
    lv_slider_set_range(speakerSlider, 0, 100);
    lv_obj_add_event_cb(speakerSlider, speakerChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(speakerSlider, speakerReleased, LV_EVENT_RELEASED, nullptr);

    stateText = lv_label_create(screen);
    lv_obj_set_width(stateText, 310);
    lv_obj_set_style_text_color(stateText, lv_color_hex(0xd1d1d6), LV_PART_MAIN);
    lv_obj_set_style_text_font(stateText, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(stateText, LV_ALIGN_TOP_MID, 0, 181);

    lv_obj_t *back = lv_btn_create(screen);
    lv_obj_set_size(back, 52, 27);
    lv_obj_set_pos(back, 4, 207);
    lv_obj_add_event_cb(back, [](lv_event_t *) { closeScreen(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *backText = lv_label_create(back);
    lv_label_set_text(backText, "Zurück");
    lv_obj_center(backText);

    toggleButton = lv_btn_create(screen);
    lv_obj_set_size(toggleButton, 137, 27);
    lv_obj_set_pos(toggleButton, 60, 207);
    lv_obj_add_event_cb(toggleButton, toggleTest, LV_EVENT_CLICKED, nullptr);
    toggleLabel = lv_label_create(toggleButton);
    lv_label_set_text(toggleLabel, "Pegeltest starten");
    lv_obj_center(toggleLabel);

    lv_obj_t *ping = lv_btn_create(screen);
    lv_obj_set_size(ping, 50, 27);
    lv_obj_set_pos(ping, 202, 207);
    lv_obj_add_event_cb(ping, pingSpeaker, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *pingText = lv_label_create(ping);
    lv_label_set_text(pingText, "Ping");
    lv_obj_center(pingText);

    playButton = lv_btn_create(screen);
    lv_obj_set_size(playButton, 61, 27);
    lv_obj_set_pos(playButton, 256, 207);
    lv_obj_add_event_cb(playButton, playRecording, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *playText = lv_label_create(playButton);
    lv_label_set_text(playText, "Abhören");
    lv_obj_center(playText);

    tick = lv_timer_create([](lv_timer_t *) { refresh(); }, 120, nullptr);
    lv_timer_pause(tick);
}
} // namespace

extern "C" void mic_test_open(void)
{
    prevScreen = lv_screen_active();
    if (!screen)
        build();
    if (tick)
        lv_timer_resume(tick);
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    refresh();
}
