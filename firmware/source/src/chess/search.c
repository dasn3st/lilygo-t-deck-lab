#include "search.h"
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#define NOW_MS() ((uint32_t)millis())
#else
#include <time.h>
static uint32_t NOW_MS(void) { return (uint32_t)(clock() * 1000ULL / CLOCKS_PER_SEC); }
#endif

// ---------------------------------------------------------------------------------------
// Alpha-beta search with iterative deepening, a transposition table, quiescence search and
// the usual move-ordering heuristics. Plain C, no device dependencies, so the same code runs
// on the PC - which is how its strength gets MEASURED rather than claimed.
// ---------------------------------------------------------------------------------------

#define INF 30000
#define MATE 29000
#define MAX_PLY 64

// ---- evaluation --------------------------------------------------------------------------
// Centipawns. A pawn is 100. These are ordinary values; the piece-square tables matter more to
// how it PLAYS than the exact material numbers do.
static const int kMat[7] = {0, 100, 320, 330, 500, 900, 0};

// Piece-square tables, from White's point of view, rank 1 first. Black reads them mirrored.
// They encode the handful of ideas that stop an engine playing like a calculator: knights
// belong in the middle, bishops on long diagonals, rooks on the 7th, pawns want to advance,
// and the king wants a corner in the middlegame and the centre in the endgame.
static const int kPst[7][64] = {
    {0},
    {// pawn
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10,-20,-20, 10, 10,  5,
     5, -5,-10,  0,  0,-10, -5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5,  5, 10, 25, 25, 10,  5,  5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
     0,  0,  0,  0,  0,  0,  0,  0},
    {// knight
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50},
    {// bishop
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10,-10,-10,-10,-10,-20},
    {// rook
     0,  0,  5, 10, 10,  5,  0,  0,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0},
    {// queen
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -10,  5,  5,  5,  5,  5,  0,-10,
     0,  0,  5,  5,  5,  5,  0, -5,
    -5,  0,  5,  5,  5,  5,  0, -5,
   -10,  0,  5,  5,  5,  5,  0,-10,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20},
    {// king, middlegame: stay tucked away
    20, 30, 10,  0,  0, 10, 30, 20,
    20, 20,  0,  0,  0,  0, 20, 20,
   -10,-20,-20,-20,-20,-20,-20,-10,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30},
};

// In the endgame the king is a strong piece and belongs in the middle. Using the middlegame
// table all the way through is why naive engines shuffle their king on the back rank in a king
// and pawn ending and fail to win it.
static const int kKingEnd[64] = {
   -50,-30,-30,-30,-30,-30,-30,-50,
   -30,-30,  0,  0,  0,  0,-30,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 30, 40, 40, 30,-10,-30,
   -30,-10, 20, 30, 30, 20,-10,-30,
   -30,-20,-10,  0,  0,-10,-20,-30,
   -50,-40,-30,-20,-20,-30,-40,-50};

#define SQ64(sq) (((sq) >> 4) * 8 + ((sq) & 7))
#define MIRROR(i) ((i) ^ 56) // flip rank for Black

