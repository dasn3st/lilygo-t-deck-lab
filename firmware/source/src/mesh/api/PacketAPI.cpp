#ifdef USE_PACKET_API

#include "api/PacketAPI.h"
#include "MeshService.h"
#include "PowerFSM.h"
#include "RadioInterface.h"
#include "modules/AdminModule.h" // bluetoothTornDown
#include "modules/NodeInfoModule.h"

PacketAPI *packetAPI = nullptr;

// The size of ONE queued packet, so the queue depth can be turned into bytes instead of
// guessed at. DataPacket holds a unique_ptr to a heap-allocated copy of this struct, so the
// real cost per entry is this plus two small allocation headers plus the deque node.
extern "C" uint32_t tdeck_pktq_itemsz(void) { return (uint32_t)sizeof(meshtastic_FromRadio); }

PacketAPI *PacketAPI::create(PacketServer *_server)
{
    if (!packetAPI) {
        packetAPI = new PacketAPI(_server);
    }
    return packetAPI;
}

PacketAPI::PacketAPI(PacketServer *_server)
    : concurrency::OSThread("PacketAPI"), isConnected(false), programmingMode(false), server(_server)
{
    api_type = TYPE_PACKET;
}

int32_t PacketAPI::runOnce()
{
    bool success = false;
#ifndef ARCH_PORTDUINO
    // ⚠️ "IS BLUETOOTH UP", NOT "IS BLUETOOTH CONFIGURED ON".
    //
    // This branch exists so that, with a phone attached over Bluetooth, the on-device UI stays
    // out of the way in programming mode. But Wi-Fi and Bluetooth cannot share this radio:
    // bringing Wi-Fi up calls disableBluetooth() and the stack is gone, while the setting stays
    // exactly as it was. The device then spent the rest of its run believing a phone might be
    // listening - feeding its own screen nothing, and letting toPhoneQueue fill up and discard.
    // Measured before this change: 180 seconds, 0 packets delivered, 9 dropped, with our own
    // queue empty the whole time.
    if (config.bluetooth.enabled && !bluetoothTornDown) {
        if (!programmingMode) {
            // in programmingMode we don't send any packets to the client except this one notify
            programmingMode = true;
            success = notifyProgrammingMode();
        }
    } else
#endif
    {
        success = sendPacket();
    }
    success |= receivePacket();
    // A FULL OUTBOUND QUEUE IS NOT "NOTHING TO DO". Now that the queue is bounded by a memory
    // budget rather than by an unreachable 300 (see PacketServer.cpp), it genuinely fills
    // during the boot node sync. Backing off to 50ms in that case would turn every boot into a
    // crawl - come straight back instead, the moment the UI task has drained one.
    if (!success && server && !server->available())
        return 10;
    return success ? 10 : 50;
}

bool PacketAPI::receivePacket(void)
{
    bool data_received = false;
    while (server->hasData()) {
        isConnected = true;
        data_received = true;

        powerFSM.trigger(EVENT_INPUT);
        lastContactMsec = millis();

        meshtastic_ToRadio *mr;
        auto p = server->receivePacket()->move();
        int id = p->getPacketId();
        LOG_DEBUG("Received packet id=%u", id);
        mr = (meshtastic_ToRadio *)&static_cast<DataPacket<meshtastic_ToRadio> *>(p.get())->getData();

        switch (mr->which_payload_variant) {
        case meshtastic_ToRadio_packet_tag: {
            meshtastic_MeshPacket *mp = &mr->packet;
            mp->transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_API;
            printPacket("PACKET FROM QUEUE", mp);
            service->handleToRadio(*mp);
            break;
        }
        case meshtastic_ToRadio_want_config_id_tag: {
            uint32_t config_nonce = mr->want_config_id;
            LOG_INFO("Screen wants config, nonce=%u", config_nonce);
            handleStartConfig();
            break;
        }
        case meshtastic_ToRadio_heartbeat_tag:
            if (mr->heartbeat.nonce == 1) {
                if (nodeInfoModule) {
                    LOG_INFO("Broadcasting nodeinfo ping");
                    nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, true, 0, true);
                }
            } else {
                LOG_DEBUG("Got client heartbeat");
            }
            break;
        default:
            LOG_ERROR("Error: unhandled meshtastic_ToRadio variant: %d", mr->which_payload_variant);
            break;
        }
    }
    return data_received;
}

bool PacketAPI::sendPacket(void)
{
    if (server->available()) {
        // fill dummy buffer; we don't use it, we directly send the fromRadio structure
        uint32_t len = getFromRadio(txBuf);
        if (len != 0) {
            static uint32_t id = 0;
            fromRadioScratch.id = ++id;
            bool result = server->sendPacket(DataPacket<meshtastic_FromRadio>(id, fromRadioScratch));
            if (!result) {
                LOG_ERROR("send queue full");
            }
            return result;
        }
    }
    return false;
}

bool PacketAPI::notifyProgrammingMode(void)
{
    // tell the client we are in programming mode by sending only the bluetooth config state
    LOG_INFO("force client into programmingMode");
    memset(&fromRadioScratch, 0, sizeof(fromRadioScratch));
    fromRadioScratch.id = nodeDB->getNodeNum();
    fromRadioScratch.which_payload_variant = meshtastic_FromRadio_config_tag;
    fromRadioScratch.config.which_payload_variant = meshtastic_Config_bluetooth_tag;
    fromRadioScratch.config.payload_variant.bluetooth = config.bluetooth;
    return server->sendPacket(DataPacket<meshtastic_FromRadio>(0, fromRadioScratch));
}

/**
 * return true if we got (once!) contact from our client and the server send queue is not full
 */
// ⛔ A FULL QUEUE IS BACKPRESSURE, NOT A DISCONNECT. This used to return
//       isConnected && server->available()
// and PhoneAPI::checkConnectionTimeout() treats a false here as "Lost phone connection" and
// calls close() - which stops the device sending ANYTHING to its own screen until the UI
// talks first. So one momentary bit of backpressure permanently starved the display, and
// Meshtastic's toPhoneQueue then filled up and discarded real packets.
//
// Measured on the device: 180 seconds, 0 packets delivered to the UI, 9 dropped, with our own
// queue reading 0 the whole time - because the session had already been closed.
//
// The two questions were tangled together. available() answers "can I take another packet
// right now", which is transient and says nothing about whether the client exists. The client
// here is the screen: it cannot walk away like a phone can, and it is marked present the
// moment it sends anything. Runaway growth is bounded by the heap floor in
// PacketServer::available(), which is the right place for that.
bool PacketAPI::checkIsConnected()
{
    isConnected |= server->hasData();
    return isConnected;
}

#endif