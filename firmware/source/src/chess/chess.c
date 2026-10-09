#include "chess.h"
#include <stdio.h>
#include <stdlib.h> // atoi, used by the FEN parser
#include <string.h>

// ---------------------------------------------------------------------------------------
// 0x88 board. sq = rank*16 + file, so a1=0, h1=7, a8=112, h8=119, and a square is off the
// board exactly when (sq & 0x88) is non-zero. Every sliding-move bounds check is that one AND.
// ---------------------------------------------------------------------------------------

#define ON_BOARD(sq) (!((sq) & 0x88))
#define RANK(sq) ((sq) >> 4)
#define FILE_OF(sq) ((sq) & 7)

static const int kKnightOff[8] = {33, 31, 18, 14, -33, -31, -18, -14};
static const int kKingOff[8] = {16, -16, 1, -1, 17, 15, -17, -15};
static const int kBishopOff[4] = {17, 15, -17, -15};
static const int kRookOff[4] = {16, -16, 1, -1};

// ---- Zobrist ---------------------------------------------------------------------------
// Fixed seed, so a position always hashes the same across runs and across the PC and device.
// That matters: the opening book and any saved game are keyed by this.
// ⛔ POINTERS, NOT ARRAYS. As plain statics these are 17KB of internal RAM that the device
// cannot spare - see the header note on chess_set_alloc. Allocated once on first use.
static uint64_t (*zPiece)[128]; // [16][128]
static uint64_t *zEp;           // [128]
static uint64_t zSide;
static uint64_t zCastle[16]; // 128 bytes; small enough to stay put
static bool zInit = false;
static void *(*s_alloc)(unsigned long) = NULL;

void chess_set_alloc(void *(*fn)(unsigned long bytes))
{
    s_alloc = fn;
}

static uint64_t rng64(uint64_t *s)
{
    // xorshift64*. Small, fast, and good enough for hashing - this is not cryptography.
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

static void zobristInit(void)
{
    if (zInit)
        return;
    if (!zPiece) {
        zPiece = (uint64_t(*)[128])(s_alloc ? s_alloc(16UL * 128UL * sizeof(uint64_t))
                                            : calloc(16UL * 128UL, sizeof(uint64_t)));
        zEp = (uint64_t *)(s_alloc ? s_alloc(128UL * sizeof(uint64_t)) : calloc(128UL, sizeof(uint64_t)));
    }
    if (!zPiece || !zEp)
        return; // leaves zInit false; chess_set_fen reports the failure rather than corrupting
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int p = 0; p < 16; p++)
        for (int q = 0; q < 128; q++)
            zPiece[p][q] = rng64(&s);
    zSide = rng64(&s);
    for (int i = 0; i < 16; i++)
        zCastle[i] = rng64(&s);
    for (int i = 0; i < 128; i++)
        zEp[i] = rng64(&s);
    zInit = true;
}

static uint64_t hashBoard(const Board *b)
{
    uint64_t h = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (!ON_BOARD(sq))
            continue;
        if (b->board[sq])
            h ^= zPiece[b->board[sq] & 15][sq];
    }
    if (b->side == BLACK)
        h ^= zSide;
    h ^= zCastle[b->castling & 15];
    if (b->ep != NO_EP)
        h ^= zEp[b->ep];
    return h;
}

// ---- setup -------------------------------------------------------------------------------

