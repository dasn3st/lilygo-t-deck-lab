// Tests for the search. Perft proved the RULES; this proves the engine plays chess.
//
// ⭐ EVERY CHECK HERE IS SELF-VALIDATING - none of them depend on me knowing the right answer.
// A test that asserts "the best move in this position is Qb7" is only as good as my chess, and
// if I get it wrong the test enshrines the mistake. Instead:
//   * mate finding: play the move the engine returns, then ASK THE RULES whether it is mate.
//   * legality: self-play a whole game and confirm every move came from the legal move list and
//     the game ended in a real terminal state.
//   * strength: play the levels against each other. A difficulty ladder where the higher rung
//     does not actually win more is a menu of lies, and that is checkable without an opinion.
//
//   gcc -O2 -o stest chess.c search.c search_test.c && ./stest

#include "chess.h"
#include "search.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails = 0;

static void check(int cond, const char *what)
{
    printf("  %-52s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond)
        fails++;
}

// ---- 1. can it see a mate, and is what it found really mate? --------------------------------
//
// ⛔ THE FIRST VERSION OF THIS TEST WAS WRONG, AND IN THE EXACT WAY THE HEADER WARNS ABOUT.
// It listed "k7/8/1K6/8/8/8/8/1Q6 w" as a mate in 1, expecting Qb7#. White's own king on b6
// blocks the queen's path up the b-file, so Qb7 is not even a legal move. The engine found a
// mate in TWO and was marked as failing. The move check was self-validating; the PREMISE - that
// a mate in 1 existed - was my assertion, and it was simply wrong.
//
// So the premise is derived too: try every legal move, ask the rules which ones give mate, and
// only then require the engine to agree. Nothing here depends on my chess being any good.
static bool mateInOneExists(Board *b, char *moveOut)
{
    Move list[MAX_MOVES];
    int n = chess_gen_moves(b, list);
    Undo u;
    for (int i = 0; i < n; i++) {
        chess_make(b, &list[i], &u);
        bool mate = (chess_game_over(b) == 1);
        chess_unmake(b, &u);
        if (mate) {
            if (moveOut)
                chess_move_str(&list[i], moveOut);
            return true;
        }
    }
    return false;
}

static void testMates(void)
{
    static const char *kPositions[] = {
        "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1",      // back rank
        "6k1/5p1p/6p1/8/8/8/5PPP/R5K1 w - - 0 1", // luft: no mate in 1
        "7k/6pp/8/8/8/8/5PPP/R5RK w - - 0 1",     // two rooks
        "k7/8/1K6/8/8/8/8/1Q6 w - - 0 1",         // the one I got wrong: mate in 2, not 1
        "rnbqkbnr/pppp1ppp/8/4p3/6P1/5P2/PPPPP2P/RNBQKBNR b KQkq - 0 2", // fool's mate is on
    };
    for (unsigned i = 0; i < sizeof(kPositions) / sizeof(kPositions[0]); i++) {
        Board b;
        if (!chess_set_fen(&b, kPositions[i])) {
            printf("  bad FEN: %s\n", kPositions[i]);
            fails++;
            continue;
        }
        char proof[6] = {0};
        bool exists = mateInOneExists(&b, proof);

        search_history_clear();
        Move m;
        if (!search_best_move(&b, CHESS_CLUB, 1500, &m)) {
            check(0, "search returned no move");
            continue;
        }
        Undo u;
        chess_make(&b, &m, &u);
        bool played = (chess_game_over(&b) == 1);
        chess_unmake(&b, &u);

        char got[6];
        chess_move_str(&m, got);
        char label[110];
        if (exists)
            snprintf(label, sizeof(label), "mate in 1 exists (%s); engine played %s", proof, got);
        else
            snprintf(label, sizeof(label), "no mate in 1 here; engine played %s", got);
        // If a mate in 1 is available the engine must take it. If none exists it must not
        // somehow produce one - which would mean the rules and the search disagree.
        check(exists == played, label);
    }
}

// ---- 2. a whole game, every move legal, ending in a real result ------------------------------
static int playGame(ChessLevel white, ChessLevel black, uint32_t ms, int *plies, int verbose)
{
    Board b;
    chess_init(&b);
    search_history_clear();
    search_history_push(b.hash);
    int n = 0;
    for (; n < 300; n++) {
        int over = chess_game_over(&b);
        if (over) {
            *plies = n;
            // 1 = side to move is mated, so the WINNER is the other side.
            if (over == 1)
                return b.side == WHITE ? -1 : 1;
            return 0;
        }
        Move m;
        if (!search_best_move(&b, b.side == WHITE ? white : black, ms, &m)) {
            *plies = n;
            return 0;
        }
        // Confirm the engine's own move is in the legal list - if the search ever returns a
        // move the rules do not allow, everything else it says is worthless.
        Move legal[MAX_MOVES];
        int ln = chess_gen_moves(&b, legal);
        int found = 0;
        for (int i = 0; i < ln; i++)
            if (legal[i].from == m.from && legal[i].to == m.to && legal[i].promo == m.promo)
                found = 1;
        if (!found) {
            char buf[6];
            chess_move_str(&m, buf);
            printf("  ILLEGAL MOVE RETURNED: %s\n", buf);
            fails++;
            *plies = n;
            return 0;
        }
        Undo u;
        chess_make(&b, &m, &u);
        search_history_push(b.hash);
        if (verbose) {
            char buf[6];
            chess_move_str(&m, buf);
            printf("%s ", buf);
        }
    }
    *plies = n;
    return 0; // hit the move cap: a draw for our purposes
}

int main(void)
{
    search_init(NULL, 4u * 1024 * 1024); // 4MB table on the PC

    printf("1. mate finding (self-validated against the rules)\n");
    testMates();

    printf("\n2. speed\n");
    {
        Board b;
        chess_init(&b);
        search_history_clear();
        Move m;
        clock_t t0 = clock();
        search_best_move(&b, CHESS_MAX, 3000, &m);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        SearchInfo in;
        search_last_info(&in);
        char buf[6];
        chess_move_str(&m, buf);
        printf("  opening position, 3s: depth %d, %u nodes, %.0f knps, plays %s (%+.2f)\n", in.depth, in.nodes,
               secs > 0 ? in.nodes / secs / 1000.0 : 0.0, buf, in.score / 100.0);
        check(in.depth >= 6, "reaches at least depth 6 in 3s on the PC");
        // ⚠️ Assert it actually SEARCHED. Without this, a search that returns instantly reports
        // its maximum depth with zero nodes and sails past a depth check - which is exactly how
        // the isRepetition off-by-one first showed up.
        check(in.nodes > 10000, "actually searched (nodes > 10k)");
    }

    printf("\n3. a full self-played game is legal start to finish\n");
    {
        int plies = 0;
        playGame(CHESS_CASUAL, CHESS_CASUAL, 120, &plies, 0);
        printf("  game ran %d plies with no illegal move\n", plies);
        check(plies > 10, "game lasted more than 10 plies");
    }

    printf("\n4. does the ladder actually climb? (Club vs Beginner, 6 games)\n");
    {
        int clubWins = 0, begWins = 0, draws = 0;
        for (int g = 0; g < 6; g++) {
            int plies = 0;
            // Alternate colours so the result is not just White's first-move advantage.
            int r = (g & 1) ? playGame(CHESS_CLUB, CHESS_BEGINNER, 200, &plies, 0)
                            : -playGame(CHESS_BEGINNER, CHESS_CLUB, 200, &plies, 0);
            if (r > 0) clubWins++;
            else if (r < 0) begWins++;
            else draws++;
        }
        printf("  Club %d - Beginner %d - drawn %d\n", clubWins, begWins, draws);
        check(clubWins > begWins, "Club scores better than Beginner");
    }

    printf("\n%s\n", fails ? "SEARCH TESTS FAILED" : "SEARCH TESTS ALL PASS");
    return fails ? 1 : 0;
}
