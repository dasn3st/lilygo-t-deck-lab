#include "configuration.h"

#include "TDeckChess.h"
#include "chess/chess.h"
#include "chess/search.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h> // xTaskCreatePinnedToCoreWithCaps - the search's stack in PSRAM
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/common/SdCard.h"

// -----------------------------------------------------------------------------------------
// The chess game. Jake, 2026-09-22: "local chess with a chess ai kinda thing with difficulty
// settings up to master".
//
// ⛔ THE SEARCH RUNS ON ITS OWN TASK, not the UI task and not the main loop. Measured on this
// chip the engine does ~16-20 thousand positions a second, so a deep think is many seconds. The
// UI task holds spiLock while it runs, so searching there would freeze the screen and stall the
// radio together; the main loop is where Meshtastic services the radio, so searching THERE stops
// packets being drained on a device that hears 248 nodes. A separate low-priority task blocks
// neither. Jake: "I don't mind if it takes 10 seconds of chess doesn't freeze anything."
//
// ⭐ AND IT SEARCHES A COPY OF THE BOARD. Alpha-beta makes and unmakes moves as it goes, so the
// position is transiently wrong for the whole search - and the UI reads the real one to draw.
//
// ⭐ EVERYTHING EXCEPT THE SEARCH IS ON THE UI TASK - see the note in TDeckChess.h. The search
// task only ever writes s_result and raises s_searchDone; the UI task picks that up.
// -----------------------------------------------------------------------------------------

static const uint8_t kPlayer = WHITE;
static const uint8_t kEngine = BLACK;

static Board s_board;
static bool s_boardReady = false; // the position is loaded - independent of the engine's tables

// From the moment a search is requested until its move is ON THE BOARD. Deliberately not cleared
// by the search task: the gap between "search finished" and "move applied" is exactly where the
// player used to be able to pick up Black's pieces.
static volatile bool s_thinking = false;
static volatile bool s_searchDone = false; // set by the search task, consumed by the UI task
static volatile bool s_searchOk = false;
// The search task is working (from the give until the UI task has collected its answer). Kept
// apart from s_thinking because a CANCELLED search is still winding down for a moment after the
// player has been handed the board back - and a new search must not start on top of it.
static bool s_taskBusy = false;
static bool s_discard = false; // a New game / Take back overtook the search; throw its answer away
static bool s_pending = false; // a search was asked for while a cancelled one was winding down

// ⭐ THE ENGINE SEARCHES A COPY, NEVER THE LIVE BOARD. See above.
static Board s_searchBoard;
static Move s_result;

// Its own task, so ten seconds of thinking blocks nothing: not the screen, not the radio, not
// the main loop. Priority 1 - the same as the UI task and below the loop - so it yields to both.
static TaskHandle_t s_task = nullptr;
static bool s_taskStackInPsram = false; // decides which delete call is right
static SemaphoreHandle_t s_go = nullptr;
static const uint32_t kThinkMs = 10000; // Jake: "I don't mind if it takes 10 seconds"
static int s_level = CHESS_CLUB;
static char s_status[64] = "Your move";
static uint32_t s_version = 1;
static uint8_t s_lastFrom = 0xFF, s_lastTo = 0xFF; // the engine's reply, for highlighting
static uint8_t s_selected = 0xFF;                  // set by the UI via tdeck_chess_moves_from

// Undo history - and, through each entry's hash, the positions the repetition rule counts.
// ⛔ A FULL STACK DROPS ITS OLDEST ENTRY; it must never refuse a move. It used to refuse, so at
// move 32 (64 plies) the player's taps silently did nothing and the game could not continue.
#define UNDO_MAX 64
static Undo s_undo[UNDO_MAX];
static int s_undoN = 0;

static void setStatus(const char *s)
{
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    s_version++;
}

static Undo *pushUndo(void)
{
    if (s_undoN == UNDO_MAX) {
        memmove(&s_undo[0], &s_undo[1], sizeof(Undo) * (UNDO_MAX - 1));
        s_undoN--;
    }
    return &s_undo[s_undoN++];
}

