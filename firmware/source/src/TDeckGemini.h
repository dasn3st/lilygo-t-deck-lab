#pragma once
#include <stddef.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Ollama Agent for the colour T-Deck.
//
// The provider is selected through the SD-card configuration and currently targets Ollama Cloud.
//
// ⛔ THE KEY IS NEVER COMPILED IN. It is read at runtime from /ollama.txt on the SD card, so the
// firmware and the web installer can be handed to anyone without handing over Jake's key. Same
// rule as the Max - see gemini-key-TEMPLATE.txt.
//
// ⚠️ USING THIS DROPS BLUETOOTH. Starting wi-fi on this board calls disableBluetooth() (one
// antenna), and BT stays down until the next reboot. Jake accepted that: "Idea is anyway I won't
// Bluetooth much to my tdeck". Worth saying out loud in the UI too, so nobody loses their phone
// link by surprise.
//
// THREADING: ask() only records a question. service(), called from the main loop, drives the
// state machine and the network - the UI task must never block on a socket. This is the same
// deferred pattern as the map, pins, notes and weather work.
// -----------------------------------------------------------------------------

namespace tdeckgemini
{
enum ConversationMode {
    MODE_CHAT = 0,
    MODE_MARKDOWN = 1,
    MODE_CALENDAR = 2
};

enum State {
    IDLE = 0, // nothing asked
    WORKING,  // connecting / sending / waiting
    DONE,     // reply() holds an answer
    FAILED    // statusText() says why
};

void service();               // from the main loop (safe thread)
void ask(const char *prompt); // record a question (safe from the UI task)
void saveSummary();           // ask Ollama for a Markdown summary and save it in /notes
bool hasCalendarDraft();
bool saveCalendarDraft();
bool hasConversation();
int conversationTurns();
const char *conversationText(); // formatted chat transcript for the small-screen UI
void restoreSession();
void deleteSession();
void setMode(ConversationMode mode);
ConversationMode mode();
int state();
const char *reply();      // the answer, "" until DONE
const char *statusText(); // short progress or error line
bool haveConfig();        // /ollama.txt exists and holds a key
bool voiceConfig(char *url, size_t urlCap, char *key, size_t keyCap, char *model, size_t modelCap,
                 char *language, size_t languageCap);
void refreshModels();     // fetch the account's current Ollama Cloud model catalogue
int modelCount();
const char *modelName();
void nextModel();
void clear();             // drop the answer, back to IDLE
void release();           // free the Ollama buffers after the UI has gone idle
} // namespace tdeckgemini
