// See UnreadCounts.h for the bug this fixes.
//
// A fixed table rather than a std::map: this is touched from the message path, it never needs
// to be iterated in order, and twenty-four conversations with something unread in them at once
// is already far more than anyone has. A fixed array also means no allocation on the path a
// message arrives on, which on this device matters.
#include "graphics/view/TFT/UnreadCounts.h"

namespace
{
const int kMax = 24;

struct Slot {
    uint32_t key;
    uint16_t count;
    uint32_t seq; // for evicting the least recently touched when the table is full
};

Slot slots[kMax];
uint32_t nextSeq = 1;

int find(uint32_t key)
{
    for (int i = 0; i < kMax; i++)
        if (slots[i].count && slots[i].key == key)
            return i;
    return -1;
}
} // namespace

void unread_add(uint32_t key)
{
    const int at = find(key);
    if (at >= 0) {
        if (slots[at].count < 0xffff)
            slots[at].count++;
        slots[at].seq = nextSeq++;
        return;
    }
    // A free slot, or failing that the one touched longest ago. Dropping the stalest count is
    // better than dropping the newest message's, which is the one you are about to look for.
    int use = -1;
    for (int i = 0; i < kMax; i++) {
        if (!slots[i].count) {
            use = i;
            break;
        }
        if (use < 0 || slots[i].seq < slots[use].seq)
            use = i;
    }
    slots[use].key = key;
    slots[use].count = 1;
    slots[use].seq = nextSeq++;
}

void unread_clear(uint32_t key)
{
    const int at = find(key);
    if (at >= 0)
        slots[at].count = 0;
}

void unread_clear_all(void)
{
    for (int i = 0; i < kMax; i++)
        slots[i].count = 0;
}

uint32_t unread_get(uint32_t key)
{
    const int at = find(key);
    return at >= 0 ? slots[at].count : 0;
}

uint32_t unread_total(void)
{
    uint32_t n = 0;
    for (int i = 0; i < kMax; i++)
        n += slots[i].count;
    return n;
}