int eval_position(const Board *b)
{
    int mg[2] = {0, 0};
    int material[2] = {0, 0};
    int bishops[2] = {0, 0};
    int pawnsOnFile[2][8];
    memset(pawnsOnFile, 0, sizeof(pawnsOnFile));
    int pieceCount = 0;

    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88)
            continue;
        uint8_t pc = b->board[sq];
        if (!pc)
            continue;
        uint8_t c = PCOLOUR(pc), t = PTYPE(pc);
        material[c] += kMat[t];
        if (t != PAWN && t != KING)
            pieceCount++;
        if (t == BISHOP)
            bishops[c]++;
        if (t == PAWN)
            pawnsOnFile[c][sq & 7]++;
        int idx = SQ64(sq);
        if (c == BLACK)
            idx = MIRROR(idx);
        mg[c] += kPst[t][idx];
    }

    // Endgame weighting. Sliding between the two king tables rather than switching at a
    // threshold avoids the engine changing its mind about where its king belongs because a
    // single piece came off.
    int phase = pieceCount > 12 ? 12 : pieceCount; // 12 = full board of pieces
    for (int c = 0; c < 2; c++) {
        int ksq = SQ64(b->king[c]);
        if (c == BLACK)
            ksq = MIRROR(ksq);
        int mgK = kPst[KING][ksq], egK = kKingEnd[ksq];
        mg[c] += (mgK * phase + egK * (12 - phase)) / 12;
    }

    for (int c = 0; c < 2; c++) {
        if (bishops[c] >= 2)
            mg[c] += 30; // the bishop pair is worth about half a pawn
        for (int f = 0; f < 8; f++) {
            if (pawnsOnFile[c][f] > 1)
                mg[c] -= 15 * (pawnsOnFile[c][f] - 1); // doubled
            if (pawnsOnFile[c][f]) {
                bool left = f > 0 && pawnsOnFile[c][f - 1];
                bool right = f < 7 && pawnsOnFile[c][f + 1];
                if (!left && !right)
                    mg[c] -= 12; // isolated
            }
        }
    }

    int score = (material[WHITE] + mg[WHITE]) - (material[BLACK] + mg[BLACK]);
    return b->side == WHITE ? score : -score;
}

// ---- transposition table --------------------------------------------------------------------
enum { TT_EXACT = 0, TT_ALPHA = 1, TT_BETA = 2 };
typedef struct {
    uint64_t key;
    int16_t score;
    uint8_t depth;
    uint8_t flag;
    uint8_t from, to, promo, pad;
} TTEntry;

static TTEntry *s_tt = NULL;
static uint32_t s_ttCount = 0;

// ⛔ THE MOVE LISTS LIVE HERE, NOT ON THE CALL STACK. See the note in chess.c: at 1536 bytes a
// list and 22+ plies of depth, keeping them as locals cost ~66KB of stack on a device whose
// largest task stack is 16KB. One flat allocation indexed by ply costs the same memory once
// instead of once per level, and it goes in PSRAM where there is room for it.
static Move *s_moveStack = NULL;

// ⛔ 64KB AS A PLAIN STATIC - by far the biggest single piece of the 91KB that stopped the
// device booting properly. Allocated from the same PSRAM block as everything else now.
static int16_t (*s_history)[128]; // [128][128], int16 is ample for a move-ordering score

// ⛔ 4KB of internal RAM for a list of position hashes - cold, and touched once per move.
// Internal RAM is the scarce thing on this device and the TLS handshake needs 34KB of it
// CONTIGUOUS; every static kilobyte here is one the radio and wi-fi cannot have.
static uint64_t *s_gameHist = NULL;
static int s_gameHistN = 0;
// Pushes that did not fit. Counted so the matching pops cancel THEM rather than eating real
// entries - otherwise a very long game would silently lose the positions repetition needs.
static int s_gameHistLost = 0;

// PSRAM with the rest of the search tables: internal RAM is what the TLS handshake
// needs contiguously, and 768 bytes of it for move ordering is a bad trade.
static Move (*s_killers)[2];

#define PLY_LIST(ply) (s_moveStack + (size_t)(ply) * MAX_MOVES)