static void *psram(unsigned long n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

// ⭐ ONE LINE OF FEN, then the level. The standard way to write a position down, and
// chess_get_fen/set_fen already exist for the perft tests. Written after every move - a game is
// a handful of moves an hour, so the SD cost is nothing, and the alternative is losing the game
// to a flat battery. The level rides along so a reboot does not quietly drop you back to Club.
//
// ⛔ UI TASK ONLY. That task holds spiLock, which the SD card shares with the screen and radio.
static const char *kSaveFile = "/chess.fen";

static void saveGame(void)
{
    char fen[100];
    chess_get_fen(&s_board, fen, sizeof(fen));
    FsFile f = SDFs.open(kSaveFile, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f)
        return; // no card, or the bus is busy: the game simply is not saved this move
    f.println(fen);
    f.print("level=");
    f.println(s_level);
    f.close();
}

static bool loadGame(void)
{
    FsFile f = SDFs.open(kSaveFile, O_RDONLY);
    if (!f)
        return false;
    char fen[100] = {0}, extra[32] = {0};
    const int n = f.fgets(fen, sizeof(fen));
    f.fgets(extra, sizeof(extra)); // absent in saves from before 2026-09-29; that is fine
    f.close();
    if (n < 10)
        return false;
    for (char *p = fen; *p; p++)
        if (*p == '\n' || *p == '\r') {
            *p = 0;
            break;
        }
    // ⛔ A REJECTED FEN MUST NOT LEAVE A HALF-PARSED BOARD. chess_set_fen writes as it goes, so
    // parse into a scratch board and only adopt it if the whole thing was valid.
    Board tmp;
    if (!chess_set_fen(&tmp, fen))
        return false;
    s_board = tmp;
    if (!strncmp(extra, "level=", 6)) {
        const int lv = atoi(extra + 6);
        if (lv >= 0 && lv < CHESS_LEVELS)
            s_level = lv;
    }
    return true;
}

// The POSITION, once per boot. The engine's big tables are a separate matter (engineReady()):
// they come and go with the app, but the board, the undo history and the level stay in RAM - a
// few hundred bytes - so putting the app away no longer costs you your Take back.
static bool ensureBoard(void)
{
    if (s_boardReady)
        return true;
    // Zobrist keys go to PSRAM from the very first position, table or no table.
    chess_set_alloc(psram);
    Board b;
    if (!chess_set_fen(&b, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"))
        return false; // could not even allocate the hash keys; try again on the next call
    s_board = b;
    if (loadGame())
        setStatus(s_board.side == kPlayer ? "Your move" : "My move");
    s_undoN = 0;
    s_boardReady = true;
    s_version++;
    return true;
}

// ⭐ EVERYTHING THE ENGINE ALLOCATES GOES TO PSRAM. The transposition table is up to 1MB and the
// move stack another 110KB; in internal RAM they would take the whole device down. search_init()
// now shrinks the table to fit rather than giving up (see search.c).
static bool engineReady(void)
{
    if (!search_ready()) {
        // ⛔ LEAVE 512KB FOR EVERYONE ELSE. Taking the biggest table that fitted left 132KB of
        // PSRAM after Maps had been open, and 20 seconds later a node-DB save could not get its
        // 80KB and the device aborted (stress test, 2026-09-29). Halve the table until what is left
        // afterwards stays comfortable; a 256KB table plays a few percent weaker, which nobody at
        // Club level will ever notice, and a crash everyone does.
        const unsigned long kKeepFree = 512UL * 1024UL;
        const unsigned long kOtherTables = 192UL * 1024UL; // move stack, history, killers, hashes
        const unsigned long freePs = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        unsigned long table = 1024UL * 1024UL;
        while (table > 64UL * 1024UL && freePs < table + kOtherTables + kKeepFree)
            table /= 2;
        if (freePs < 64UL * 1024UL + kOtherTables + kKeepFree)
            return false; // not even a small table without crowding everything else out
        search_init(psram, table);
    }
    return search_ready();
}

// The positions of this game so far, for the search's repetition check. Rebuilt from the undo
// stack before every search because search_release() throws the search's own copy away.
static void seedSearchHistory(void)
{
    search_history_clear();
    for (int i = 0; i < s_undoN; i++)
        search_history_push(s_undo[i].hash);
    search_history_push(s_board.hash);
}

static int repetitions(void)
{
    int n = 1; // the position on the board now
    for (int i = 0; i < s_undoN; i++)
        if (s_undo[i].hash == s_board.hash)
            n++;
    return n;
}

#define SQ88(sq64) (((sq64) / 8) * 16 + ((sq64) % 8))
#define SQ64OF(sq88) (((sq88) >> 4) * 8 + ((sq88) & 7))

// Stop thinking about the position on the board, because it is about to change. The player gets
// the board back IMMEDIATELY; the search task's answer, when it comes, is thrown away.
static void cancelThinking(void)
{
    if (!s_thinking && !s_taskBusy)
        return;
    s_thinking = false;
    s_pending = false;
    if (s_taskBusy) {
        s_discard = true;
        search_stop();
    }
}

void tdeck_chess_new_game(void)
{
    ensureBoard();
    cancelThinking(); // its answer belongs to the old game
    chess_init(&s_board);
    s_undoN = 0;
    s_lastFrom = s_lastTo = 0xFF;
    s_selected = 0xFF;
    saveGame();
    setStatus("Your move");
}

uint8_t tdeck_chess_piece_at(int sq64)
{
    if (!ensureBoard() || sq64 < 0 || sq64 > 63)
        return 0;
    return s_board.board[SQ88(sq64)];
}

bool tdeck_chess_highlighted(int sq64)
{
    const uint8_t sq = (uint8_t)SQ88(sq64);
    return sq == s_lastFrom || sq == s_lastTo || sq == s_selected;
}

int tdeck_chess_moves_from(int sq64, uint8_t *dests, int cap)
{
    s_selected = 0xFF;
    // Only ever the player's pieces, only on the player's turn. Without this, a board left with
    // Black to move let you pick up and play Black's pieces - the "swap".
    if (sq64 < 0 || sq64 > 63 || !ensureBoard() || s_thinking || s_board.side != kPlayer)
        return 0;
    s_selected = (uint8_t)SQ88(sq64);
    Move list[MAX_MOVES];
    const int n = chess_gen_moves(&s_board, list);
    int out = 0;
    for (int i = 0; i < n && out < cap; i++)
        if (list[i].from == s_selected)
            dests[out++] = (uint8_t)SQ64OF(list[i].to);
    if (!out)
        s_selected = 0xFF; // nothing to show; do not leave a square lit with no moves
    return out;
}

bool tdeck_chess_try_move(int from64, int to64)
{
    if (!ensureBoard() || s_thinking || s_board.side != kPlayer)
        return false;
    if (from64 < 0 || from64 > 63 || to64 < 0 || to64 > 63)
        return false;
    const uint8_t f = (uint8_t)SQ88(from64), t = (uint8_t)SQ88(to64);
    Move list[MAX_MOVES];
    const int n = chess_gen_moves(&s_board, list);
    const Move *found = nullptr;
    for (int i = 0; i < n; i++) {
        if (list[i].from != f || list[i].to != t)
            continue;
        // ⭐ AUTO-QUEEN. Four promotion moves share a from/to, so something has to choose. A
        // promotion dialog on a touchscreen this size is a lot of UI for a case that wants a
        // queen more than 95% of the time. Underpromotion is the cost, and it is noted rather
        // than hidden.
        if ((list[i].flags & MOVE_PROMO) && list[i].promo != QUEEN)
            continue;
        found = &list[i];
        break;
    }
    if (!found)
        return false;
    chess_make(&s_board, found, pushUndo());
    s_lastFrom = f;
    s_lastTo = t;
    s_selected = 0xFF;
    saveGame();
    s_version++;
    return true;
}

bool tdeck_chess_player_turn(void)
{
    return ensureBoard() && s_board.side == kPlayer && !s_thinking;
}

bool tdeck_chess_thinking(void)
{
    return s_thinking;
}

bool tdeck_chess_engine_busy(void)
{
    return s_thinking || s_taskBusy || s_searchDone || search_ready();
}

static void chessTask(void *)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        Move m;
        const bool ok = search_best_move(&s_searchBoard, (ChessLevel)s_level, kThinkMs, &m);
        s_result = m;
        s_searchOk = ok;
        s_searchDone = true; // the UI task applies it; s_thinking stays up until it has
    }
}

extern "C" unsigned tdeck_chess_stack_free(void)
{
    return s_task ? (unsigned)(uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t)) : 0;
}

