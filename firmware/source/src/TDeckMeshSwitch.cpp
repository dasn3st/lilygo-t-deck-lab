// -----------------------------------------------------------------------------
// T-Deck launcher: mesh "kill switch" bridge (firmware side)
//
// The LVGL launcher (device-ui) is compiled with no access to the firmware's
// src/ headers and talks to the mesh only via protobuf messages. These tiny free
// functions are the sanctioned bridge: the UI declares them `extern "C"` and calls
// them; the linker resolves them here where the mesh stack IS reachable.
//
// "off" parks the SX1262 exactly the way NodeDB does during a config swap
// (getRadioIface()->sleep()); "on" restarts RX. Parking frees the shared SPI
// bus + radio ISR and drops radio power draw so a foreground app (e.g. an
// emulator) gets a clean core and a quiet bus.
//
// THREADING (v2 — race-safe): device-ui runs on its own FreeRTOS task, separate
// from the Meshtastic main loop. Touching the radio (SPI transactions, RX state)
// from the UI task races the mesh thread and hangs the UI. So the UI-facing setter
// only RECORDS the desired state; tdeck_mesh_switch_service(), called from the main
// loop(), applies it on the same thread NodeDB parks the radio from — safe and
// serialized with the radio's cooperative OSThread.
// -----------------------------------------------------------------------------
#include "mesh/RadioLibInterface.h"
#include "mesh/Router.h"

static volatile bool s_tdeckMeshEnabled = true; // desired state (written by the UI task)
static volatile bool s_tdeckMeshApplied = true; // last state actually pushed to the radio
static volatile bool s_tdeckMeshPending = false; // a change is waiting to be applied

// Called from the UI (LVGL) task. Only records intent — never touches the radio here.
extern "C" void tdeck_set_mesh_enabled(bool on)
{
    s_tdeckMeshEnabled = on;
    s_tdeckMeshPending = true;
}

extern "C" bool tdeck_get_mesh_enabled(void)
{
    return s_tdeckMeshEnabled;
}

// Called from the main Meshtastic loop() — safe radio context. Applies a pending
// on/off request; if the radio isn't up yet it leaves the request pending and retries
// next loop. Idempotent and cheap when there's nothing to do.
extern "C" void tdeck_mesh_switch_service(void)
{
    if (!s_tdeckMeshPending)
        return;

    bool want = s_tdeckMeshEnabled;
    if (want == s_tdeckMeshApplied) { // UI toggled off then back on before we ran
        s_tdeckMeshPending = false;
        return;
    }

    RadioInterface *ri = router ? router->getRadioIface() : nullptr;
    if (!ri)
        return; // radio not ready yet — keep pending, try again next loop

    if (want) {
        // getRadioIface() on a LoRa board is always a RadioLibInterface subclass
        // (SX126x on the T-Deck); startReceive() re-enters RX mode.
        static_cast<RadioLibInterface *>(ri)->startReceive();
    } else {
        ri->sleep(); // park the radio (base virtual, overridden by SX126x)
    }
    s_tdeckMeshApplied = want;
    s_tdeckMeshPending = false;
}
