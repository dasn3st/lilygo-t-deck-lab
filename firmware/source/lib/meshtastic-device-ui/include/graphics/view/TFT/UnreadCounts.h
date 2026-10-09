#pragma once
#include <cstdint>

// Unread messages, counted PER CONVERSATION.
//
// THE BUG THIS EXISTS FOR. Jake, 2026-08-07: "both the color and the max had 2 new messages in
// the am, but only one was shared. im missing messages." He had not lost anything - the device
// kept ONE unread number for the whole mesh, and opening any single chat set it to zero. Two
// messages in two different conversations therefore read as "2 new"; you opened one, saw one,
// and the counter wiped, leaving the other sitting in its own thread with nothing pointing at
// it. Both his devices "agreed" because both share the flaw, not because the radio dropped
// anything. Upstream knew: the two clear sites each carried
// "// TODO: not all messages may be actually read".
//
// So: a count per conversation, and opening a conversation clears ONLY that one.
//
// The key is a node number for a direct message, or kChannelKey | index for a channel. Node
// numbers are real 32-bit addresses, but channel indexes are 0..7, so the top bit is free to
// tell them apart.

const uint32_t kChannelKey = 0x80000000u;

inline uint32_t unreadKeyForChannel(uint8_t ch) { return kChannelKey | ch; }

void unread_add(uint32_t key);        // a message arrived for this conversation
void unread_clear(uint32_t key);      // ...and you have now opened it
void unread_clear_all(void);          // "mark everything read"
uint32_t unread_get(uint32_t key);    // how many are waiting in this one
uint32_t unread_total(void);          // ...and across all of them, for the top bar
