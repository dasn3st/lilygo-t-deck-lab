#pragma once

#include <stdbool.h>
#include <stdint.h>

// The chess GAME, kept away from the UI. The engine itself is src/chess/; this owns the current
// position, whose turn it is, and running the search somewhere it cannot freeze the screen.
//
// ⛔ THE SEARCH MUST NOT RUN ON THE UI TASK. That task holds spiLock while it runs, so a
// two-second think would freeze the display AND stall the radio - the exact failure this device
// has a long history with. The search runs on its own "chess" task, on a COPY of the board.
//
// ⛔ AND EVERYTHING ELSE HERE RUNS ON THE UI TASK - including applying the engine's answer
// (tdeck_chess_service(), called from the UI poll via chess_idle_check()). That is what makes the
// board single-threaded, and it is why saving the game needs no spiLock of its own: the UI task
// already holds it. Until 2026-09-29 the answer was applied from loop(), which wrote the save
// file with no lock on the bus the screen and radio share, and could lose the move entirely if
// the app was put away in the same instant - leaving Black to move and nobody to move it.
//
// ⭐ YOU ARE ALWAYS WHITE. Every entry point that moves a piece checks it, so the engine's side
// can never be handed to the player - which is what "it swaps players when I quit and come
// back" was.

#ifdef __cplusplus
extern "C" {
#endif

void tdeck_chess_new_game(void);

// Piece on a square, in 0..63 with 0 = a1 and 63 = h8. 0 = empty, otherwise the engine's
// encoding: colour in bit 3, type in bits 0-2 (1 pawn .. 6 king).
uint8_t tdeck_chess_piece_at(int sq64);

// Is this square part of the move being shown (selected square, or the engine's last move)?
bool tdeck_chess_highlighted(int sq64);

// Legal destinations from a square, for showing the player where a piece can go. Always 0 when it
// is not the player's turn. sq64 < 0 clears the selection.
int tdeck_chess_moves_from(int sq64, uint8_t *dests, int cap);

// Try the player's move. Returns false if it is not legal, which also makes this the validator.
// Promotion is automatic to a queen - see the note in the .cpp.
bool tdeck_chess_try_move(int from64, int to64);

// Whose turn, and whether a think is in progress. "Thinking" lasts until the engine's move is
// actually on the board, not merely until the search task finishes.
bool tdeck_chess_player_turn(void);
bool tdeck_chess_thinking(void);

// Ask the engine to move. Returns false if it is busy, it is not its turn, the game is over, or
// there is not the memory to think (the status line then says so, and a tap retries).
bool tdeck_chess_request_engine(void);

// If it is the engine's turn and nothing is thinking - a game reloaded mid-think, or a search that
// could not start - start it now. Called when the board is opened.
void tdeck_chess_resume(void);

// Take back the last full move (yours and the reply), so one tap undoes what you did. Also
// cancels a think in progress and takes back the move it was answering.
bool tdeck_chess_undo(void);

// 0 playing, 1 you are mated, 2 engine is mated, 3 stalemate, 4 fifty-move draw,
// 5 not enough material, 6 threefold repetition.
int tdeck_chess_result(void);

// One line for the status bar: whose turn, what the engine found, or how it ended.
const char *tdeck_chess_status(void);

// Bumps every time the board or the status changes, so the UI repaints on a change instead of
// guessing from the thinking flag (which misses a move that lands between two polls).
uint32_t tdeck_chess_version(void);

int tdeck_chess_level(void);
void tdeck_chess_set_level(int lv);
const char *tdeck_chess_level_name(void);

// UI task only. Applies a finished search to the board.
void tdeck_chess_service(void);

// True while the chess app owns the engine's tables, so a diagnostic must not touch them.
bool tdeck_chess_engine_busy(void);

#ifdef __cplusplus
}
#endif
