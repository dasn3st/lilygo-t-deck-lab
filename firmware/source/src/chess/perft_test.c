// Perft: walk every legal move to depth N and count the leaves. The counts below are the
// published, universally agreed values for these positions. If the engine matches all of them,
// its move generation is correct - including the awkward corners that quietly ruin a game
// twenty moves later: en passant, en-passant discovered check, castling through check, castling
// rights lost when a rook is CAPTURED on its home square, and all four promotions.
//
// This compiles and runs on the PC. Finding a movegen bug here takes seconds; finding the same
// bug on the device by losing a game to it takes an evening and you still would not know which
// rule was wrong.
//
//   gcc -O2 -o perft chess.c perft_test.c && ./perft

#include "chess.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

struct Case {
    const char *name;
    const char *fen;
    int maxDepth;
    unsigned long long expect[7]; // index = depth
};

static struct Case kCases[] = {
    {"initial", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5,
     {1, 20, 400, 8902, 197281, 4865609, 0}},
    // Kiwipete - the standard second test. Dense with castling, pins and captures.
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4,
     {1, 48, 2039, 97862, 4085603, 0, 0}},
    // Built to catch en-passant bugs, including the discovered check along the 5th rank.
    {"position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, {1, 14, 191, 2812, 43238, 674624, 0}},
    // Promotions galore, and castling rights that must survive exactly the right things.
    {"position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4,
     {1, 6, 264, 9467, 422333, 0, 0}},
    {"position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, {1, 44, 1486, 62379, 2103487, 0, 0}},
    {"position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4,
     {1, 46, 2079, 89890, 3894594, 0, 0}},
};

int main(void)
{
    int fails = 0;
    unsigned long long totalNodes = 0;
    clock_t t0 = clock();

    for (unsigned c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
        struct Case *k = &kCases[c];
        Board b;
        if (!chess_set_fen(&b, k->fen)) {
            printf("  %-12s FEN REJECTED: %s\n", k->name, k->fen);
            fails++;
            continue;
        }
        // Round-trip the FEN as a free extra check: if writing it back does not reproduce the
        // input, either the parser or the writer is wrong and every later result is suspect.
        char back[128];
        chess_get_fen(&b, back, sizeof(back));
        if (strcmp(back, k->fen))
            printf("  %-12s fen round-trip differs:\n      in  %s\n      out %s\n", k->name, k->fen, back);

        for (int d = 1; d <= k->maxDepth; d++) {
            unsigned long long got = chess_perft(&b, d);
            unsigned long long want = k->expect[d];
            totalNodes += got;
            if (got != want) {
                printf("  %-12s depth %d  got %llu  WANT %llu   <-- MISMATCH\n", k->name, d, got, want);
                fails++;
            } else {
                printf("  %-12s depth %d  %llu ok\n", k->name, d, got);
            }
        }
    }

    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n%llu nodes in %.2fs (%.0f knps)\n", totalNodes, secs, secs > 0 ? totalNodes / secs / 1000.0 : 0.0);
    printf("%s\n", fails ? "PERFT FAILED" : "PERFT ALL PASS - move generation is correct");
    return fails ? 1 : 0;
}
