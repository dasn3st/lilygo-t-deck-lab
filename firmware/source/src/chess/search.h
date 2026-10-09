#pragma once

#include "chess.h"

#ifdef __cplusplus
extern "C" {
#endif

// Difficulty. Named for how they PLAY, not with rating numbers - a number on a menu is a claim,
// and until the engine has actually been measured against rated opposition it would be a made-up
// one. Jake asked for "up to master"; what the top level really is gets measured, then labelled.
//
// ⭐ THE WEAK LEVELS ARE NOT JUST SHALLOW. A depth-2 search is still tactically perfect inside
// its horizon, so it plays like a machine with amnesia: it never hangs a piece to a one-move
// threat but walks into everything else. That is unpleasant to play and does not feel like a
// weaker opponent. So the low levels also pick from among the top few moves with weighted
// randomness, which produces the inconsistency a human beginner actually has.
typedef enum {
    CHESS_BEGINNER = 0, // depth 2, picks loosely among decent moves
    CHESS_CASUAL,       // depth 3, a bit of randomness
    CHESS_CLUB,         // depth 5, plays its best move
    CHESS_STRONG,       // depth 7 or ~3s
    CHESS_EXPERT,       // depth 9 or ~8s
    CHESS_MAX,          // as deep as it gets in the time allowed
    CHESS_LEVELS
} ChessLevel;

const char *chess_level_name(ChessLevel lv);

// One-time setup: allocates the transposition table. allocFn may be NULL to use malloc; the
// device passes a PSRAM allocator, because the table is big, cold and must not touch the
// internal heap (largest free block there measured at 20KB).
void search_init(void *(*allocFn)(unsigned long bytes), unsigned long tableBytes);

// Pick a move. Returns false only if there are no legal moves at all.
// `msBudget` caps thinking time; 0 means "use the level's own default".
bool search_best_move(Board *b, ChessLevel lv, uint32_t msBudget, Move *out);

// Stats from the last search, for the UI and for strength testing.
typedef struct {
    int depth;         // deepest completed iteration
    int score;         // centipawns from the side-to-move's point of view
    uint32_t nodes;
    uint32_t ms;
    int mateIn;        // 0 if none; >0 = we mate, <0 = we get mated
} SearchInfo;
void search_last_info(SearchInfo *info);

// Positions seen in the game so far, for the threefold-repetition rule. The caller pushes after
// every move and pops on takeback; the search consults it so it neither claims nor walks into a
// draw by accident.
void search_history_clear(void);
void search_history_push(uint64_t hash);
void search_history_pop(void);
// How many times this position has occurred in the game history - 3 is a draw by repetition.
int search_history_count(uint64_t hash);

// True once search_init() has everything the search needs. The transposition table is optional
// (it shrinks to fit, or is skipped) so this is the one question that matters to a caller.
bool search_ready(void);
unsigned long search_table_entries(void); // for diagnostics: 0 means it is playing without one

// Static evaluation, centipawns, positive = good for the side to move. Exposed for tests.
int eval_position(const Board *b);

// Free everything search_init() allocated - over 1.2MB of PSRAM. Safe only when no search is
// running; search_init() reallocates on next use. The game itself is saved to a file, so all of
// this is a rebuildable cache.
void search_release(void);

// Abort the current search from another thread/task. On the device the UI sets this so a long
// think can be cancelled by the user rather than freezing the app.
void search_stop(void);
// Re-arm after a stop. Call before starting a search (search_best_move does not, on purpose).
void search_clear_stop(void);

#ifdef __cplusplus
}
#endif