void search_init(void *(*allocFn)(unsigned long bytes), unsigned long tableBytes)
{
    // ⛔ "ALREADY SET UP" MEANS THE MOVE STACK, NOT THE TABLE. The search cannot run without the
    // stack; it plays perfectly well (a little weaker) without the table. This used to key off
    // the table and bail out the moment the 1MB allocation failed - which after the map tile
    // cache had taken 1.5MB of PSRAM it did, every time (largest free block 1,015,796 bytes,
    // measured 2026-09-29) - leaving NOTHING allocated and the engine answering "No move".
    if (s_moveStack)
        return;
    // The rules half needs an allocator too, for its Zobrist tables. First, so a failure below
    // cannot leave it on the default.
    chess_set_alloc(allocFn);

    if (tableBytes < sizeof(TTEntry) * 1024)
        tableBytes = sizeof(TTEntry) * 1024;
    uint32_t n = (uint32_t)(tableBytes / sizeof(TTEntry));
    // Round DOWN to a power of two so the index is a mask, not a modulo. This is on the hot
    // path of every node; a 64-bit division there is genuinely expensive on this chip.
    uint32_t p = 1;
    while (p * 2 <= n)
        p *= 2;
    // Take the biggest table there is room for, halving down to 16K entries rather than giving up.
    // A quarter-size table costs a few percent of strength; no table at all still plays.
    s_tt = NULL;
    s_ttCount = 0;
    for (; p >= 1024; p /= 2) {
        s_tt = (TTEntry *)(allocFn ? allocFn((unsigned long)p * sizeof(TTEntry)) : calloc(p, sizeof(TTEntry)));
        if (s_tt)
            break;
    }
    if (s_tt) {
        memset(s_tt, 0, (size_t)p * sizeof(TTEntry));
        s_ttCount = p;
    }

    // One list per ply, plus a margin: quiescence can extend past MAX_PLY in a wild position and
    // running off the end of this would corrupt whatever follows it rather than simply playing
    // badly. The depth guard in quiesce() is the real limit; this is the belt.
    const size_t plies = MAX_PLY + 8;
    s_moveStack = (Move *)(allocFn ? allocFn((unsigned long)(plies * MAX_MOVES * sizeof(Move)))
                                   : calloc(plies * MAX_MOVES, sizeof(Move)));
    s_history = (int16_t(*)[128])(allocFn ? allocFn(128UL * 128UL * sizeof(int16_t))
                                          : calloc(128UL * 128UL, sizeof(int16_t)));
    s_gameHist = (uint64_t *)(allocFn ? allocFn(512UL * sizeof(uint64_t)) : calloc(512UL, sizeof(uint64_t)));
    s_killers = (Move(*)[2])(allocFn ? allocFn((unsigned long)MAX_PLY * 2 * sizeof(Move))
                                     : calloc((size_t)MAX_PLY * 2, sizeof(Move)));
    if (s_history)
        memset(s_history, 0, 128UL * 128UL * sizeof(int16_t));
    // All or nothing for the parts the search indexes without checking. A half-built set would be
    // worse than none: search_ready() would say yes and the search would walk off a null pointer.
    if (!s_moveStack || !s_gameHist || !s_killers || !s_history)
        search_release();
}

bool search_ready(void)
{
    return s_moveStack != NULL;
}

unsigned long search_table_entries(void)
{
    return s_ttCount;
}

// ---- search state ----------------------------------------------------------------------------
static uint32_t s_nodes;
static uint32_t s_deadline;
static volatile bool s_abort;
static bool s_timeUp;
static SearchInfo s_info;

void search_stop(void) { s_abort = true; }
// ⛔ CLEARED BY THE CALLER BEFORE A SEARCH, NOT BY THE SEARCH ITSELF. search_best_move() used to
// reset the flag on entry, so a Stop that arrived between "start thinking" and the search task
// actually starting was wiped out, and a cancelled think ran its full ten seconds.
void search_clear_stop(void) { s_abort = false; }

