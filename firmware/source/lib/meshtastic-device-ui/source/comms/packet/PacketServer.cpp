#include "comms/PacketServer.h"
#include "util/SharedQueue.h"
#include "configuration.h" // LOG_INFO
#include <assert.h>
#include <esp_heap_caps.h>

// ⛔ THIS QUEUE IS BOUNDED BY FREE MEMORY, NOT BY A PACKET COUNT.
//
// Every entry is a whole meshtastic_FromRadio on the heap - measured at 768 bytes, ~810 with
// the wrapper and allocator overhead. Upstream's limit of 300 therefore stood for 240KB on a
// device with about 86KB of free internal heap: it could never fire, the heap ran out first,
// and every boot spent its first minute a few hundred bytes from nothing. Traced 2026-09-19:
//     49s   free=2,004  largest=436   queue 145 deep
//     50s   Wi-Fi drops, "Reason 204" - the handshake could not allocate
//
// ⚠️ A FIXED SMALL COUNT IS NOT THE ANSWER EITHER, AND THE FIRST ATTEMPT AT THIS WAS WRONG.
// Capping at 32 held the memory fine, but it applied backpressure even with 80KB free - and
// packets then piled up in Meshtastic's own toPhoneQueue, which does NOT block when it fills.
// It discards. Measured after that change: 9 "ToPhone queue is full, drop packet" in three
// minutes. The earlier commit claimed "backpressure, not dropping"; that claim was false.
//
// The real constraint is memory, so say memory. Below the floor we stop accepting, which is
// the crash this exists to prevent. Above it we behave as upstream always did, so there is
// no backpressure and nothing upstream overflows.
//
// ⛔ AND IT MUST BE THE MEMORY THE QUEUE ACTUALLY USES. Until 2026-09-29 this measured the
// INTERNAL heap, with a 40KB floor - but a settled device sits at 28-40KB internal, so the
// queue held almost permanently: measured holding from 46 seconds after boot, the screen fed
// nothing, and Meshtastic's toPhoneQueue full and discarding ("ToPhone queue is full, drop
// packet") every few seconds. That is missed messages, and the discarded backlog sat in the
// same internal heap, pushing it lower still. The entries now live in PSRAM (util/Packet.h),
// so the internal heap is no longer this queue's business; PSRAM is what it spends.
const uint32_t max_packet_queue_size = 300;             // upstream's ceiling, unchanged
const uint32_t kQueuePsramFloor = 256 * 1024;           // keep this much PSRAM free for everyone else
const uint32_t kQueuePsramResume = 320 * 1024;          // ...and this much before accepting again

SharedQueue *sharedQueue = nullptr;

// Instrumentation, 2026-09-19. Every boot drives the internal heap to a few hundred bytes
// while the node database syncs, and this queue is the prime suspect: it holds up to
// max_packet_queue_size whole meshtastic_FromRadio structures, each one a separate heap
// allocation, and the UI task drains it only as fast as LVGL lets it. Measure before changing
// anything - twice today an inference from a single number turned out to be wrong.
static uint32_t s_queuePeak = 0;
extern "C" uint32_t tdeck_pktq_depth(void) { return sharedQueue ? (uint32_t)sharedQueue->serverQueueSize() : 0u; }
extern "C" uint32_t tdeck_pktq_peak(void) { return s_queuePeak; }
extern "C" uint32_t tdeck_pktq_cap(void) { return max_packet_queue_size; }

PacketServer *packetServer = nullptr;

PacketServer::PacketServer() : queue(nullptr) {}

PacketServer *PacketServer::init(void)
{
    packetServer = new PacketServer;
    sharedQueue = new SharedQueue;
    packetServer->begin(sharedQueue);
    return packetServer;
}

void PacketServer::begin(SharedQueue *_queue)
{
    queue = _queue;
}

Packet::PacketPtr PacketServer::receivePacket(void)
{
    assert(queue);
    if (queue->clientQueueSize() == 0)
        return {nullptr};
    return queue->serverReceive();
}

bool PacketServer::sendPacket(Packet &&p)
{
    assert(queue);
    // The same test available() applies. It is checked there BEFORE the packet is taken off
    // the radio's queue, so reaching this is a race rather than the normal path.
    if (!available()) {
        return false;
    }
    queue->serverSend(std::move(p));
    const uint32_t n = (uint32_t)queue->serverQueueSize();
    if (n > s_queuePeak)
        s_queuePeak = n;
    return true;
}

bool PacketServer::hasData() const
{
    assert(queue);
    return queue->clientQueueSize() > 0;
}

bool PacketServer::available() const
{
    assert(queue);
    if (queue->serverQueueSize() >= max_packet_queue_size)
        return false;
    // Hysteresis, so we do not sit on the floor flapping open and shut once per packet.
    static bool holding = false;
#ifdef BOARD_HAS_PSRAM
    const uint32_t freeNow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    const uint32_t freeNow = kQueuePsramResume; // no PSRAM: the count ceiling is the only limit
#endif
    if (holding) {
        if (freeNow >= kQueuePsramResume) {
            holding = false;
            LOG_INFO("[PacketServer] UI queue flowing again, %u bytes PSRAM free", (unsigned)freeNow);
        }
    } else if (freeNow < kQueuePsramFloor) {
        holding = true;
        LOG_INFO("[PacketServer] holding the UI queue at %u deep, %u bytes PSRAM free",
                 (unsigned)queue->serverQueueSize(), (unsigned)freeNow);
    }
    return !holding;
}
