#pragma once

#include "util/PsramAlloc.h"
#include <memory>

/**
 * Polymorphic packets that can be moved into and out of packet queues.
 *
 * ⛔ BOTH THE WRAPPER AND THE PAYLOAD LIVE IN PSRAM (when the board has it). The radio-to-UI
 * queue holds a whole meshtastic_FromRadio per entry - 768 bytes - and ESP-IDF's malloc puts
 * anything that size in the scarce internal heap. That made the queue compete with Wi-Fi,
 * TLS and every task stack, so it had to be throttled by internal free memory - and the
 * throttle then stopped feeding the screen whenever the device sat below it, which after boot
 * was nearly always (measured 2026-09-29: held from 46s after boot onwards, messages dropped).
 * In PSRAM the queue costs the internal heap nothing, so it never has to hold back.
 */
class Packet
{
  public:
    using PacketPtr = std::unique_ptr<Packet>;

    Packet(int packetId) : id(packetId) {}

    // virtual move constructor
    virtual PacketPtr move() { return PacketPtr(new Packet(std::move(*this))); }

    // Disable copying
    Packet(const Packet &) = delete;
    Packet &operator=(const Packet &) = delete;

    virtual ~Packet() {}

    int getPacketId() const { return id; }

    TUI_PSRAM_NEW_DELETE

  protected:
    // Enable moving
    Packet(Packet &&) = default;
    Packet &operator=(Packet &&) = default;

  private:
    int id;
};

/**
 * generic packet type class
 */
template <typename PacketType> class DataPacket : public Packet
{
    // Destroys and frees a payload that was placement-constructed in tui_psram_malloc() memory.
    struct PayloadDeleter {
        void operator()(PacketType *p) const
        {
            if (p) {
                p->~PacketType();
                tui_psram_free(p);
            }
        }
    };

    template <typename... Args> static PacketType *makePayload(Args &&...args)
    {
        void *mem = tui_psram_malloc(sizeof(PacketType));
        if (!mem)
            std::abort(); // what `new` would have done on failure
        return new (mem) PacketType(std::forward<Args>(args)...);
    }

  public:
    template <typename... Args>
    DataPacket(int id, Args &&...args) : Packet(id), data(makePayload(std::forward<Args>(args)...))
    {
    }

    PacketPtr move() override { return PacketPtr(new DataPacket(std::move(*this))); }

    // Disable copying
    DataPacket(const DataPacket &) = delete;
    DataPacket &operator=(const DataPacket &) = delete;

    virtual ~DataPacket() {}

    const PacketType &getData() const { return *data; }

  protected:
    // Enable moving
    DataPacket(DataPacket &&) = default;
    DataPacket &operator=(DataPacket &&) = default;

  private:
    std::unique_ptr<PacketType, PayloadDeleter> data;
};