// ⭐ GIVE THE TABLES BACK. Measured on the device: opening chess takes 1,250,764 bytes of PSRAM
// - a 1MB transposition table, a 96KB move stack, a 32KB history table - and before this it was
// never returned. Half the free PSRAM on the device, held for the rest of the session because
// somebody looked at a chessboard once, when the map tile cache wants up to 1.5MB of the same
// pool.
//
// This is only safe because the GAME does not live here: the position is written to /chess.fen
// after every move, so everything below is a cache that can be rebuilt. search_init() reallocates
// on the next use and the board is reloaded from the file.
//
// ⛔ THE SEARCH MUST NOT BE RUNNING. Freeing the move stack from under a live search would be a
// use-after-free on another task. The caller checks.
void search_release(void)
{
    if (s_tt) {
        free(s_tt);
        s_tt = NULL;
        s_ttCount = 0;
    }
    if (s_moveStack) {
        free(s_moveStack);
        s_moveStack = NULL;
    }
    if (s_history) {
        free(s_history);
        s_history = NULL;
    }
    if (s_gameHist) {
        free(s_gameHist);
        s_gameHist = NULL;
        s_gameHistN = 0;
        s_gameHistLost = 0;
    }
    if (s_killers) {
        free(s_killers);
        s_killers = NULL;
    }
}
void search_history_clear(void)
{
    s_gameHistN = 0;
    s_gameHistLost = 0;
}
void search_history_push(uint64_t h)
{
    if (s_gameHist && s_gameHistN < 512 && !s_gameHistLost)
        s_gameHist[s_gameHistN++] = h;
    else
        s_gameHistLost++;
}
void search_history_pop(void)
{
    if (s_gameHistLost)
        s_gameHistLost--;
    else if (s_gameHistN)
        s_gameHistN--;
}
int search_history_count(uint64_t h)
{
    int n = 0;
    for (int i = 0; s_gameHist && i < s_gameHistN; i++)
        if (s_gameHist[i] == h)
            n++;
    return n;
}

static bool isRepetition(const Board *b)
{
    // Twofold inside the search is treated as a draw. Engines do this deliberately: waiting for
    // a true threefold means the search cannot see that a line is going nowhere until far too
    // late, and it lets a losing side walk into a repetition it should have spotted.
    //
    // ⛔ START AT N-2, NOT N-1. The caller pushes the hash immediately after making the move, so
    // the LAST entry IS the position being asked about - comparing against it matches instantly
    // and every node returns "draw" before searching anything.
    // Caught by the search test: it reported depth 30 reached with ZERO nodes searched, and the
    // engine playing the first move in the list every time. A plausible-looking off-by-one that
    // silently turned the whole search off.
    if (!s_gameHist)
        return false;
    for (int i = s_gameHistN - 2; i >= 0 && i >= s_gameHistN - b->halfmove - 1; i--)
        if (s_gameHist[i] == b->hash)
            return true;
    return false;
}

static bool outOfTime(void)
{
    if (s_abort)
        return true;
    // Checking the clock on every node is itself measurable, so only every 2048.
    if ((s_nodes & 2047) == 0 && s_deadline && (int32_t)(NOW_MS() - s_deadline) > 0)
        s_timeUp = true;
    return s_timeUp;
}

// MVV-LVA: try capturing the most valuable victim with the least valuable attacker first. Good
// move ordering is worth more to a search than almost any evaluation term - it is what keeps the
// effective branching factor near 3 instead of near 35.
static void scoreMoves(const Board *b, Move *list, int n, int ply, const Move *ttMove)
{
    for (int i = 0; i < n; i++) {
        Move *m = &list[i];
        if (ttMove && m->from == ttMove->from && m->to == ttMove->to && m->promo == ttMove->promo) {
            m->score = 30000; // the table's move first, always
            continue;
        }
        if (m->flags & MOVE_CAPTURE) {
            uint8_t victim = PTYPE(b->board[m->to]);
            uint8_t attacker = PTYPE(b->board[m->from]);
            m->score = (int16_t)(20000 + kMat[victim] * 10 - kMat[attacker]);
        } else if (m->flags & MOVE_PROMO) {
            m->score = (int16_t)(19000 + kMat[m->promo]);
        } else if (s_killers && ply < MAX_PLY &&
                   ((s_killers[ply][0].from == m->from && s_killers[ply][0].to == m->to) ||
                    (s_killers[ply][1].from == m->from && s_killers[ply][1].to == m->to))) {
            m->score = 18000; // quiet moves that caused a cutoff at this depth before
        } else {
            int h = s_history ? s_history[m->from][m->to] : 0;
            m->score = (int16_t)(h > 17000 ? 17000 : h);
        }
    }
}

