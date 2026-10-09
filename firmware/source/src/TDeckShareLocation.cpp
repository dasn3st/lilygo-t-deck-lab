// -----------------------------------------------------------------------------
// TDeckShareLocation - send my position to the mesh right now.
//
// Jake, 2026-09-18: "manua share location and info button?"
//
// Meshtastic broadcasts your position on a timer, and that timer is deliberately slow
// (and slower still when you have not moved) because every broadcast is airtime everyone
// else has to sit through. Which is right, until the moment somebody needs to know where
// you are NOW - which, hunting, is the moment that matters.
//
// So this is a deliberate one-shot: it sends the current position immediately and tells
// you whether it went. It does not change any interval, so nothing about the device's
// normal airtime behaviour changes.
//
// THREADING: the UI (LVGL) task must not transmit. sendOurPosition() allocates a packet and
// hands it to the router, and the radio is exactly the thing that must not be poked from a
// task that could be holding the display's SPI lock. Same deferred-service shape as
// TDeckBeep / TDeckPop: the button records intent, the main loop does it.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include "main.h"
#include "mesh/NodeDB.h"
#include "modules/PositionModule.h"

// 0 = nothing pending, 1 = asked, 2 = sent, 3 = could not (no position to send)
static volatile uint8_t s_state = 0;

extern "C" void tdeck_share_location_request(void)
{
    if (s_state == 1)
        return; // already queued; a second tap must not send twice
    s_state = 1;
}

// 0 idle / 1 sending / 2 sent / 3 failed. The UI polls this to say what happened.
extern "C" uint8_t tdeck_share_location_state(void)
{
    return s_state;
}

extern "C" void tdeck_share_location_clear(void)
{
    if (s_state > 1)
        s_state = 0;
}

extern "C" void tdeck_share_location_service(void)
{
    if (s_state != 1)
        return;

    if (!positionModule || !nodeDB) {
        s_state = 3;
        return;
    }
    // Refuse rather than broadcast a position we do not have: sending 0,0 would put this
    // device in the Gulf of Guinea on everybody else's map, which is worse than silence.
    // copyNodePosition is the accessor NodeDB actually exposes - position lives in its own
    // side table (meshtastic_NodePositionEntry), not on NodeInfoLite.
    meshtastic_PositionLite pos;
    if (!nodeDB->copyNodePosition(nodeDB->getNodeNum(), pos) || (pos.latitude_i == 0 && pos.longitude_i == 0)) {
        s_state = 3;
        return;
    }

    positionModule->sendOurPosition();
    s_state = 2;
}