static bool startTask(void)
{
    if (!s_go) {
        s_go = xSemaphoreCreateBinary();
        if (!s_go)
            return false;
    }
    if (s_task)
        return true;
    // ⛔ PINNED TO CORE 1, AWAY FROM THE DISPLAY. Jake: "having chess open is lagging hard,
    // maybe froze my device". An unpinned CPU-bound search was free to land on core 0 - where the
    // "tft" task lives at the SAME priority 1 - and the two time-sliced, halving the UI for ten
    // solid seconds. Core 1 is the Arduino loop's core, which spends most of its time waiting.
    //
    // ⭐ STACK IN PSRAM. 10KB of internal heap is a quarter of what the device has spare, and
    // after Maps it was more than there was - the task could not be created, the engine never
    // replied, and the game sat with Black to move. The search is pure computation (no flash, no
    // SD, no radio), which is exactly the kind of task a PSRAM stack is safe for. Internal RAM is
    // the fallback, not the default.
    s_taskStackInPsram = xTaskCreatePinnedToCoreWithCaps(chessTask, "chess", 10240, nullptr, 1, &s_task, 1,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    if (!s_taskStackInPsram) {
        s_task = nullptr;
        if (xTaskCreatePinnedToCore(chessTask, "chess", 10240, nullptr, 1, &s_task, 1) != pdPASS) {
            s_task = nullptr;
            return false;
        }
    }
    return true;
}

static void setEndStatus(void)
{
    switch (tdeck_chess_result()) {
    case 1: setStatus("Checkmate - you lose"); break;
    case 2: setStatus("Checkmate - you win"); break;
    case 3: setStatus("Stalemate - a draw"); break;
    case 4: setStatus("Draw - fifty moves, no capture"); break;
    case 5: setStatus("Draw - not enough pieces to mate"); break;
    case 6: setStatus("Draw - same position three times"); break;
    default: break;
    }
}

static void launchSearch(void)
{
    seedSearchHistory();
    s_searchBoard = s_board; // the copy it is allowed to scribble on
    s_searchDone = false;
    s_discard = false;
    search_clear_stop(); // HERE, not inside the search - a Stop that lands first must still count
    s_taskBusy = true;
    xSemaphoreGive(s_go);
}

bool tdeck_chess_request_engine(void)
{
    if (!ensureBoard() || s_thinking || s_board.side != kEngine)
        return false;
    if (tdeck_chess_result()) {
        setEndStatus();
        return false;
    }
    // ⭐ SAY WHEN IT CANNOT THINK, AND MAKE A TAP RETRY. The old code returned false and nothing
    // heard it: the screen said "Your move" with Black to move, and the next piece you touched
    // was Black's.
    if (!engineReady() || !startTask()) {
        setStatus("Low on memory - tap the board to let me try again");
        return false;
    }
    s_thinking = true;
    setStatus("Thinking...");
    if (s_taskBusy)
        s_pending = true; // a cancelled search is still stopping; start as soon as it has
    else
        launchSearch();
    return true;
}

void tdeck_chess_resume(void)
{
    if (ensureBoard() && !s_thinking && s_board.side == kEngine)
        tdeck_chess_request_engine();
}

// Free the thinking task and the tables when the app is put away. Only when idle.
extern "C" void tdeck_chess_release(void)
{
    if (s_thinking || s_taskBusy || s_searchDone)
        return; // never tear down under a live search, or with its answer still to apply
    if (s_task) {
        if (s_taskStackInPsram)
            vTaskDeleteWithCaps(s_task);
        else
            vTaskDelete(s_task);
        s_task = nullptr;
    }
    // ⭐ AND HAND BACK THE 1.25MB OF PSRAM. Jake asked the right question - "if I close chess is
    // it always in ram?" - and it was: measured 2,505,728 bytes free before opening chess and
    // 1,254,964 after, never recovered. The tables are a CACHE; the game itself stays in RAM here
    // (and on the card in /chess.fen), so throwing them away loses nothing.
    search_release();
}

bool tdeck_chess_undo(void)
{
    if (!ensureBoard())
        return false;
    // Take back while it is thinking = "no, not that move". Cancel the think and undo yours.
    cancelThinking();
    // Back to a position where it is YOUR move: the engine's reply and your move before it, or
    // just your move if the engine never answered it.
    int take = (s_board.side == kPlayer) ? 2 : 1;
    if (s_undoN < take) {
        // Not pretending: the history starts where this session picked the game up from the card.
        setStatus("Nothing to take back");
        return false;
    }
    while (take--)
        chess_unmake(&s_board, &s_undo[--s_undoN]);
    s_lastFrom = s_lastTo = 0xFF;
    s_selected = 0xFF;
    saveGame();
    setStatus("Took that back");
    return true;
}

int tdeck_chess_result(void)
{
    if (!ensureBoard())
        return 0;
    const int over = chess_game_over(&s_board);
    if (over == 1)
        return s_board.side == kPlayer ? 1 : 2; // whoever is to move is the one mated
    if (over == 2)
        return 3;
    if (over == 3)
        return 4;
    if (over == 4)
        return 5;
    if (repetitions() >= 3)
        return 6;
    return 0;
}

const char *tdeck_chess_status(void)
{
    return s_status;
}

uint32_t tdeck_chess_version(void)
{
    return s_version;
}

int tdeck_chess_level(void)
{
    return s_level;
}

void tdeck_chess_set_level(int lv)
{
    if (lv < 0 || lv >= CHESS_LEVELS)
        lv = CHESS_CLUB;
    // Load the game FIRST: loading reads the saved level, and doing it afterwards would quietly
    // put back the old one.
    const bool haveBoard = ensureBoard();
    s_level = lv;
    if (haveBoard)
        saveGame(); // the level is part of the save now
    s_version++;
}

const char *tdeck_chess_level_name(void)
{
    return chess_level_name((ChessLevel)s_level);
}

// UI task. The search happened on the chess task; this lands its move on the real board, from
// the same task as every other change to it.
void tdeck_chess_service(void)
{
    if (!s_searchDone)
        return;
    const Move m = s_result;
    const bool ok = s_searchOk;
    s_searchDone = false;
    s_taskBusy = false;
    if (s_discard) { // overtaken by New game / Take back - they have already set the status
        s_discard = false;
        LOG_INFO("[CHESS] search discarded");
        if (s_pending) { // and the new position is already waiting for its own
            s_pending = false;
            launchSearch();
        }
        return;
    }
    LOG_INFO("[CHESS] search done, stack headroom %u bytes, table %lu entries", tdeck_chess_stack_free(),
             search_table_entries());

    bool applied = false;
    if (ok && s_board.side == kEngine) {
        // Belt and braces: only ever play a move that is legal HERE. The copy it searched is the
        // same position unless something above slipped, and a wrong move on the real board would
        // corrupt the game for good.
        Move list[MAX_MOVES];
        const int n = chess_gen_moves(&s_board, list);
        for (int i = 0; i < n; i++) {
            if (list[i].from == m.from && list[i].to == m.to && list[i].promo == m.promo) {
                chess_make(&s_board, &list[i], pushUndo());
                s_lastFrom = m.from;
                s_lastTo = m.to;
                applied = true;
                break;
            }
        }
    }
    s_thinking = false;
    if (!applied) {
        if (tdeck_chess_result())
            setEndStatus();
        else
            setStatus("Lost my train of thought - tap the board");
        return;
    }
    saveGame();

    SearchInfo in;
    search_last_info(&in);
    char mv[6];
    chess_move_str(&m, mv);
    if (tdeck_chess_result()) {
        setEndStatus();
    } else if (in.mateIn) {
        // mateIn is from the ENGINE's side: positive, it is mating you; negative, you can mate it.
        char buf[64];
        if (in.mateIn > 0)
            snprintf(buf, sizeof(buf), "%s - I have mate in %d", mv, in.mateIn);
        else
            snprintf(buf, sizeof(buf), "%s - you have mate in %d", mv, -in.mateIn);
        setStatus(buf);
    } else {
        // ⛔ NEGATED, AND THE OLD COMMENT HERE WAS WRONG. The score is worked out at the ROOT of
        // the search, where the side to move is the ENGINE - so positive meant good for the
        // engine, and "+2.0" appeared when you were two pawns down. Shown from your side now:
        // positive means you are better.
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  depth %d  %+.1f", mv, in.depth, -in.score / 100.0);
        setStatus(buf);
    }
}
