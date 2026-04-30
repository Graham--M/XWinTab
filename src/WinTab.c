//
// This is Windows code that provides the wintab API to the application.
// Currently only the parts needed by Rebelle are implemented. If no device is
// found, it will still offer a context, because Rebelle will reconfigure and
// persist the user settings to not use wintab otherwise.
//

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <windows.h>

#include "wintab.h"
#include "XWinTabTypes.h"

typedef void (WINAPI *EventCallback)(EventInfo *);

typedef struct PacketDataTAG {
    UINT pkStatus;
    DWORD pkTime;
    DWORD pkChanged;
    UINT pkSerialNumber;
    DWORD pkButtons;
    LONG pkX;
    LONG pkY;
    UINT pkNormalPressure;
    ORIENTATION pkOrientation;
} PacketData;

typedef struct PacketQueueTAG {
    PacketData *buffer;
    int count;
    int size;
    int nextWrite;
    int nextRead;
} PacketQueue;

typedef struct ContextTAG {
    PacketQueue queue;
    HWND hwnd;
    HCTX handle;
    BOOL enabled;
    BOOL inProximity;
    LOGCONTEXTW logContext;
} Context;


static BOOL g_isLoaded;
static BOOL g_didInit;
static Context g_context;
static DeviceInfo g_deviceInfo;
static BOOL g_PenIsDown = FALSE;
static HHOOK g_mouseHook = NULL;
static HHOOK g_kbHook = NULL;

// Per-app hook behavior
typedef enum AppTypeTAG {
    kAppTypeGeneric  = 0,  // Default: block LBUTTON only, allow MOUSEMOVE
    kAppTypeSimple   = 1,  // MediBang, FireAlpaca: block MOUSEMOVE + LBUTTON
} AppType;
static AppType g_appType = kAppTypeGeneric;

// When TRUE, pen behaves as mouse in Krita (LBUTTON suppression disabled).
// Toggled by pressing the lower barrel button on the stylus.
static BOOL g_mouseModeOverride = FALSE;

static CRITICAL_SECTION g_lock;
static HMODULE g_module;
static HANDLE g_thread;
static BOOL g_threadStop;

static int (WINAPI *pLoad)(void);
static DeviceInfo* (WINAPI *pGetSelectedDevice)();
static int (WINAPI *pBeginEvents)(EventCallback callback);
static int (WINAPI *pCheckEvents)(int timeout);
static int (WINAPI *pShutdown)(void);

#define XWT_PACKET        0
#define XWT_CTXOPEN       1
#define XWT_CTXCLOSE      2
#define XWT_CTXOVERLAP    4
#define XWT_PROXIMITY     5

#define XWINTAB_VERSION_STRINGIFY(S) #S
#define XWINTAB_VERSION_MAJOR 0
#define XWINTAB_VERSION_MINOR 5
#define XWINTAB_NAME(MA,MI) "XWinTab " XWINTAB_VERSION_STRINGIFY(MA) "." \
XWINTAB_VERSION_STRINGIFY(MI)

static const char kXWinTabID[] = XWINTAB_NAME(XWINTAB_VERSION_MAJOR,
                                              XWINTAB_VERSION_MINOR);

static FILE *g_logFile;


// ----------------
// Logging
//

void err_dlg(const WCHAR *msg) {
    MessageBoxW(NULL, msg, L"XWinTab Error", MB_OK);
}

static void init_log() {
    WCHAR wbuffer[MAX_PATH];

    DWORD result = GetEnvironmentVariableW(L"XWINTAB_LOG", wbuffer, MAX_PATH);
    if (!result || result >= MAX_PATH)
        return;

    if (wbuffer[0] != '1' || wbuffer[1] != 0)
        return;

    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, wbuffer))) {
        err_dlg(L"Error getting user folder for log file");
        return;
    }

    const WCHAR suffix[] = L"\\XWinTabLog.txt";
    const size_t suffix_len = sizeof(suffix) / sizeof(WCHAR);
    size_t path_len = wcslen(wbuffer);

    if ((MAX_PATH - suffix_len) < path_len) {
        err_dlg(L"Error opening log file: path length too big.");
        return;
    }

    for (int i = 0; i < suffix_len; i++)
        wbuffer[path_len + i] = suffix[i];

    g_logFile = _wfopen(wbuffer, L"wb");
    if (!g_logFile)
        err_dlg(L"Error opening log file: _wfopen() failed.");
}

static void log_str(const char *str) {
    if (!g_logFile)
        return;
    fprintf(g_logFile, "%s", str);
}

static void log_strf(const char *fmt, ...) {
    if (!g_logFile)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
}

static void close_log() {
    if (!g_logFile)
        return;
    fclose(g_logFile);
    g_logFile = NULL;
}


// ----------------
// Packet Queue
//

static BOOL queue_init(PacketQueue *queue) {
    queue->buffer = (PacketData *) malloc(sizeof(PacketData) * 128);

    if (!queue->buffer)
        return FALSE;

    queue->count = 0;
    queue->size = 128;
    queue->nextRead = 0;
    queue->nextWrite = 0;

    return TRUE;
}

