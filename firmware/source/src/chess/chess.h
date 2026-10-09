#pragma once

// A small, complete chess engine for the T-Deck. Jake, 2026-09-22: "local chess with a chess ai
// kinda thing with difficulty settings up to master".
//
// ⭐ DELIBERATELY PLAIN C WITH NO DEVICE DEPENDENCIES, so the exact same source compiles on the
// PC. That is not tidiness for its own sake: a chess move generator is the classic place to
// ship a bug that looks like nothing for a hundred moves and then loses a game, and the only
// real defence is perft - walk every legal move to depth N from known positions and compare the
// leaf count against published values. One wrong en-passant or castling rule changes the count.
// Doing that on the PC takes seconds; doing it on the device would be miserable and slow.
//
// Board representation is 0x88: a 16x8 array where a square is off-board if (sq & 0x88). That
// makes the bounds check for every sliding move a single AND, and makes knight and king moves
// simple offsets. Bitboards would be faster but are far more code to get right, and at the
// depths this hardware can reach the difference is not what limits playing strength.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Piece encoding: colour in bit 3, type in bits 0-2. EMPTY is 0 so a cleared board is empty.
enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum { WHITE = 0, BLACK = 1 };

#define PIECE(colour, type) ((uint8_t)(((colour) << 3) | (type)))
#define PTYPE(p) ((uint8_t)((p) & 7))
#define PCOLOUR(p) ((uint8_t)(((p) >> 3) & 1))
#define IS_EMPTY(p) ((p) == EMPTY)

// Castling rights, one bit each.
enum { CASTLE_WK = 1, CASTLE_WQ = 2, CASTLE_BK = 4, CASTLE_BQ = 8 };

typedef struct {
    uint8_t from, to;
    uint8_t promo;  // piece TYPE promoted to, or 0
    uint8_t flags;  // MOVE_* below
    int16_t score;  // move-ordering score, filled by the search
} Move;

enum { MOVE_CAPTURE = 1, MOVE_ENPASSANT = 2, MOVE_CASTLE = 4, MOVE_DOUBLEPUSH = 8, MOVE_PROMO = 16 };

// Everything needed to unmake a move. Kept in a stack by the search rather than copying the
// whole board, which at these depths is the difference between usable and not.
typedef struct {
    Move move;
    uint8_t captured;
    uint8_t castling;
    uint8_t ep;
    uint8_t halfmove;
    uint64_t hash;
} Undo;

typedef struct {
    uint8_t board[128]; // 0x88
    uint8_t side;       // side to move
    uint8_t castling;
    uint8_t ep;        // en-passant TARGET square, or 0x7F for none
    uint8_t halfmove;  // for the fifty-move rule
    uint16_t fullmove;
    uint8_t king[2];   // king square per colour, kept incrementally
    uint64_t hash;     // Zobrist
} Board;

#define NO_EP 0x7F
#define MAX_MOVES 256

// Supply the allocator the engine uses for its big cold tables (Zobrist keys here, the history
// table and move stack in search.c). Call before chess_init. Default is malloc.
//
// ⛔ ON THE DEVICE THIS MUST RETURN PSRAM. Left on the default these tables sit in internal
// static RAM and take 91KB of it, which is more than the whole device has spare - it boots and
// then cannot allocate. See the note at the top of chess.c.
void chess_set_alloc(void *(*fn)(unsigned long bytes));

// Set up the standard opening position.
void chess_init(Board *b);

// Parse a FEN string. Returns false if it is malformed. Used heavily by the perft tests.
bool chess_set_fen(Board *b, const char *fen);

// Write the current position as FEN into buf (at least 96 bytes).
void chess_get_fen(const Board *b, char *buf, int cap);

// Generate all LEGAL moves into list, returning how many. Legality is checked properly - moves
// that leave or place your own king in check are not returned, so the caller never has to think
// about it and "no moves" unambiguously means checkmate or stalemate.
int chess_gen_moves(Board *b, Move *list);

// Generate only captures and promotions - the quiescence search needs this.
int chess_gen_captures(Board *b, Move *list);

void chess_make(Board *b, const Move *m, Undo *u);
void chess_unmake(Board *b, const Undo *u);

// Is `colour`'s king currently attacked?
bool chess_in_check(const Board *b, uint8_t colour);

// Is square `sq` attacked by any piece of `bySide`?
bool chess_attacked(const Board *b, uint8_t sq, uint8_t bySide);

// Count leaf nodes at the given depth. THE correctness test - see the note at the top.
uint64_t chess_perft(Board *b, int depth);

// "e2e4", or "e7e8q" for a promotion. buf needs 6 bytes.
void chess_move_str(const Move *m, char *buf);

// Parse a move in that same notation against the current position. Returns false if it is not
// a legal move here, which also makes this the input validator.
bool chess_parse_move(Board *b, const char *str, Move *out);

// 0 = game continues, 1 = checkmate (side to move loses), 2 = stalemate,
// 3 = fifty-move rule, 4 = insufficient material.
int chess_game_over(Board *b);

#ifdef __cplusplus
}
#endif