// Selection sort one move at a time: with a beta cutoff usually happening in the first few
// moves, fully sorting the list is wasted work.
static void pickMove(Move *list, int n, int i)
{
    int best = i;
    for (int j = i + 1; j < n; j++)
        if (list[j].score > list[best].score)
            best = j;
    if (best != i) {
        Move t = list[i];
        list[i] = list[best];
        list[best] = t;
    }
}

// Quiescence: at the leaves, keep searching captures only. Without this the engine stops
// counting mid-exchange and believes it is a queen up when the recapture is one ply away -
// the single biggest source of nonsense moves in a naive alpha-beta.
static int quiesce(Board *b, int alpha, int beta, int ply)
{
    if (outOfTime())
        return 0;
    // Hard ceiling. A quiescence search in a position full of recaptures can run a long way, and
    // it must never index past the end of the move stack.
    if (ply >= MAX_PLY + 6)
        return eval_position(b);
    s_nodes++;
    int stand = eval_position(b);
    if (stand >= beta)
        return beta;
    if (stand > alpha)
        alpha = stand;

    Move *list = PLY_LIST(ply);
    int n = chess_gen_captures(b, list);
    scoreMoves(b, list, n, 0, NULL);
    Undo u;
    for (int i = 0; i < n; i++) {
        pickMove(list, n, i);
        chess_make(b, &list[i], &u);
        int score = -quiesce(b, -beta, -alpha, ply + 1);
        chess_unmake(b, &u);
        if (s_timeUp)
            return 0;
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }
    return alpha;
}

static int negamax(Board *b, int depth, int alpha, int beta, int ply)
{
    if (outOfTime())
        return 0;
    if (ply > 0 && isRepetition(b))
        return 0;
    if (b->halfmove >= 100)
        return 0;

    const int alphaOrig = alpha;
    TTEntry *e = NULL;
    Move ttMove, *ttMovePtr = NULL;
    if (s_ttCount) {
        e = &s_tt[b->hash & (s_ttCount - 1)];
        if (e->key == b->hash) {
            ttMove.from = e->from;
            ttMove.to = e->to;
            ttMove.promo = e->promo;
            ttMove.flags = 0;
            ttMove.score = 0;
            ttMovePtr = &ttMove;
            if (ply > 0 && e->depth >= depth) {
                // ⛔ MATE SCORES MUST BE RE-BASED ON THE WAY OUT. A mate score means "mate in N
                // plies FROM THIS NODE", so storing it raw makes it wrong everywhere else the
                // same position appears at a different distance from the root - the engine then
                // reports nonsense mate distances and can prefer a slower mate over a faster one.
                int sc = e->score;
                if (sc > MATE - 1000)
                    sc -= ply;
                else if (sc < -MATE + 1000)
                    sc += ply;
                if (e->flag == TT_EXACT)
                    return sc;
                if (e->flag == TT_ALPHA && sc <= alpha)
                    return alpha;
                if (e->flag == TT_BETA && sc >= beta)
                    return beta;
            }
        }
    }

    const bool inCheck = chess_in_check(b, b->side);
    if (inCheck)
        depth++; // never stop searching in the middle of a forcing sequence

    if (depth <= 0)
        return quiesce(b, alpha, beta, ply);

    s_nodes++;
    Move *list = PLY_LIST(ply);
    int n = chess_gen_moves(b, list);
    if (n == 0)
        return inCheck ? -MATE + ply : 0; // mated here, or stalemate

    scoreMoves(b, list, n, ply, ttMovePtr);
    Undo u;
    Move best = list[0];
    int bestScore = -INF;

    for (int i = 0; i < n; i++) {
        pickMove(list, n, i);
        chess_make(b, &list[i], &u);
        search_history_push(b->hash);
        int score;
        if (i == 0) {
            score = -negamax(b, depth - 1, -beta, -alpha, ply + 1);
        } else {
            // Late move reductions: after the first few moves, and with good ordering, the rest
            // are probably bad. Search them shallower and only re-search if one surprises us.
            int reduce = (depth >= 3 && i >= 4 && !(list[i].flags & (MOVE_CAPTURE | MOVE_PROMO)) && !inCheck) ? 1 : 0;
            score = -negamax(b, depth - 1 - reduce, -alpha - 1, -alpha, ply + 1);
            if (score > alpha && score < beta)
                score = -negamax(b, depth - 1, -beta, -alpha, ply + 1);
        }
        search_history_pop();
        chess_unmake(b, &u);
        if (s_timeUp)
            return 0;

        if (score > bestScore) {
            bestScore = score;
            best = list[i];
        }
        if (score > alpha)
            alpha = score;
        if (alpha >= beta) {
            if (!(list[i].flags & MOVE_CAPTURE) && ply < MAX_PLY && s_killers) {
                s_killers[ply][1] = s_killers[ply][0];
                s_killers[ply][0] = list[i];
                if (s_history) {
                    int h = s_history[list[i].from][list[i].to] + depth * depth;
                    s_history[list[i].from][list[i].to] = (int16_t)(h > 30000 ? 30000 : h); // no overflow
                }
            }
            break;
        }
    }

    if (s_ttCount && e && !s_timeUp) {
        // Always replace. Depth-preferred replacement is better in a long search; in one this
        // short the table mostly holds this search's own nodes, and always-replace keeps the
        // most recent - which is what the next iteration of the deepening will ask for.
        e->key = b->hash;
        // The matching half of the re-basing above: store the distance from THIS node.
        int store = bestScore;
        if (store > MATE - 1000)
            store += ply;
        else if (store < -MATE + 1000)
            store -= ply;
        e->score = (int16_t)store;
        e->depth = (uint8_t)(depth > 255 ? 255 : depth);
        e->flag = (uint8_t)(bestScore <= alphaOrig ? TT_ALPHA : (bestScore >= beta ? TT_BETA : TT_EXACT));
        e->from = best.from;
        e->to = best.to;
        e->promo = best.promo;
    }
    return bestScore;
}