static PacketData* queue_read(PacketQueue *queue) {
    if (!queue->count)
        return NULL;
    PacketData *result = queue->buffer + queue->nextRead;
    queue->count--;
    queue->nextRead = (queue->nextRead + 1) % queue->size;
    return result;
}

static PacketData* queue_peek_prev(PacketQueue *queue) {
    if (!queue->count)
        return NULL;
    if (queue->count == 1)
        return queue->buffer + queue->nextRead;

    int idx = queue->nextWrite ? queue->nextWrite : queue->size;
    return queue->buffer + idx - 1;
}

typedef BOOL (*QueueItrFunc)(PacketData *, void *);

static void queue_iterate(PacketQueue *queue, QueueItrFunc fn, void *userData) {
    if (!queue->count || !fn)
        return;

    int pos = queue->nextRead;
    for (int i = 0; i < queue->count; i++) {
        if (!fn(queue->buffer + pos, userData))
            break;
        pos = (pos + 1) % queue->size;
    }
}

static PacketData* queue_write(PacketQueue *queue) {
    if (queue->count == queue->size)
        return NULL;
    PacketData *result = queue->buffer + queue->nextWrite;
    queue->count++;
    queue->nextWrite = (queue->nextWrite + 1) % queue->size;
    return result;
}

static BOOL queue_resize(PacketQueue *queue, int size) {
    if (size < 1)
        return FALSE;

    if (size == queue->size)
        return TRUE;

    PacketData *resized = (PacketData *) malloc(sizeof(PacketData) * size);
    if (!resized)
        return FALSE;

    if (!queue->count) {
        free(queue->buffer);
        queue->buffer = resized;
        queue->size = size;
        queue->nextRead = 0;
        queue->nextWrite = 0;
        return TRUE;
    }

    int copied = 0;
    int new_count = queue->count > size ? size : queue->count;
    int n = queue->size - queue->nextRead;

    if (n > new_count)
        n = new_count;

    memcpy(resized, &queue->buffer[queue->nextRead], sizeof(PacketData) * n);
    copied = n;

    if (copied != new_count) {
        n = new_count - copied;
        memcpy(&resized[copied], queue->buffer, sizeof(PacketData) * n);
    }

    free(queue->buffer);
    queue->buffer = resized;
    queue->size = size;
    queue->count = new_count;
    queue->nextRead = 0;
    queue->nextWrite = new_count == size ? 0 : new_count;
    return TRUE;
}

static void queue_free(PacketQueue *queue) {
    if (queue->buffer)
        free(queue->buffer);
    queue->buffer = NULL;
    queue->size = 0;
    queue->count = 0;
}


// ----------------
// Event Handling
//