bool chess_set_fen(Board *b, const char *fen)
{
    zobristInit();
    if (!zInit)
        return false; // could not allocate the hash tables; better to say so than to play wrongly
    memset(b, 0, sizeof(*b));
    b->ep = NO_EP;
    b->king[WHITE] = b->king[BLACK] = 0xFF;

    int rank = 7, file = 0;
    const char *p = fen;
    for (; *p && *p != ' '; p++) {
        if (*p == '/') {
            if (file != 8)
                return false;
            rank--;
            file = 0;
            if (rank < 0)
                return false;
            continue;
        }
        if (*p >= '1' && *p <= '8') {
            file += *p - '0';
            if (file > 8)
                return false;
            continue;
        }
        if (file > 7 || rank < 0)
            return false;
        uint8_t colour = (*p >= 'a' && *p <= 'z') ? BLACK : WHITE;
        char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        uint8_t type;
        switch (c) {
        case 'P': type = PAWN; break;
        case 'N': type = KNIGHT; break;
        case 'B': type = BISHOP; break;
        case 'R': type = ROOK; break;
        case 'Q': type = QUEEN; break;
        case 'K': type = KING; break;
        default: return false;
        }
        uint8_t sq = (uint8_t)(rank * 16 + file);
        b->board[sq] = PIECE(colour, type);
        if (type == KING)
            b->king[colour] = sq;
        file++;
    }
    if (rank != 0 || file != 8)
        return false;

    while (*p == ' ')
        p++;
    b->side = (*p == 'b') ? BLACK : WHITE;
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;

    b->castling = 0;
    if (*p == '-') {
        p++;
    } else {
        for (; *p && *p != ' '; p++) {
            if (*p == 'K') b->castling |= CASTLE_WK;
            else if (*p == 'Q') b->castling |= CASTLE_WQ;
            else if (*p == 'k') b->castling |= CASTLE_BK;
            else if (*p == 'q') b->castling |= CASTLE_BQ;
        }
    }
    while (*p == ' ')
        p++;
    if (*p && *p != '-' && p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8')
        b->ep = (uint8_t)((p[1] - '1') * 16 + (p[0] - 'a'));
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    b->halfmove = (uint8_t)atoi(p);
    while (*p && *p != ' ')
        p++;
    while (*p == ' ')
        p++;
    b->fullmove = (uint16_t)(*p ? atoi(p) : 1);
    if (b->fullmove == 0)
        b->fullmove = 1;

    if (b->king[WHITE] == 0xFF || b->king[BLACK] == 0xFF)
        return false; // a position with no king is not one this engine can reason about
    b->hash = hashBoard(b);
    return true;
}

void chess_init(Board *b)
{
    chess_set_fen(b, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

void chess_get_fen(const Board *b, char *buf, int cap)
{
    static const char *kSyms = ".PNBRQK";
    int n = 0;
    for (int rank = 7; rank >= 0 && n < cap - 1; rank--) {
        int run = 0;
        for (int file = 0; file < 8; file++) {
            uint8_t pc = b->board[rank * 16 + file];
            if (!pc) {
                run++;
                continue;
            }
            if (run) {
                n += snprintf(buf + n, cap - n, "%d", run);
                run = 0;
            }
            char c = kSyms[PTYPE(pc)];
            if (PCOLOUR(pc) == BLACK)
                c = (char)(c + 32);
            n += snprintf(buf + n, cap - n, "%c", c);
        }
        if (run)
            n += snprintf(buf + n, cap - n, "%d", run);
        if (rank)
            n += snprintf(buf + n, cap - n, "/");
    }
    n += snprintf(buf + n, cap - n, " %c ", b->side == WHITE ? 'w' : 'b');
    if (!b->castling)
        n += snprintf(buf + n, cap - n, "-");
    else {
        if (b->castling & CASTLE_WK) n += snprintf(buf + n, cap - n, "K");
        if (b->castling & CASTLE_WQ) n += snprintf(buf + n, cap - n, "Q");
        if (b->castling & CASTLE_BK) n += snprintf(buf + n, cap - n, "k");
        if (b->castling & CASTLE_BQ) n += snprintf(buf + n, cap - n, "q");
    }
    if (b->ep == NO_EP)
        n += snprintf(buf + n, cap - n, " -");
    else
        n += snprintf(buf + n, cap - n, " %c%c", 'a' + FILE_OF(b->ep), '1' + RANK(b->ep));
    snprintf(buf + n, cap - n, " %d %d", b->halfmove, b->fullmove);
}

// ---- attacks -------------------------------------------------------------------------------

bool chess_attacked(const Board *b, uint8_t sq, uint8_t bySide)
{
    // Pawns. Looking BACKWARDS from the target: a white pawn attacks up-left and up-right, so a
    // square is attacked by a white pawn if there is one down-left or down-right of it.
    if (bySide == WHITE) {
        int s1 = sq - 17, s2 = sq - 15;
        if (ON_BOARD(s1) && b->board[s1] == PIECE(WHITE, PAWN))
            return true;
        if (ON_BOARD(s2) && b->board[s2] == PIECE(WHITE, PAWN))
            return true;
    } else {
        int s1 = sq + 17, s2 = sq + 15;
        if (ON_BOARD(s1) && b->board[s1] == PIECE(BLACK, PAWN))
            return true;
        if (ON_BOARD(s2) && b->board[s2] == PIECE(BLACK, PAWN))
            return true;
    }
    for (int i = 0; i < 8; i++) {
        int s = sq + kKnightOff[i];
        if (ON_BOARD(s) && b->board[s] == PIECE(bySide, KNIGHT))
            return true;
    }
    for (int i = 0; i < 8; i++) {
        int s = sq + kKingOff[i];
        if (ON_BOARD(s) && b->board[s] == PIECE(bySide, KING))
            return true;
    }
    for (int i = 0; i < 4; i++) {
        for (int s = sq + kBishopOff[i]; ON_BOARD(s); s += kBishopOff[i]) {
            uint8_t pc = b->board[s];
            if (!pc)
                continue;
            if (PCOLOUR(pc) == bySide && (PTYPE(pc) == BISHOP || PTYPE(pc) == QUEEN))
                return true;
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int s = sq + kRookOff[i]; ON_BOARD(s); s += kRookOff[i]) {
            uint8_t pc = b->board[s];
            if (!pc)
                continue;
            if (PCOLOUR(pc) == bySide && (PTYPE(pc) == ROOK || PTYPE(pc) == QUEEN))
                return true;
            break;
        }
    }
    return false;
}

bool chess_in_check(const Board *b, uint8_t colour)
{
    return chess_attacked(b, b->king[colour], (uint8_t)(colour ^ 1));
}

// ---- move generation -------------------------------------------------------------------------

static void addMove(Move *list, int *n, uint8_t from, uint8_t to, uint8_t promo, uint8_t flags)
{
    if (*n >= MAX_MOVES)
        return;
    Move *m = &list[(*n)++];
    m->from = from;
    m->to = to;
    m->promo = promo;
    m->flags = flags;
    m->score = 0;
}

static void addPawnMove(Move *list, int *n, uint8_t from, uint8_t to, uint8_t flags, uint8_t side)
{
    const int last = (side == WHITE) ? 7 : 0;
    if (RANK(to) == last) {
        // All four promotions. Underpromotion to a knight genuinely matters (it forks, and it
        // mates), and rook/bishop cost nothing to generate and occasionally avoid a stalemate.
        addMove(list, n, from, to, QUEEN, (uint8_t)(flags | MOVE_PROMO));
        addMove(list, n, from, to, ROOK, (uint8_t)(flags | MOVE_PROMO));
        addMove(list, n, from, to, BISHOP, (uint8_t)(flags | MOVE_PROMO));
        addMove(list, n, from, to, KNIGHT, (uint8_t)(flags | MOVE_PROMO));
    } else {
        addMove(list, n, from, to, 0, flags);
    }
}

// Pseudo-legal: may leave the king in check. chess_gen_moves filters those out.
static int genPseudo(const Board *b, Move *list, bool capturesOnly)
{
    int n = 0;
    const uint8_t us = b->side, them = (uint8_t)(us ^ 1);
    const int fwd = (us == WHITE) ? 16 : -16;
    const int startRank = (us == WHITE) ? 1 : 6;

    for (int sq = 0; sq < 128; sq++) {
        if (!ON_BOARD(sq))
            continue;
        uint8_t pc = b->board[sq];
        if (!pc || PCOLOUR(pc) != us)
            continue;
        uint8_t type = PTYPE(pc);

        if (type == PAWN) {
            int one = sq + fwd;
            if (ON_BOARD(one) && !b->board[one]) {
                if (!capturesOnly || RANK(one) == (us == WHITE ? 7 : 0))
                    addPawnMove(list, &n, (uint8_t)sq, (uint8_t)one, 0, us);
                int two = sq + 2 * fwd;
                if (!capturesOnly && RANK(sq) == startRank && ON_BOARD(two) && !b->board[two])
                    addMove(list, &n, (uint8_t)sq, (uint8_t)two, 0, MOVE_DOUBLEPUSH);
            }
            for (int d = -1; d <= 1; d += 2) {
                int cap = sq + fwd + d;
                if (!ON_BOARD(cap))
                    continue;
                // ⚠️ The +d must not wrap a file. 0x88 catches a wrap off the a/h edge because
                // the neighbouring "file" lands in the invalid half of the row - which is the
                // whole reason this representation is worth using.
                uint8_t t = b->board[cap];
                if (t && PCOLOUR(t) == them)
                    addPawnMove(list, &n, (uint8_t)sq, (uint8_t)cap, MOVE_CAPTURE, us);
                else if (!t && b->ep != NO_EP && cap == b->ep)
                    addMove(list, &n, (uint8_t)sq, (uint8_t)cap, 0, MOVE_CAPTURE | MOVE_ENPASSANT);
            }
            continue;
        }

        if (type == KNIGHT || type == KING) {
            const int *off = (type == KNIGHT) ? kKnightOff : kKingOff;
            for (int i = 0; i < 8; i++) {
                int to = sq + off[i];
                if (!ON_BOARD(to))
                    continue;
                uint8_t t = b->board[to];
                if (t && PCOLOUR(t) == us)
                    continue;
                if (capturesOnly && !t)
                    continue;
                addMove(list, &n, (uint8_t)sq, (uint8_t)to, 0, (uint8_t)(t ? MOVE_CAPTURE : 0));
            }
            continue;
        }

        // Sliders.
        const int *off;
        int dirs;
        if (type == BISHOP) { off = kBishopOff; dirs = 4; }
        else if (type == ROOK) { off = kRookOff; dirs = 4; }
        else { off = kKingOff; dirs = 8; } // queen: the king's 8 directions, but sliding
        for (int i = 0; i < dirs; i++) {
            for (int to = sq + off[i]; ON_BOARD(to); to += off[i]) {
                uint8_t t = b->board[to];
                if (t && PCOLOUR(t) == us)
                    break;
                if (!capturesOnly || t)
                    addMove(list, &n, (uint8_t)sq, (uint8_t)to, 0, (uint8_t)(t ? MOVE_CAPTURE : 0));
                if (t)
                    break;
            }
        }
    }

    // Castling. Rights alone are not enough: the squares between must be empty, and the king
    // must not be in check, pass THROUGH check, or land in check. Missing the "pass through"
    // rule is the classic perft-breaking bug, and it is checked here rather than left to the
    // legality filter because the filter only tests the final position.
    if (!capturesOnly) {
        const uint8_t e = (us == WHITE) ? 4 : 116; // e1 / e8
        if (b->king[us] == e && !chess_attacked(b, e, them)) {
            const uint8_t kRight = (us == WHITE) ? CASTLE_WK : CASTLE_BK;
            const uint8_t kLeft = (us == WHITE) ? CASTLE_WQ : CASTLE_BQ;
            if ((b->castling & kRight) && !b->board[e + 1] && !b->board[e + 2] &&
                b->board[e + 3] == PIECE(us, ROOK) && !chess_attacked(b, (uint8_t)(e + 1), them))
                addMove(list, &n, e, (uint8_t)(e + 2), 0, MOVE_CASTLE);
            if ((b->castling & kLeft) && !b->board[e - 1] && !b->board[e - 2] && !b->board[e - 3] &&
                b->board[e - 4] == PIECE(us, ROOK) && !chess_attacked(b, (uint8_t)(e - 1), them))
                addMove(list, &n, e, (uint8_t)(e - 2), 0, MOVE_CASTLE);
        }
    }
    return n;
}

// Castling rights are cleared when a king or rook MOVES, and also when a rook is CAPTURED on
// its home square - forgetting the second is another classic perft mismatch, because the rights
// then survive a rook that no longer exists.
static const uint8_t kCastleMask[128] = {
    // a1=0 clears WQ, e1=4 clears both white, h1=7 clears WK; same on rank 8.
    [0] = (uint8_t)~CASTLE_WQ,
    [4] = (uint8_t)~(CASTLE_WQ | CASTLE_WK),
    [7] = (uint8_t)~CASTLE_WK,
    [112] = (uint8_t)~CASTLE_BQ,
    [116] = (uint8_t)~(CASTLE_BQ | CASTLE_BK),
    [119] = (uint8_t)~CASTLE_BK,
};

static inline uint8_t castleMaskFor(uint8_t sq)
{
    uint8_t m = kCastleMask[sq];
    return m ? m : 0xFF; // squares not in the table clear nothing
}

void chess_make(Board *b, const Move *m, Undo *u)
{
    u->move = *m;
    u->castling = b->castling;
    u->ep = b->ep;
    u->halfmove = b->halfmove;
    u->hash = b->hash;
    u->captured = EMPTY;

    const uint8_t us = b->side, them = (uint8_t)(us ^ 1);
    uint8_t pc = b->board[m->from];

    b->hash ^= zCastle[b->castling & 15];
    if (b->ep != NO_EP)
        b->hash ^= zEp[b->ep];

    if (m->flags & MOVE_ENPASSANT) {
        // The captured pawn is NOT on the destination square - it is beside the moving pawn.
        uint8_t capSq = (uint8_t)(us == WHITE ? m->to - 16 : m->to + 16);
        u->captured = b->board[capSq];
        b->hash ^= zPiece[u->captured & 15][capSq];
        b->board[capSq] = EMPTY;
    } else if (b->board[m->to]) {
        u->captured = b->board[m->to];
        b->hash ^= zPiece[u->captured & 15][m->to];
    }

    b->hash ^= zPiece[pc & 15][m->from];
    b->board[m->from] = EMPTY;
    uint8_t placed = (m->flags & MOVE_PROMO) ? PIECE(us, m->promo) : pc;
    b->board[m->to] = placed;
    b->hash ^= zPiece[placed & 15][m->to];

    if (PTYPE(pc) == KING) {
        b->king[us] = m->to;
        if (m->flags & MOVE_CASTLE) {
            // Move the rook too. Kingside: h-rook to f. Queenside: a-rook to d.
            uint8_t rFrom, rTo;
            if (FILE_OF(m->to) == 6) { rFrom = (uint8_t)(m->to + 1); rTo = (uint8_t)(m->to - 1); }
            else { rFrom = (uint8_t)(m->to - 2); rTo = (uint8_t)(m->to + 1); }
            uint8_t rook = b->board[rFrom];
            b->board[rFrom] = EMPTY;
            b->board[rTo] = rook;
            b->hash ^= zPiece[rook & 15][rFrom] ^ zPiece[rook & 15][rTo];
        }
    }

    b->castling &= castleMaskFor(m->from);
    b->castling &= castleMaskFor(m->to); // a rook captured on its home square loses the right
    b->hash ^= zCastle[b->castling & 15];

    b->ep = NO_EP;
    if (m->flags & MOVE_DOUBLEPUSH) {
        b->ep = (uint8_t)((m->from + m->to) / 2);
        b->hash ^= zEp[b->ep];
    }

    if (PTYPE(pc) == PAWN || u->captured)
        b->halfmove = 0;
    else if (b->halfmove < 255)
        b->halfmove++;

    if (us == BLACK)
        b->fullmove++;
    b->side = them;
    b->hash ^= zSide;
}

void chess_unmake(Board *b, const Undo *u)
{
    const Move *m = &u->move;
    const uint8_t us = (uint8_t)(b->side ^ 1);

    if (us == BLACK && b->fullmove)
        b->fullmove--;
    b->side = us;
    b->castling = u->castling;
    b->ep = u->ep;
    b->halfmove = u->halfmove;
    b->hash = u->hash;

    uint8_t moved = b->board[m->to];
    if (m->flags & MOVE_PROMO)
        moved = PIECE(us, PAWN);
    b->board[m->from] = moved;
    b->board[m->to] = EMPTY;

    if (PTYPE(moved) == KING) {
        b->king[us] = m->from;
        if (m->flags & MOVE_CASTLE) {
            uint8_t rFrom, rTo;
            if (FILE_OF(m->to) == 6) { rFrom = (uint8_t)(m->to + 1); rTo = (uint8_t)(m->to - 1); }
            else { rFrom = (uint8_t)(m->to - 2); rTo = (uint8_t)(m->to + 1); }
            b->board[rFrom] = b->board[rTo];
            b->board[rTo] = EMPTY;
        }
    }

    if (u->captured) {
        if (m->flags & MOVE_ENPASSANT)
            b->board[us == WHITE ? m->to - 16 : m->to + 16] = u->captured;
        else
            b->board[m->to] = u->captured;
    }
}

// ⛔ GENERATES INTO THE CALLER'S LIST AND FILTERS IN PLACE - no scratch array.
// The obvious version keeps a local Move pseudo[MAX_MOVES] and copies the legal ones out. That
// is 1536 bytes of stack per ply, and with the search's own list it made a deep search cost 66KB
// of stack on a device whose biggest task stack is 16KB. Compacting in place is safe because the
// write index n can never overtake the read index i: n only advances when i does.
static int genLegal(Board *b, Move *list, bool capturesOnly)
{
    int np = genPseudo(b, list, capturesOnly);
    int n = 0;
    Undo u;
    for (int i = 0; i < np; i++) {
        const Move m = list[i]; // by value: the slot may be overwritten below
        chess_make(b, &m, &u);
        // After make, side has flipped - so "did WE leave our king attacked" is a question
        // about the side that just moved, i.e. b->side ^ 1.
        const bool legal = !chess_in_check(b, (uint8_t)(b->side ^ 1));
        chess_unmake(b, &u);
        if (legal)
            list[n++] = m;
    }
    return n;
}

int chess_gen_moves(Board *b, Move *list)
{
    return genLegal(b, list, false);
}

int chess_gen_captures(Board *b, Move *list)
{
    return genLegal(b, list, true);
}

uint64_t chess_perft(Board *b, int depth)
{
    if (depth == 0)
        return 1;
    Move list[MAX_MOVES];
    int n = chess_gen_moves(b, list);
    if (depth == 1)
        return (uint64_t)n; // the leaf count IS the legal move count; no need to make them
    uint64_t total = 0;
    Undo u;
    for (int i = 0; i < n; i++) {
        chess_make(b, &list[i], &u);
        total += chess_perft(b, depth - 1);
        chess_unmake(b, &u);
    }
    return total;
}

void chess_move_str(const Move *m, char *buf)
{
    static const char *kPromo = ".pnbrqk";
    buf[0] = (char)('a' + FILE_OF(m->from));
    buf[1] = (char)('1' + RANK(m->from));
    buf[2] = (char)('a' + FILE_OF(m->to));
    buf[3] = (char)('1' + RANK(m->to));
    if (m->flags & MOVE_PROMO) {
        buf[4] = kPromo[m->promo];
        buf[5] = 0;
    } else {
        buf[4] = 0;
    }
}

bool chess_parse_move(Board *b, const char *str, Move *out)
{
    if (!str || strlen(str) < 4)
        return false;
    Move list[MAX_MOVES];
    int n = chess_gen_moves(b, list);
    char buf[6];
    for (int i = 0; i < n; i++) {
        chess_move_str(&list[i], buf);
        if (!strncmp(buf, str, strlen(buf)) && strlen(buf) == strlen(str)) {
            *out = list[i];
            return true;
        }
    }
    return false;
}

int chess_game_over(Board *b)
{
    Move list[MAX_MOVES];
    int n = chess_gen_moves(b, list);
    if (n == 0)
        return chess_in_check(b, b->side) ? 1 : 2;
    if (b->halfmove >= 100)
        return 3;
    // Insufficient material: king vs king, or king and a single minor against a bare king.
    // Deliberately conservative - it does not try to judge the harder drawn endings, because
    // claiming a draw that is not one is far worse than playing on in a position you cannot win.
    int minors = 0, others = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (!ON_BOARD(sq) || !b->board[sq])
            continue;
        uint8_t t = PTYPE(b->board[sq]);
        if (t == KING)
            continue;
        if (t == KNIGHT || t == BISHOP)
            minors++;
        else
            others++;
    }
    if (others == 0 && minors <= 1)
        return 4;
    return 0;
}
