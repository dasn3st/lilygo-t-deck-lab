// -----------------------------------------------------------------------------
// T-Deck launcher: FTP file-share bridge (firmware side)
//
// Serves the SD card over WiFi so files can be dragged to/from a computer with any
// FTP client (Windows Explorer, macOS Finder, FileZilla). On-demand only: the UI
// brings WiFi up, starts this, shows the address, and stops+drops WiFi on exit — so
// WiFi (and its RAM cost) is only alive while you're actually transferring.
//
// Storage backend is STORAGE_SD (set in variants/esp32s3/t-deck/platformio.ini), which
// uses the Arduino SD.h instance the firmware already mounts via FSCommon. We do NOT
// call SD.begin() here — the card is already mounted; re-initializing it with the wrong
// (default) SPI pins would break it.
//
// HARDWARE RISK TO VERIFY ON-DEVICE: the SD card and the LoRa radio share the SPI bus,
// serialized elsewhere by spiLock. SimpleFTPServer doesn't know about that lock, so a
// transfer running at the same instant as a radio SPI transaction is the thing to watch
// for (corruption / hangs). Test with real transfers before trusting it with important
// files. If it's a problem, the fix is to guard handleFTP()'s SD access with spiLock.
// -----------------------------------------------------------------------------
#include "configuration.h"

#if HAS_WIFI && defined(HAS_SDCARD)
#include <SimpleFTPServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include "graphics/common/SdCard.h"
#include <cstdio>
#include <cstring>

static FtpServer *ftpSrv = nullptr;
static bool ftpRunning = false;
static WiFiServer downloadHttpServer(80);
static WiFiClient downloadHttpClient;
static bool downloadHttpRunning = false;
static char downloadPath[96] = {};
static char downloadToken[17] = {};
static char downloadHttpRequest[128] = {};
static size_t downloadHttpRequestLength = 0;

static void clearDownloadHttpClient()
{
    downloadHttpClient.stop();
    downloadHttpRequestLength = 0;
    downloadHttpRequest[0] = 0;
}

static void serveDownloadHttpRequest()
{
    char *methodEnd = strchr(downloadHttpRequest, ' ');
    if (!methodEnd) {
        clearDownloadHttpClient();
        return;
    }
    *methodEnd = 0;
    char *path = methodEnd + 1;
    char *pathEnd = strchr(path, ' ');
    if (pathEnd)
        *pathEnd = 0;

    char expected[40];
    snprintf(expected, sizeof(expected), "/download/%s", downloadToken);
    if (strcmp(downloadHttpRequest, "GET") != 0 || strcmp(path, expected) != 0) {
        downloadHttpClient.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        clearDownloadHttpClient();
        return;
    }

    FsFile file = SDFs.open(downloadPath, O_RDONLY);
    if (!file) {
        downloadHttpClient.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        clearDownloadHttpClient();
        return;
    }

    const uint32_t size = file.size();
    const bool geoJson = strstr(downloadPath, ".geojson") != nullptr;
    const char *filename = geoJson ? "kartenpunkte.geojson" : strrchr(downloadPath, '/') + 1;
    downloadHttpClient.print("HTTP/1.1 200 OK\r\nContent-Type: ");
    downloadHttpClient.print(geoJson ? "application/geo+json; charset=utf-8\r\n" : "text/plain; charset=utf-8\r\n");
    downloadHttpClient.print("Content-Disposition: attachment; filename=\"");
    downloadHttpClient.print(filename);
    downloadHttpClient.print("\"\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    downloadHttpClient.print(size);
    downloadHttpClient.print("\r\n\r\n");

    uint8_t buffer[512];
    while (file.available() && downloadHttpClient.connected()) {
        int count = file.read(buffer, sizeof(buffer));
        if (count <= 0)
            break;
        downloadHttpClient.write(buffer, count);
    }
    file.close();
    clearDownloadHttpClient();
}

static void serviceDownloadHttp()
{
    if (!downloadHttpRunning)
        return;
    if (!downloadHttpClient || !downloadHttpClient.connected()) {
        clearDownloadHttpClient();
        downloadHttpClient = downloadHttpServer.available();
        return;
    }
    while (downloadHttpClient.available()) {
        const char c = (char)downloadHttpClient.read();
        if (c == '\n') {
            downloadHttpRequest[downloadHttpRequestLength] = 0;
            serveDownloadHttpRequest();
            return;
        }
        if (c != '\r' && downloadHttpRequestLength + 1 < sizeof(downloadHttpRequest))
            downloadHttpRequest[downloadHttpRequestLength++] = c;
    }
}

extern "C" bool tdeck_download_http_start(const char *path, char *url, size_t urlSize)
{
    const bool notePath = path && strncmp(path, "/notes/", 7) == 0 && !strstr(path, "..");
    const bool pinsPath = path && strcmp(path, "/pins-export.geojson") == 0;
    if ((!notePath && !pinsPath) || !url || urlSize == 0 || WiFi.status() != WL_CONNECTED)
        return false;

    FsFile file = SDFs.open(path, O_RDONLY);
    if (!file)
        return false;
    file.close();

    snprintf(downloadPath, sizeof(downloadPath), "%s", path);
    snprintf(downloadToken, sizeof(downloadToken), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    downloadHttpRequestLength = 0;
    downloadHttpRequest[0] = 0;
    downloadHttpServer.begin();
    downloadHttpServer.setNoDelay(true);
    downloadHttpRunning = true;

    const String ip = WiFi.localIP().toString();
    const int written = snprintf(url, urlSize, "http://%s/download/%s", ip.c_str(), downloadToken);
    if (written < 0 || (size_t)written >= urlSize) {
        downloadHttpServer.end();
        downloadHttpRunning = false;
        downloadPath[0] = 0;
        downloadToken[0] = 0;
        return false;
    }
    return true;
}

extern "C" void tdeck_download_http_stop(void)
{
    if (downloadHttpRunning)
        downloadHttpServer.end();
    downloadHttpRunning = false;
    clearDownloadHttpClient();
    downloadPath[0] = 0;
    downloadToken[0] = 0;
}

// Start the FTP server with a login. Safe to call repeatedly (no-op if already running).
extern "C" void tdeck_ftp_start(const char *user, const char *pass)
{
    if (ftpRunning)
        return;
    if (!ftpSrv)
        ftpSrv = new FtpServer();
    ftpSrv->begin(user, pass);
    ftpRunning = true;
}

// Stop the server (called when the File Share screen closes).
extern "C" void tdeck_ftp_stop(void)
{
    tdeck_download_http_stop();
    if (!ftpRunning)
        return;
    if (ftpSrv)
        ftpSrv->end();
    ftpRunning = false;
}

// Pump the server — must be called often (every UI tick) while running so transfers progress.
extern "C" void tdeck_ftp_service(void)
{
    if (ftpRunning && ftpSrv)
        ftpSrv->handleFTP();
    serviceDownloadHttp();
}

extern "C" bool tdeck_ftp_running(void)
{
    return ftpRunning;
}

#else  // no WiFi or no SD on this build — stubs so the UI links cleanly
extern "C" bool tdeck_download_http_start(const char *, char *, size_t) { return false; }
extern "C" void tdeck_download_http_stop(void) {}
extern "C" void tdeck_ftp_start(const char *, const char *) {}
extern "C" void tdeck_ftp_stop(void) {}
extern "C" void tdeck_ftp_service(void) {}
extern "C" bool tdeck_ftp_running(void) { return false; }
#endif