static BOOL context_message(Context *ctx, UINT msg, WPARAM wParam,
                            LPARAM lParam) {
    BOOL result;
    result = PostMessageW(ctx->hwnd, msg + ctx->logContext.lcMsgBase,
                          wParam, lParam);
    if (!result)
        log_strf("context_message send fail\n");
    return result;
                            }

                            static UINT copy_field(void *dst, const void *src, size_t size) {
                                UINT size32 = (UINT) size;

                                const char *ps = (const char *) src;
                                char *pd = (char *) dst;

                                for (UINT i = 0; i < size32; i++)
                                    pd[i] = ps[i];

                                return size32;
                            }

                            static UINT packet_copy(Context *ctx, const PacketData *packet, void *out) {
                                UINT written = 0;
                                char *pkt = (char *) out;
                                WTPKT mask = ctx->logContext.lcPktData;

                                if (mask & PK_CONTEXT)
                                    written += copy_field(&pkt[written], &ctx->handle, sizeof(HCTX));
                                if (mask & PK_STATUS)
                                    written += copy_field(&pkt[written], &packet->pkStatus, sizeof(UINT));
                                if (mask & PK_TIME)
                                    written += copy_field(&pkt[written], &packet->pkTime, sizeof(LONG));
                                if (mask & PK_CHANGED)
                                    written += copy_field(&pkt[written], &packet->pkChanged, sizeof(WTPKT));
                                if (mask & PK_SERIAL_NUMBER)
                                    written += copy_field(&pkt[written], &packet->pkSerialNumber,
                                                          sizeof(UINT));
                                    if (mask & PK_CURSOR) {
                                        UINT data = 1;
                                        written += copy_field(&pkt[written], &data, sizeof(UINT));
                                    }
                                    if (mask & PK_BUTTONS)
                                        written += copy_field(&pkt[written], &packet->pkButtons, sizeof(DWORD));
                                if (mask & PK_X)
                                    written += copy_field(&pkt[written], &packet->pkX, sizeof(LONG));
                                if (mask & PK_Y)
                                    written += copy_field(&pkt[written], &packet->pkY, sizeof(LONG));
                                if (mask & PK_Z) {
                                    LONG data = 0;
                                    written += copy_field(&pkt[written], &data, sizeof(LONG));
                                }
                                if (mask & PK_NORMAL_PRESSURE)
                                    written += copy_field(&pkt[written], &packet->pkNormalPressure,
                                                          sizeof(UINT));
                                    if (mask & PK_TANGENT_PRESSURE) {
                                        // Unsupported
                                        UINT data = 0;
                                        written += copy_field(&pkt[written], &data, sizeof(UINT));
                                    }
                                    if (mask & PK_ORIENTATION)
                                        written += copy_field(&pkt[written], &packet->pkOrientation,
                                                              sizeof(ORIENTATION));
                                        if (mask & PK_ROTATION) {
                                            // Unsupported
                                            ROTATION data;
                                            data.roPitch = data.roRoll = data.roYaw = 0;
                                            written += copy_field(&pkt[written], &data, sizeof(ROTATION));
                                        }
                                        log_strf("packet: x %d y %d p %d serial %d\n",
                                                 packet->pkX, packet->pkY, packet->pkNormalPressure, packet->pkSerialNumber);
                                        return written;
                            }

                            static LONG scale_axis(LONG in, LONG inOrg, LONG inExt, LONG outOrg,
                                                   LONG outExt) {
                                if ((inExt > 0 && outExt > 0) || (inExt < 0 && outExt < 0))
                                    return MulDiv(in - inOrg, abs(outExt), abs(inExt)) + outOrg;

                                return MulDiv(abs(inExt) - (in - inOrg), abs(outExt), abs(inExt)) + outOrg;
                                                   }

                                                   static int calculate_azimuth(int x, int y) {
                                                       double angle = atan2(-x, y) + M_PI;
                                                       int result = 0.5 + (angle * 1800.0 / M_PI);
                                                       return result < 3600 ? result : 0;
                                                   }

                                                   // Hook proc to suppress mouse events when pen is drawing.
                                                    //
                                                    //  kAppTypeGeneric (Krita etc.)
                                                    //    Block WM_LBUTTONDOWN/UP — Krita uses Wintab for drawing so mouse
                                                    //    button events are the only thing triggering a duplicate stroke.
                                                    //    WM_MOUSEMOVE is allowed so the brush cursor still follows the pen.
                                                    //
                                                    //  kAppTypeSimple (MediBang, FireAlpaca etc.)
                                                    //    No mouse events blocked. These apps use WM_LBUTTONDOWN to trigger
                                                    //    their drawing mode but rely on Wintab packets for actual position
                                                    //    and pressure — WM_MOUSEMOVE only updates the brush indicator cursor,
                                                    //    not the stroke data, so blocking it is unnecessary and causes the
                                                    //    indicator to freeze. The x=0,y=0 button packet fix in on_event()
                                                    //    is sufficient to prevent the cursor jumping to the top-left corner.
                                                    static LRESULT CALLBACK suppress_mouse_hook(int nCode, WPARAM wParam, LPARAM lParam) {
                                                        if (nCode >= 0 && g_appType == kAppTypeGeneric) {
                                                            // Lower barrel button = Right click (OTD mapping).
                                                            // Toggle mouse mode on press so pen can interact with Krita UI.
                                                            if (wParam == WM_RBUTTONDOWN) {
                                                                g_mouseModeOverride = !g_mouseModeOverride;
                                                                log_strf("suppress_mouse_hook: lower barrel → mouse mode %s\n",
                                                                        g_mouseModeOverride ? "ON" : "OFF");
                                                                // Don't block the right-click itself — let Krita see it
                                                            }

                                                            // Block LBUTTON while pen is drawing and mouse mode is OFF.
                                                            // This prevents Wine's X11 driver from triggering a duplicate
                                                            // mouse stroke on top of the Wintab stroke.
                                                            if (g_PenIsDown && !g_mouseModeOverride) {
                                                                if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
                                                                    wParam == WM_LBUTTONDBLCLK) {
                                                                    log_strf("suppress_mouse_hook: blocking WM_LBUTTON* (draw mode)\n");
                                                                    return 1;
                                                                }
                                                            }
                                                        }
                                                        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
                                                    }

                                                    // Low-level keyboard hook to detect the barrel button toggle shortcut.
                                                    // OTD maps the lower barrel button to PageDown (VK_NEXT = 0x22).
                                                    // WH_KEYBOARD_LL is system-wide so it catches OTD-injected key events
                                                    // regardless of which thread they arrive on — unlike WH_MOUSE which is
                                                    // thread-local and misses OTD barrel button events.
                                                    static LRESULT CALLBACK kb_hook(int nCode, WPARAM wParam, LPARAM lParam) {
                                                        if (nCode >= 0 && wParam == WM_KEYDOWN) {
                                                            KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *) lParam;
                                                            // Toggle on PageDown (lower barrel button via OTD)
                                                            if (kb->vkCode == VK_NEXT) {
                                                                g_mouseModeOverride = !g_mouseModeOverride;
                                                                log_strf("kb_hook: PageDown (barrel) → mouse mode %s\n",
                                                                        g_mouseModeOverride ? "ON" : "OFF");
                                                                return 1; // eat the keystroke, don't pass to Krita
                                                            }
                                                        }
                                                        return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
                                                    }

                                                    static void WINAPI on_event(EventInfo *ev) {
                                                       static UINT serial = 0;

                                                       EnterCriticalSection(&g_lock);

                                                       BOOL is_proximity = ev->type == kEventTypeProximityIn ||
                                                       ev->type == kEventTypeProximityOut;

                                                       // Sometimes the stylus may already be in proximity by the time we are
                                                       // loaded and some drivers don't emit proximity events at all.
                                                       if (g_deviceInfo.id != -1 && !g_context.inProximity && !is_proximity) {
                                                           log_strf("on_event: Converting normal event to proximity event\n");
                                                           ev->type = kEventTypeProximityIn;
                                                           is_proximity = TRUE;
                                                       }

                                                       if (!g_context.enabled && (!is_proximity || g_deviceInfo.id == -1)) {
                                                           log_strf("on_event: rejecting %d\n", ev->type);
                                                           LeaveCriticalSection(&g_lock);
                                                           return;
                                                       }

                                                       PacketData *pkt = queue_write(&g_context.queue);
                                                       if (!pkt) {
                                                           log_strf("on_event: Queue Full\n");
                                                           // Queue totally full. Last pkt as overflowed.
                                                           pkt = queue_peek_prev(&g_context.queue);
                                                           pkt->pkStatus |= TPS_QUEUE_ERR;
                                                           LeaveCriticalSection(&g_lock);
                                                           return;
                                                       }

                                                       // Extra bump if we wrapped around
                                                       if (!serial)
                                                           serial++;

                                                       memset(pkt, 0, sizeof(PacketData));

                                                       // Set proximity status
                                                       if (ev->type == kEventTypeProximityIn) {
                                                           g_context.inProximity = TRUE;
                                                           pkt->pkStatus |= TPS_PROXIMITY;
                                                       }
                                                       else if (ev->type == kEventTypeProximityOut)
                                                           g_context.inProximity = FALSE;
                                                       else if (g_context.inProximity)
                                                           pkt->pkStatus |= TPS_PROXIMITY;

                                                       // Does this actually need adjusted?
                                                       pkt->pkTime = ev->time;

                                                       pkt->pkSerialNumber = serial++;
                                                       pkt->pkButtons = ev->buttonsState;

                                                       // Detect pen contact (button 1 usually = tip)
                                                       if (ev->buttonsState & 1) {
                                                           g_PenIsDown = TRUE;
                                                       } else {
                                                           g_PenIsDown = FALSE;
                                                       }

                                                       const LOGCONTEXTW *lc = &g_context.logContext;

                                                       // Track last known tablet position so button press events
                                                       // (which arrive with x=0,y=0 from XCB) don't send a
                                                       // zero-coordinate packet to the app. MediBang/FireAlpaca
                                                       // interpret x=0,y=0 as "cursor jumped to top-left corner"
                                                       // which looks like the cursor is frozen/stuck.
                                                       static LONG g_lastPkX = 0;
                                                       static LONG g_lastPkY = 0;

                                                       BOOL is_button_event = (ev->type == kEventTypeButtonPress ||
                                                                               ev->type == kEventTypeButtonRelease);

                                                       if (is_button_event && ev->x == 0 && ev->y == 0) {
                                                           // Use last known position — button events carry no coords
                                                           pkt->pkX = g_lastPkX;
                                                           pkt->pkY = g_lastPkY;
                                                           log_strf("on_event: button event, using last pos x=%d y=%d\n",
                                                                    g_lastPkX, g_lastPkY);
                                                       } else {
                                                           pkt->pkX = scale_axis(ev->x, lc->lcInOrgX, lc->lcInExtX,
                                                                                 lc->lcOutOrgX, lc->lcOutExtX);
                                                           LONG fy = lc->lcInExtY - ev->y;
                                                           pkt->pkY = scale_axis(fy, lc->lcInOrgY, lc->lcInExtY,
                                                                                 lc->lcOutOrgY, lc->lcOutExtY);
                                                           g_lastPkX = pkt->pkX;
                                                           g_lastPkY = pkt->pkY;
                                                       }

                                                       pkt->pkNormalPressure = ev->pressure;

                                                       pkt->pkOrientation.orAltitude = 900 - 15 * max(abs(ev->xTilt),
                                                                                                      abs(ev->yTilt));

                                                       pkt->pkOrientation.orAzimuth = calculate_azimuth(ev->xTilt, ev->yTilt);

                                                       // Can't be bothered computing this properly right now.
                                                       pkt->pkChanged = g_context.logContext.lcPktData;

                                                       log_strf("event: x %d y %d p %d tltx %d tlty %d serial %d\n",
                                                                ev->x, ev->y, ev->pressure, ev->xTilt, ev->yTilt, pkt->pkSerialNumber);

                                                       if (ev->type == kEventTypeProximityIn ||
                                                           ev->type == kEventTypeProximityOut) {
                                                           BOOL isIn = ev->type == kEventTypeProximityIn;
                                                       log_strf("on_event: Send WT_PROXIMITY %d\n", isIn);
                                                       context_message(&g_context, XWT_PROXIMITY, (WPARAM) g_context.handle,
                                                                       MAKELPARAM(isIn, 1));
                                                           }
                                                           else if (g_context.logContext.lcOptions & CXO_MESSAGES) {
                                                               // Always forward Wintab packet to Krita.
                                                               // Mouse suppression when pen is down is handled by
                                                               // the WH_MOUSE hook installed in WTOpenW.
                                                               log_strf("on_event: send WT_PACKET (pen_down=%d)\n", g_PenIsDown);
                                                               context_message(&g_context, XWT_PACKET, pkt->pkSerialNumber,
                                                                               (LPARAM) g_context.handle);
                                                           }

                                                           LeaveCriticalSection(&g_lock);
                                                   }

                                                   static DWORD WINAPI thread_func(void *userdata) {
                                                       while (!g_threadStop) {
                                                           if (!pCheckEvents(100))
                                                               return -1;
                                                       }
                                                       return 0;
                                                   }


                                                   // ----------------
                                                   // Connection and Context Setup
                                                   //

                                                   static BOOL load_xwintab() {
                                                       g_isLoaded = TRUE;
                                                       g_deviceInfo.id = -1;

                                                       if (!g_didInit) {
                                                           g_didInit = TRUE;
                                                           InitializeCriticalSection(&g_lock);

                                                           g_module = LoadLibraryW(L"XWinTabHelper.dll.so");
                                                           if (!g_module) {
                                                               log_strf("Failed to load XWinTabHelper.dll.so\n");
                                                               return FALSE;
                                                           }

                                                           log_strf("Loaded XWinTabHelper.dll.so\n");

                                                           pLoad = (void*) GetProcAddress(g_module, "Load");
                                                           pGetSelectedDevice = (void*) GetProcAddress(g_module, "GetSelectedDevice");
                                                           pBeginEvents = (void*) GetProcAddress(g_module, "BeginEvents");
                                                           pCheckEvents = (void*) GetProcAddress(g_module, "CheckEvents");
                                                           pShutdown = (void*) GetProcAddress(g_module, "Shutdown");

                                                           if (!pLoad || !pGetSelectedDevice || !pBeginEvents || !pCheckEvents || !pShutdown) {
                                                               log_strf("Failed to load helper functions\n");
                                                               FreeLibrary(g_module);
                                                               g_module = NULL;
                                                               return FALSE;
                                                           }

                                                           log_strf("Loaded helper functions\n");
                                                       }

                                                       if (!g_module) {
                                                           log_strf("Skipping setup due to failed helper load\n");
                                                           return FALSE;
                                                       }

                                                       if (!pLoad())
                                                           log_strf("Load() call failed\n");
                                                       else {
                                                           DeviceInfo *device = pGetSelectedDevice();
                                                           if (device) {
                                                               g_deviceInfo = *device;
                                                               log_strf("Using device: %d\n", g_deviceInfo.id);
                                                               return TRUE;
                                                           }

                                                           g_deviceInfo.id = -1;
                                                           log_strf("Couldn't find suitable tablet device\n");
                                                       }

                                                       pShutdown();
                                                       return FALSE;
                                                   }

                                                   static void init_log_context(LPLOGCONTEXTW lctx) {
                                                       memset(lctx, 0, sizeof(LOGCONTEXTW));

                                                       for (size_t i = 0; i < sizeof(kXWinTabID); i++)
                                                           lctx->lcName[i] = kXWinTabID[i];

                                                       lctx->lcOptions = CXO_SYSTEM;
                                                       lctx->lcStatus = CXS_ONTOP;
                                                       lctx->lcLocks = CXL_INSIZE | CXL_INASPECT | CXL_MARGIN |
                                                       CXL_SENSITIVITY | CXL_SYSOUT;
                                                       lctx->lcMsgBase = WT_DEFBASE;
                                                       lctx->lcDevice = 0;
                                                       lctx->lcPktRate = 100;
                                                       lctx->lcPktData = PK_CONTEXT | PK_STATUS | PK_SERIAL_NUMBER| PK_TIME |
                                                       PK_CURSOR | PK_BUTTONS |  PK_X | PK_Y |
                                                       PK_NORMAL_PRESSURE | PK_ORIENTATION;
                                                       lctx->lcPktMode = 0;
                                                       lctx->lcMoveMask = PK_BUTTONS | PK_X | PK_Y | PK_NORMAL_PRESSURE |
                                                       PK_ORIENTATION;
                                                       lctx->lcBtnDnMask = 0xffffffff;
                                                       lctx->lcBtnUpMask = 0xffffffff;

                                                       lctx->lcInOrgZ = lctx->lcInExtZ = lctx->lcOutOrgZ = lctx->lcOutExtZ = 0;

                                                       if (g_deviceInfo.id == -1) {
                                                           // Didn't detect a plugged in tablet so just set some dummy values.
                                                           lctx->lcInOrgX = lctx->lcInOrgY = 0;
                                                           lctx->lcInExtX = lctx->lcInExtY = 1024;
                                                       } else {
                                                           lctx->lcInOrgX = g_deviceInfo.xAxis.min;
                                                           lctx->lcInOrgY = g_deviceInfo.yAxis.min;
                                                           lctx->lcInExtX = g_deviceInfo.xAxis.max - g_deviceInfo.xAxis.min;
                                                           lctx->lcInExtY = g_deviceInfo.yAxis.max - g_deviceInfo.yAxis.min;
                                                       }

                                                       lctx->lcSensX = 65536;
                                                       lctx->lcSensY = 65536;
                                                       lctx->lcSensZ = 65536;
                                                       lctx->lcSysMode = 0;

                                                       lctx->lcSysOrgX = lctx->lcOutOrgX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                                                       lctx->lcSysOrgY = lctx->lcOutOrgY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                                                       lctx->lcSysExtX = lctx->lcOutExtX = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                                                       lctx->lcSysExtY = lctx->lcOutExtY = GetSystemMetrics(SM_CYVIRTUALSCREEN);

                                                       lctx->lcSysSensX = 65536;
                                                       lctx->lcSysSensY = 65536;
                                                   }


                                                   // ----------------
                                                   // API Definitions
                                                   //

                                                   HCTX WINAPI WTOpenW(HWND hwnd, LPLOGCONTEXTW pLContext, BOOL enable) {
                                                       log_strf("WTOpenW: called (%s)\n", enable ? "enabled" : "disabled");
                                                       if (!g_isLoaded)
                                                           load_xwintab();
                                                       if (g_context.handle || !pLContext)
                                                           return NULL;

                                                       if (!pLContext->lcInExtX || !pLContext->lcInExtY)
                                                           return NULL;


                                                       log_strf("WTOpenW: Begin context creation\n");
                                                       g_context.handle = (HCTX) 128;
                                                       g_context.logContext = *pLContext;
                                                       g_context.hwnd = hwnd;

                                                       if (g_deviceInfo.id != -1) {
                                                           g_context.enabled = enable;
                                                           g_context.logContext.lcStatus = enable ? CXS_ONTOP : CXS_DISABLED;

                                                           if (!queue_init(&g_context.queue)) {
                                                               g_deviceInfo.id = -1;
                                                               return NULL;
                                                           }

                                                           // We want to hold off on the thread sending events
                                                           EnterCriticalSection(&g_lock);

                                                           if (!pBeginEvents(on_event)) {
                                                               log_strf("WTOpenW: Failed to select events\n");
                                                               g_context.enabled = FALSE;
                                                               g_deviceInfo.id = -1;
                                                               LeaveCriticalSection(&g_lock);
                                                               pShutdown();
                                                               return NULL;
                                                           }

                                                           g_thread = CreateThread(NULL, 0, thread_func, NULL, 0, NULL);
                                                           if (!g_thread) {
                                                               log_strf("WTOpenW: Failed to start event thread\n");
                                                               g_context.enabled = FALSE;
                                                               g_deviceInfo.id = -1;
                                                               LeaveCriticalSection(&g_lock);
                                                               pShutdown();
                                                               return NULL;
                                                           }

                                                           log_strf("WTOpenW: Started Event Thread\n");
                                                       }

                                                       context_message(&g_context, XWT_CTXOPEN, (WPARAM) g_context.handle,
                                                                       g_context.logContext.lcStatus);

                                                       context_message(&g_context, XWT_CTXOVERLAP, (WPARAM) g_context.handle,
                                                                       g_context.logContext.lcStatus);

                                                       // We can let the event thread do its thing now.
                                                       if (g_deviceInfo.id != -1)
                                                           LeaveCriticalSection(&g_lock);

                                                       // Detect which app we are running under so the hook
                                                       // can apply the correct suppression strategy.
                                                       {
                                                           WCHAR exePath[MAX_PATH] = {0};
                                                           GetModuleFileNameW(NULL, exePath, MAX_PATH);
                                                           // Convert to lowercase for comparison
                                                           WCHAR exeLower[MAX_PATH] = {0};
                                                           for (int i = 0; i < MAX_PATH && exePath[i]; i++)
                                                               exeLower[i] = (WCHAR)towlower(exePath[i]);

                                                           if (wcsstr(exeLower, L"medibangpaint") ||
                                                               wcsstr(exeLower, L"medibang")      ||
                                                               wcsstr(exeLower, L"firealpaca")) {
                                                               g_appType = kAppTypeSimple;
                                                               log_strf("WTOpenW: Detected simple app (MediBang/FireAlpaca), using MOUSEMOVE+LBUTTON suppression\n");
                                                           } else {
                                                               g_appType = kAppTypeGeneric;
                                                               log_strf("WTOpenW: Using generic app mode (LBUTTON suppression only)\n");
                                                           }
                                                       }

                                                       // Install a thread-local WH_MOUSE hook to suppress Wine-generated
                                                       // mouse events while the pen is in contact with the tablet.
                                                       DWORD app_tid = GetWindowThreadProcessId(hwnd, NULL);
                                                       g_mouseHook = SetWindowsHookExW(WH_MOUSE, suppress_mouse_hook, NULL, app_tid);
                                                       log_strf("WTOpenW: Mouse suppress hook %s (tid=%lu, appType=%d)\n",
                                                                g_mouseHook ? "installed" : "FAILED", app_tid, g_appType);

                                                       // Install a system-wide low-level keyboard hook to detect the
                                                       // barrel button toggle (Ctrl+Alt+M). WH_KEYBOARD_LL catches
                                                       // OTD-injected key events regardless of thread — unlike WH_MOUSE.
                                                       // Only installed for Krita (generic app type).
                                                       if (g_appType == kAppTypeGeneric) {
                                                           g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook, NULL, 0);
                                                           log_strf("WTOpenW: Keyboard hook %s\n",
                                                                    g_kbHook ? "installed" : "FAILED");
                                                       }

                                                       return g_context.handle;
                                                   }

                                                   BOOL WINAPI WTClose(HCTX ctx) {
                                                       log_strf("WTClose: called\n");
                                                       if (!ctx || g_context.handle != ctx)
                                                           return FALSE;

                                                       if (g_deviceInfo.id != -1) {
                                                           // Remove mouse suppress hook before stopping the event thread
                                                           if (g_mouseHook) {
                                                               UnhookWindowsHookEx(g_mouseHook);
                                                               g_mouseHook = NULL;
                                                               log_strf("WTClose: Mouse hook removed\n");
                                                           }
                                                           if (g_kbHook) {
                                                               UnhookWindowsHookEx(g_kbHook);
                                                               g_kbHook = NULL;
                                                               log_strf("WTClose: Keyboard hook removed\n");
                                                           }
                                                           g_threadStop = TRUE;
                                                           WaitForSingleObject(g_thread, INFINITE);
                                                           CloseHandle(g_thread);
                                                           g_thread = NULL;
                                                           g_threadStop = FALSE;
                                                           pShutdown();
                                                           g_deviceInfo.id = -1;
                                                           g_context.enabled = FALSE;
                                                           queue_free(&g_context.queue);
                                                           g_context.handle = 0;
                                                       }
                                                       g_isLoaded = FALSE;
                                                       context_message(&g_context, XWT_CTXCLOSE, (WPARAM) g_context.handle,
                                                                       g_context.logContext.lcStatus);
                                                       return TRUE;
                                                   }

                                                   UINT WINAPI WTInfoW(UINT cat, UINT idx, LPVOID ptr) {
                                                       if (!g_isLoaded)
                                                           load_xwintab();

                                                       if ((cat == WTI_DEFCONTEXT || cat == WTI_DEFSYSCTX) && !idx) {
                                                           log_strf("WTInfoW: Request for default logcontext\n");
                                                           if (ptr)
                                                               init_log_context((LPLOGCONTEXTW) ptr);
                                                           return sizeof(LOGCONTEXTW);
                                                       }

                                                       if (cat == WTI_DEVICES && idx == DVC_ORIENTATION) {
                                                           if (ptr) {
                                                               LPAXIS out = (LPAXIS) ptr;
                                                               out[0].axMin = 0;
                                                               out[0].axMax = 3600;
                                                               out[0].axUnits = TU_CIRCLE;
                                                               out[0].axResolution = CASTFIX32(3600);

                                                               out[1].axMin = -900;//-1000;
                                                               out[1].axMax = 900;//1000;
                                                               out[1].axUnits = TU_CIRCLE;
                                                               out[1].axResolution = CASTFIX32(3600);

                                                               out[2].axMin = 0;
                                                               out[2].axMax = 3600;
                                                               out[2].axUnits = TU_CIRCLE;
                                                               out[2].axResolution = CASTFIX32(3600);
                                                           }
                                                           return sizeof(AXIS) * 3;
                                                       }

                                                       if (cat == WTI_DEVICES && idx == DVC_NPRESSURE) {
                                                           if (ptr) {
                                                               LPAXIS out = (LPAXIS) ptr;
                                                               if (g_deviceInfo.id != -1) {
                                                                   out->axMin = g_deviceInfo.pressureAxis.min;
                                                                   out->axMax = g_deviceInfo.pressureAxis.max;
                                                                   out->axResolution = g_deviceInfo.pressureAxis.resolution;
                                                               } else {
                                                                   out->axMin = 0;
                                                                   out->axMax = 1024;
                                                                   out->axResolution = 1;
                                                               }
                                                               out->axUnits = TU_INCHES;
                                                           }
                                                           return sizeof(AXIS);
                                                       }

                                                       if (cat == WTI_DEVICES && idx == DVC_TPRESSURE) {
                                                           if (ptr) {
                                                               // Unsupported
                                                               LPAXIS out = (LPAXIS) ptr;
                                                               out->axMin = 0;
                                                               out->axMax = 0;
                                                               out->axResolution = 0;
                                                               out->axUnits = TU_INCHES;
                                                           }
                                                           return sizeof(AXIS);
                                                       }

                                                       if (cat == (WTI_CURSORS + 1) && idx == CSR_PHYSID) {
                                                           if (ptr) {
                                                               LPDWORD out = (LPDWORD) ptr;
                                                               *out = 0;
                                                           }
                                                           return sizeof(DWORD);
                                                       }

                                                       if (cat == (WTI_CURSORS + 1) && idx == CSR_TYPE) {
                                                           if (ptr) {
                                                               LPUINT out = (LPUINT) ptr;
                                                               *out = 0x822;
                                                           }
                                                           return sizeof(UINT);
                                                       }

                                                       if (cat == (WTI_CURSORS + 1) && idx == CSR_SYSBTNMAP) {
                                                           if (ptr) {
                                                               memset(ptr, 0, sizeof(BYTE)*32);
                                                               LPBYTE out = (LPBYTE) ptr;
                                                               for (int i = 0; i < 8; i++)
                                                                   out[i] = 1 << i;
                                                           }
                                                           return sizeof(BYTE) * 32;
                                                       }

                                                       if (cat == WTI_INTERFACE) {
                                                           if (idx == IFC_WINTABID) {
                                                               if (ptr) {
                                                                   wchar_t *out = (wchar_t *) ptr;
                                                                   for (size_t i = 0; i < sizeof(kXWinTabID); i++)
                                                                       out[i] = kXWinTabID[i];
                                                               }
                                                               return sizeof(kXWinTabID) * sizeof(wchar_t);
                                                           }

                                                           if (idx == IFC_IMPLVERSION) {
                                                               if (ptr) {
                                                                   WORD *out = (WORD *) ptr;
                                                                   *out = (XWINTAB_VERSION_MAJOR << 8) | XWINTAB_VERSION_MINOR;
                                                               }
                                                               return sizeof(WORD);
                                                           }
                                                       }

                                                       log_strf("WTInfow: unhandled cat %d idx %d\n", cat, idx);
                                                       return 0;
                                                   }

                                                   BOOL WINAPI WTEnable(HCTX ctx, BOOL enable) {
                                                       if (!ctx || g_context.handle != ctx)
                                                           return FALSE;
                                                       // We won't be getting any events anyway
                                                       if (g_deviceInfo.id == -1)
                                                           return TRUE;

                                                       EnterCriticalSection(&g_lock);
                                                       g_context.enabled = enable;
                                                       log_strf("WTEnable: %d\n", enable);
                                                       LeaveCriticalSection(&g_lock);

                                                       g_context.logContext.lcStatus = enable ? CXS_ONTOP : CXS_DISABLED;

                                                       context_message(&g_context, XWT_CTXOVERLAP, (WPARAM) g_context.handle,
                                                                       g_context.logContext.lcStatus);
                                                       return TRUE;
                                                   }

                                                   BOOL WINAPI WTOverlap(HCTX ctx, BOOL top) {
                                                       // Stub, there's only one context.
                                                       log_strf("WTOverlap: %d\n", top);
                                                       return (!ctx || g_context.handle != ctx) ? FALSE : TRUE;
                                                   }

                                                   int WINAPI WTPacketsGet(HCTX ctx, int count, LPVOID ptr) {
                                                       log_strf("WTPacketsGet: %d\n", count);
                                                       if (!ctx || g_context.handle != ctx)
                                                           return 0;
                                                       if (count < 1)
                                                           return 0;
                                                       EnterCriticalSection(&g_lock);
                                                       int read = 0;
                                                       char *out = (char *) ptr;
                                                       while (read != count) {
                                                           PacketData *pkt = queue_read(&g_context.queue);
                                                           if (!pkt)
                                                               break;
                                                           log_strf("WTPacketsGet read a packet\n", read);
                                                           if (out)
                                                               out += packet_copy(&g_context, pkt, out);
                                                           read++;
                                                       }
                                                       log_strf("WTPacketsGet Read: %d Packets\n", read);
                                                       LeaveCriticalSection(&g_lock);
                                                       return read;
                                                   }

                                                   typedef struct PktPeekIterData {
                                                       int read;
                                                       int count;
                                                       char *dst;
                                                   } PktPeekIterData;

                                                   static BOOL pkt_peek_itr(PacketData *pkt, void *userData) {
                                                       PktPeekIterData *data = (PktPeekIterData *) data;
                                                       data->dst += packet_copy(&g_context, pkt, data->dst);
                                                       data->read++;
                                                       return data->read < data->count;
                                                   }

                                                   int WINAPI WTPacketsPeek(HCTX ctx, int count, LPVOID ptr) {
                                                       if (!ctx || g_context.handle != ctx)
                                                           return 0;
                                                       if (count < 1)
                                                           return 0;
                                                       EnterCriticalSection(&g_lock);

                                                       PktPeekIterData data;
                                                       data.read = 0;
                                                       data.count = count;
                                                       data.dst = (char *) ptr;

                                                       queue_iterate(&g_context.queue, pkt_peek_itr, &data);

                                                       LeaveCriticalSection(&g_lock);
                                                       return data.read;
                                                   }

                                                   BOOL WINAPI WTGetW(HCTX ctx, LPLOGCONTEXTW pLContext) {
                                                       if (!ctx || g_context.handle != ctx || !pLContext)
                                                           return FALSE;
                                                       *pLContext = g_context.logContext;
                                                       return TRUE;
                                                   }

                                                   int WINAPI WTQueueSizeGet(HCTX ctx) {
                                                       log_strf("WTQueueSizeGet: %d\n", ctx && g_context.handle == ctx);
                                                       if (!ctx || g_context.handle != ctx)
                                                           return 0;
                                                       return g_context.queue.size;
                                                   }

                                                   BOOL WINAPI WTQueueSizeSet(HCTX ctx, int size) {
                                                       log_strf("WTQueueSizeSet: %d\n", size);
                                                       if (!ctx || g_context.handle != ctx || !size)
                                                           return FALSE;
                                                       if (size == g_context.queue.size)
                                                           return TRUE;

                                                       EnterCriticalSection(&g_lock);
                                                       BOOL result = queue_resize(&g_context.queue, size);
                                                       LeaveCriticalSection(&g_lock);
                                                       return result;
                                                   }

                                                   BOOL APIENTRY DllMain(HMODULE hModule, DWORD  reason, LPVOID lpReserved) {
                                                       if (reason == DLL_PROCESS_DETACH) {
                                                           close_log();
                                                           return TRUE;
                                                       }
                                                       if (reason == DLL_PROCESS_ATTACH) {
                                                           init_log();
                                                           return TRUE;
                                                       }
                                                       return FALSE;
                                                   }