// ---- difficulty ----------------------------------------------------------------------------
typedef struct {
    const char *name;
    int depth;
    uint32_t ms;
    int spread; // centipawns: how far below best a move may be and still get picked
} LevelCfg;

static const LevelCfg kLevels[CHESS_LEVELS] = {
    {"Beginner", 2, 300, 150}, // will hang things, like a beginner does
    {"Casual", 3, 700, 70},
    {"Club", 5, 2000, 0},
    {"Strong", 7, 3500, 0},
    {"Expert", 9, 8000, 0},
    {"Max", 30, 15000, 0}, // depth is effectively unlimited; the clock decides
};

const char *chess_level_name(ChessLevel lv)
{
    return (lv >= 0 && lv < CHESS_LEVELS) ? kLevels[lv].name : "?";
}

static uint32_t s_rand = 0x2545F491;
static uint32_t nextRand(void)
{
    s_rand ^= s_rand << 13;
    s_rand ^= s_rand >> 17;
    s_rand ^= s_rand << 5;
    return s_rand;
}

void search_last_info(SearchInfo *info)
{
    if (info)
        *info = s_info;
}

bool search_best_move(Board *b, ChessLevel lv, uint32_t msBudget, Move *out)
{
    if (lv < 0 || lv >= CHESS_LEVELS)
        lv = CHESS_CLUB;
    const LevelCfg *cfg = &kLevels[lv];

    if (!s_moveStack)
        search_init(NULL, 1024UL * 1024UL); // never search without the stack allocated
    if (!s_moveStack)
        return false;
    // The root uses the two slots the recursive search never reaches (it starts at ply 1).
    Move *list = PLY_LIST(0);
    int n = chess_gen_moves(b, list);
    if (n == 0)
        return false;
    *out = list[0];
    if (n == 1) {
        memset(&s_info, 0, sizeof(s_info));
        s_info.depth = 1;
        return true; // no point thinking about a forced move
    }

    s_nodes = 0;
    s_timeUp = false;
    if (s_killers)
        memset(s_killers, 0, (size_t)MAX_PLY * 2 * sizeof(Move));
    if (s_history)
        memset(s_history, 0, 128UL * 128UL * sizeof(int16_t));
    uint32_t start = NOW_MS();
    uint32_t budget = msBudget ? msBudget : cfg->ms;
    s_deadline = start + budget;

    // Root scores are kept so the weak levels can choose a slightly-worse move on purpose.
    // ⛔ WORKED OUT FRESH EVERY CALL - NEVER CACHED IN A STATIC. It points into the move stack,
    // and search_release() frees that stack whenever the chess app is put away. The cached
    // version kept aiming at the FIRST stack ever allocated, so every game after the first wrote
    // up to 512 bytes of scores into PSRAM that had been handed back - to the map's tile cache,
    // a Lua app, whatever took it next. Heap corruption that crashes something else, later.
    int16_t *rootScore = (int16_t *)((char *)PLY_LIST(MAX_PLY + 4)); // a spare slot, reused as scores
    Move *rootMove = PLY_LIST(MAX_PLY + 5); // borrow a reserved slot rather than 1.5KB of .bss
    int rootN = 0;
    int completedDepth = 0, bestScore = 0;

    for (int depth = 1; depth <= cfg->depth; depth++) {
        int alpha = -INF, localBest = -INF;
        Move localBestMove = list[0];
        int16_t *scores = (int16_t *)((char *)PLY_LIST(MAX_PLY + 4) + MAX_MOVES * sizeof(int16_t));
        Move *order = PLY_LIST(MAX_PLY + 7);
        int m = chess_gen_moves(b, order);
        scoreMoves(b, order, m, 0, completedDepth ? &rootMove[0] : NULL);
        Undo u;
        bool finished = true;
        for (int i = 0; i < m; i++) {
            pickMove(order, m, i);
            chess_make(b, &order[i], &u);
            search_history_push(b->hash);
            int sc = -negamax(b, depth - 1, -INF, -alpha, 1);
            search_history_pop();
            chess_unmake(b, &u);
            if (s_timeUp) {
                finished = false;
                break;
            }
            scores[i] = (int16_t)sc;
            if (sc > localBest) {
                localBest = sc;
                localBestMove = order[i];
            }
            if (sc > alpha)
                alpha = sc;
        }
        if (!finished)
            break; // an incomplete iteration is not trustworthy; keep the last complete one
        completedDepth = depth;
        bestScore = localBest;
        rootN = m;
        for (int i = 0; i < m; i++) {
            rootMove[i] = order[i];
            rootScore[i] = scores[i];
        }
        *out = localBestMove;
        // Found a forced mate - no deeper search can improve on that.
        if (localBest > MATE - 100 || localBest < -MATE + 100)
            break;
        // Do not start an iteration there is clearly no time to finish. Each one costs roughly
        // 3-4x the last, so with under a third of the budget left it would only be abandoned.
        if ((int32_t)(NOW_MS() - start) > (int32_t)(budget / 3))
            break;
    }

    // ⭐ THE WEAK LEVELS PICK IMPERFECTLY ON PURPOSE - see the note in search.h. Choosing at
    // random among moves within `spread` of the best produces the inconsistency a human beginner
    // has, instead of a machine that is flawless to depth 2 and blind at depth 3.
    if (cfg->spread > 0 && rootN > 1) {
        Move *pool = PLY_LIST(MAX_PLY + 6);
        int pn = 0;
        for (int i = 0; i < rootN; i++)
            if (rootScore[i] >= bestScore - cfg->spread)
                pool[pn++] = rootMove[i];
        if (pn > 0)
            *out = pool[nextRand() % (uint32_t)pn];
    }

    s_info.depth = completedDepth;
    s_info.score = bestScore;
    s_info.nodes = s_nodes;
    s_info.ms = NOW_MS() - start;
    s_info.mateIn = 0;
    if (bestScore > MATE - 100)
        s_info.mateIn = (MATE - bestScore + 1) / 2;
    else if (bestScore < -MATE + 100)
        s_info.mateIn = -((MATE + bestScore + 1) / 2);
    return true;
}
