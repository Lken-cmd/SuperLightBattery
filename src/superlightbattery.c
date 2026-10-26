#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <shellapi.h>
#include <strsafe.h>
#include <initguid.h>
#include <devpkey.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define LOGITECH_VID 0x046D
#define HIDPP_USAGE_PAGE 0xFF00
#define HIDPP_REPORT_SHORT 0x10
#define HIDPP_REPORT_LONG 0x11
#define HIDPP_SHORT_LEN 7
#define HIDPP_LONG_LEN 20
#define HIDPP_TRAY_SW_ID 0x01
#define HIDPP_CLI_SW_ID 0x02
#define HIDPP10_ERROR 0x8F
#define HIDPP20_ERROR 0xFF
#define HIDPP10_CONNECTION 0x41
#define HIDPP10_LINK_DOWN 0x40
#define HIDPP_DIRECT_INDEX 0xFF
#define HIDPP_SLOTS 7
#define HIDPP_ALL_SLOTS ((BYTE)((1u << HIDPP_SLOTS) - 1))
#define HIDPP_REPLY_TIMEOUT_MS 4000
#define HIDPP_MENU_TIMEOUT_MS 1000

#define HIDPP_FEATURE_DEVICE_NAME 0x0005
#define HIDPP_FEATURE_BATTERY_STATUS 0x1000
#define HIDPP_FEATURE_UNIFIED_BATTERY 0x1004
#define HIDPP_FEATURE_ADJUSTABLE_DPI 0x2201
#define HIDPP_FEATURE_ONBOARD_PROFILES 0x8100

#define DEVICE_NAME_MAX 64
#define PRODUCT_NAME_MAX 64
#define MAX_HID_PATH_CHARS 512
#define MAX_CANDIDATES 16
#define MAX_PORTS 8
#define MAX_DEVICES 8

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_UID 1
#define TRAY_RESCAN_TIMER 1
#define TRAY_RESCAN_DELAY_MS 5000
#define TRAY_MENU_REFRESH 1002
#define TRAY_MENU_EXIT 1003

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef enum HidppStatus {
    HIDPP_FAILED = 0,
    HIDPP_OK,
    HIDPP_REJECTED
} HidppStatus;

typedef enum SlotState {
    SLOT_UNKNOWN = 0,
    SLOT_ABSENT,
    SLOT_READY
} SlotState;

typedef struct HidCandidate {
    WCHAR path[MAX_HID_PATH_CHARS];
    WCHAR product[PRODUCT_NAME_MAX];
    DEVINST parent;
    USHORT pid;
    USHORT usage;
    USHORT inputReportBytes;
    USHORT outputReportBytes;
} HidCandidate;

typedef struct HidPipe {
    HANDLE handle;
    HANDLE event;
    HDEVNOTIFY notify;
    OVERLAPPED overlapped;
    DWORD reportBytes;
    BOOL pending;
    BYTE report[HIDPP_LONG_LEN];
} HidPipe;

typedef struct DpiInfo {
    BOOL found;
    int currentDpi;
    int minDpi;
    int maxDpi;
} DpiInfo;

typedef struct ProfileInfo {
    BOOL found;
    int rawIndex;
    int currentDpiIndex;
} ProfileInfo;

typedef struct HidppPort HidppPort;

/* One physical device, keyed by its firmware name so a mouse seen on the cable and on the receiver stays one entry. */
typedef struct Device {
    BOOL used;
    BOOL online;
    BOOL batteryKnown;
    BOOL charging;
    int percent;
    const WCHAR *state;
    HidppPort *port;
    BYTE index;
    WCHAR name[DEVICE_NAME_MAX];
    DpiInfo dpi;
    ProfileInfo profile;
} Device;

typedef struct DeviceSlot {
    SlotState state;
    BOOL detailFeaturesKnown;
    BYTE batteryFeature;
    BYTE dpiFeature;
    BYTE profileFeature;
    USHORT batteryFeatureId;
    Device *device;
} DeviceSlot;

/* One HID++ interface: the long collection carries requests and device events, the short one receiver notices and errors. */
struct HidppPort {
    BOOL used;
    BOOL dead;
    DEVINST parent;
    USHORT pid;
    BYTE pendingSlots;
    BYTE readSlots;
    WCHAR product[PRODUCT_NAME_MAX];
    HANDLE writeEvent;
    HidPipe longPipe;
    HidPipe shortPipe;
    DeviceSlot slots[HIDPP_SLOTS];
};

static HidppPort g_ports[MAX_PORTS];
static Device g_devices[MAX_DEVICES];
static BYTE g_swId = HIDPP_CLI_SW_ID;
/* A device that has been idle for minutes can take seconds to answer the first request. */
static DWORD g_replyTimeoutMs = HIDPP_REPLY_TIMEOUT_MS;
static BOOL g_rescanPending = FALSE;
static BOOL g_trayDirty = FALSE;
/* Set only in tray mode; open devices are then registered for removal requests. */
static HWND g_trayWindow = NULL;

static BOOL ContainsNoCase(const WCHAR *haystack, const WCHAR *needle)
{
    return FindStringOrdinal(FIND_FROMSTART, haystack, -1, needle, -1, TRUE) >= 0;
}

static const WCHAR *DisplayStringOrFallback(const WCHAR *value, const WCHAR *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static int ReadBe16(const BYTE *bytes)
{
    return ((int)bytes[0] << 8) | bytes[1];
}

static BOOL IsPrintableAscii(BYTE value)
{
    return value >= 0x20 && value <= 0x7E;
}

static int SlotForIndex(BYTE index)
{
    if (index == HIDPP_DIRECT_INDEX) {
        return 0;
    }
    return (index >= 1 && index < HIDPP_SLOTS) ? index : -1;
}

static BYTE IndexForSlot(int slot)
{
    return slot == 0 ? HIDPP_DIRECT_INDEX : (BYTE)slot;
}

static BOOL ReadParentDevNode(const WCHAR *path, DEVINST *parent)
{
    WCHAR instanceId[MAX_DEVICE_ID_LEN];
    ULONG bytes = sizeof(instanceId);
    DEVPROPTYPE type;
    DEVINST devNode;

    return CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type, (PBYTE)instanceId, &bytes, 0) == CR_SUCCESS &&
        CM_Locate_DevNodeW(&devNode, instanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        CM_Get_Parent(parent, devNode, 0) == CR_SUCCESS;
}

static BOOL ReadCandidate(HidCandidate *candidate)
{
    HANDLE handle = CreateFileW(candidate->path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HIDD_ATTRIBUTES attributes;
    PHIDP_PREPARSED_DATA preparsedData = NULL;
    HIDP_CAPS caps;
    BOOL ok;

    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    ZeroMemory(&attributes, sizeof(attributes));
    attributes.Size = sizeof(attributes);
    ZeroMemory(&caps, sizeof(caps));
    ok = HidD_GetAttributes(handle, &attributes) && attributes.VendorID == LOGITECH_VID &&
        HidD_GetPreparsedData(handle, &preparsedData);
    if (ok) {
        ok = HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == HIDPP_USAGE_PAGE &&
            (caps.InputReportByteLength == HIDPP_SHORT_LEN ||
             (caps.InputReportByteLength == HIDPP_LONG_LEN && caps.OutputReportByteLength == HIDPP_LONG_LEN));
        HidD_FreePreparsedData(preparsedData);
    }

    if (ok) {
        candidate->pid = attributes.ProductID;
        candidate->usage = caps.Usage;
        candidate->inputReportBytes = caps.InputReportByteLength;
        candidate->outputReportBytes = caps.OutputReportByteLength;
        candidate->product[0] = L'\0';
        (void)HidD_GetProductString(handle, candidate->product, sizeof(candidate->product));
        candidate->product[ARRAY_LEN(candidate->product) - 1] = L'\0';
        ok = ReadParentDevNode(candidate->path, &candidate->parent);
    }

    CloseHandle(handle);
    return ok;
}

static int EnumerateHidppCandidates(HidCandidate *candidates, int maxCandidates)
{
    GUID hidGuid;
    WCHAR *list = NULL;
    ULONG listChars = 0;
    CONFIGRET result;
    int count = 0;

    HidD_GetHidGuid(&hidGuid);
    do {
        free(list);
        list = NULL;
        if (CM_Get_Device_Interface_List_SizeW(&listChars, &hidGuid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
            return 0;
        }
        list = (WCHAR *)malloc((size_t)listChars * sizeof(WCHAR));
        if (!list) {
            return 0;
        }
        result = CM_Get_Device_Interface_ListW(&hidGuid, NULL, list, listChars, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    } while (result == CR_BUFFER_SMALL);

    for (const WCHAR *path = list; result == CR_SUCCESS && *path && count < maxCandidates; path += wcslen(path) + 1) {
        HidCandidate *candidate = &candidates[count];

        if (ContainsNoCase(path, L"VID_046D") &&
            SUCCEEDED(StringCchCopyW(candidate->path, ARRAY_LEN(candidate->path), path)) &&
            ReadCandidate(candidate)) {
            ++count;
        }
    }

    free(list);
    return count;
}

static BOOL ArmPipe(HidPipe *pipe)
{
    ZeroMemory(&pipe->overlapped, sizeof(pipe->overlapped));
    pipe->overlapped.hEvent = pipe->event;
    pipe->pending = ReadFile(pipe->handle, pipe->report, pipe->reportBytes, NULL, &pipe->overlapped) ||
        GetLastError() == ERROR_IO_PENDING;
    return pipe->pending;
}

static void ClosePipe(HidPipe *pipe)
{
    DWORD bytes;

    if (pipe->notify) {
        UnregisterDeviceNotification(pipe->notify);
    }
    if (pipe->pending) {
        CancelIoEx(pipe->handle, &pipe->overlapped);
        (void)GetOverlappedResult(pipe->handle, &pipe->overlapped, &bytes, TRUE);
    }
    if (pipe->handle) {
        CloseHandle(pipe->handle);
    }
    if (pipe->event) {
        CloseHandle(pipe->event);
    }
    ZeroMemory(pipe, sizeof(*pipe));
}

static BOOL OpenPipe(HidPipe *pipe, const WCHAR *path, DWORD access, DWORD reportBytes)
{
    HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    pipe->handle = handle;
    pipe->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    pipe->reportBytes = reportBytes;
    if (g_trayWindow) {
        /* The tray holds devices open, so it must let go when Device Manager wants to disable or update one. */
        DEV_BROADCAST_HANDLE filter;

        ZeroMemory(&filter, sizeof(filter));
        filter.dbch_size = sizeof(filter);
        filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
        filter.dbch_handle = handle;
        pipe->notify = RegisterDeviceNotificationW(g_trayWindow, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    if (pipe->event && ArmPipe(pipe)) {
        return TRUE;
    }

    ClosePipe(pipe);
    return FALSE;
}

static BOOL CompletePipeRead(HidppPort *port, HidPipe *pipe, DWORD *bytes)
{
    pipe->pending = FALSE;
    if (GetOverlappedResult(pipe->handle, &pipe->overlapped, bytes, FALSE)) {
        return TRUE;
    }

    port->dead = TRUE;
    return FALSE;
}

static void RearmPipe(HidppPort *port, HidPipe *pipe)
{
    if (!ArmPipe(pipe)) {
        port->dead = TRUE;
    }
}

/* If another port still reaches the device (cable and receiver at once), read it there instead of leaving it offline. */
static void ScheduleReadElsewhere(const Device *device, const HidppPort *closing)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        HidppPort *other = &g_ports[i];

        for (int slot = 0; other != closing && other->used && slot < HIDPP_SLOTS; ++slot) {
            if (other->slots[slot].state == SLOT_READY && other->slots[slot].device == device) {
                other->readSlots |= (BYTE)(1u << slot);
            }
        }
    }
}

static void ClosePort(HidppPort *port)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].port == port) {
            g_devices[i].port = NULL;
            g_devices[i].online = FALSE;
            ScheduleReadElsewhere(&g_devices[i], port);
        }
    }

    ClosePipe(&port->longPipe);
    ClosePipe(&port->shortPipe);
    if (port->writeEvent) {
        CloseHandle(port->writeEvent);
    }
    ZeroMemory(port, sizeof(*port));
    g_trayDirty = TRUE;
}

static void CloseDeadPorts(void)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        if (g_ports[i].used && g_ports[i].dead) {
            ClosePort(&g_ports[i]);
        }
    }
}

static void CloseAllPorts(void)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        if (g_ports[i].used) {
            ClosePort(&g_ports[i]);
        }
    }
}

static void ClosePortWithHandle(HANDLE handle)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        if (g_ports[i].used && (g_ports[i].longPipe.handle == handle || g_ports[i].shortPipe.handle == handle)) {
            ClosePort(&g_ports[i]);
        }
    }
}

static void OpenCandidate(const HidCandidate *candidate)
{
    BOOL longReports = candidate->inputReportBytes == HIDPP_LONG_LEN;
    HidppPort *port = NULL;
    HidppPort *spare = NULL;
    HidPipe *pipe;

    for (int i = 0; i < MAX_PORTS && !port; ++i) {
        if (g_ports[i].used && !g_ports[i].dead && g_ports[i].parent == candidate->parent) {
            port = &g_ports[i];
        } else if (!g_ports[i].used && !spare) {
            spare = &g_ports[i];
        }
    }

    if (!port) {
        if (!spare) {
            return;
        }
        port = spare;
        port->writeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!port->writeEvent) {
            return;
        }
        port->used = TRUE;
        port->parent = candidate->parent;
        port->pid = candidate->pid;
        (void)StringCchCopyW(port->product, ARRAY_LEN(port->product), candidate->product);
    }

    pipe = longReports ? &port->longPipe : &port->shortPipe;
    if (pipe->handle) {
        return;
    }
    if (!OpenPipe(pipe, candidate->path, longReports ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, candidate->inputReportBytes)) {
        if (!port->longPipe.handle && !port->shortPipe.handle) {
            port->dead = TRUE;
        }
        return;
    }

    if (longReports) {
        port->pendingSlots = HIDPP_ALL_SLOTS;
    }
}

static void ScanPorts(void)
{
    HidCandidate candidates[MAX_CANDIDATES];
    int count = EnumerateHidppCandidates(candidates, MAX_CANDIDATES);

    for (int i = 0; i < count; ++i) {
        OpenCandidate(&candidates[i]);
    }
}

/* The two battery features number their charging states differently. */
static const WCHAR *BatteryStateText(USHORT featureId, BYTE status, BOOL *charging)
{
    static const WCHAR *const unified[] = {L"Discharging", L"Charging", L"Charging slowly", L"Full", L"Charging error"};
    static const WCHAR *const legacy[] = {
        L"Discharging", L"Charging", L"Charging", L"Full", L"Charging slowly", L"Invalid battery", L"Thermal error"};

    if (featureId == HIDPP_FEATURE_UNIFIED_BATTERY) {
        *charging = status == 1 || status == 2;
        return status < ARRAY_LEN(unified) ? unified[status] : L"";
    }
    *charging = status == 1 || status == 2 || status == 4;
    return status < ARRAY_LEN(legacy) ? legacy[status] : L"";
}

/* Status replies and battery events share one layout: percentage in byte 4, charging state in byte 6. */
static void ApplyBattery(Device *device, USHORT featureId, const BYTE *report)
{
    if (report[4] > 100) {
        return;
    }

    device->percent = report[4];
    device->state = BatteryStateText(featureId, report[6], &device->charging);
    device->batteryKnown = TRUE;
    g_trayDirty = TRUE;
}

static void LinkDevice(Device *device, HidppPort *port, BYTE index)
{
    if (device->port != port || device->index != index || !device->online) {
        device->port = port;
        device->index = index;
        device->online = TRUE;
        g_trayDirty = TRUE;
    }
}

/* The device whose current connection is this port and index, if any. */
static Device *DeviceAt(const HidppPort *port, BYTE index)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].used && g_devices[i].port == port && g_devices[i].index == index) {
            return &g_devices[i];
        }
    }
    return NULL;
}

static void MarkOffline(const HidppPort *port, BYTE index)
{
    Device *device = DeviceAt(port, index);

    if (device && device->online) {
        device->online = FALSE;
        g_trayDirty = TRUE;
    }
}

/* Only valid while the device is connected (device->port is set). */
static DeviceSlot *SlotOf(const Device *device)
{
    return &device->port->slots[SlotForIndex(device->index)];
}

static void HandleReport(HidppPort *port, const BYTE *report, DWORD bytes)
{
    int slot = bytes >= HIDPP_SHORT_LEN ? SlotForIndex(report[1]) : -1;
    BYTE bit;
    DeviceSlot *s;

    if (slot < 0) {
        return;
    }
    bit = (BYTE)(1u << slot);
    s = &port->slots[slot];

    if (report[0] == HIDPP_REPORT_SHORT && report[2] == HIDPP10_CONNECTION) {
        if (report[4] & HIDPP10_LINK_DOWN) {
            MarkOffline(port, report[1]);
        } else if (s->state == SLOT_READY) {
            LinkDevice(s->device, port, report[1]);
            port->readSlots |= bit;
        } else {
            port->pendingSlots |= bit;
        }
        return;
    }

    if (report[0] != HIDPP_REPORT_LONG || bytes != HIDPP_LONG_LEN) {
        return;
    }

    if (s->state == SLOT_UNKNOWN) {
        /* Any traffic, including a reply that arrived after its timeout, means the device is awake now. */
        port->pendingSlots |= bit;
    } else if (s->state == SLOT_READY && report[2] == s->batteryFeature && report[3] == 0) {
        LinkDevice(s->device, port, report[1]);
        ApplyBattery(s->device, s->batteryFeatureId, report);
    }
}

static HidppStatus HidppRequest(
    HidppPort *port,
    BYTE index,
    BYTE feature,
    BYTE function,
    BYTE param0,
    BYTE param1,
    BYTE reply[HIDPP_LONG_LEN])
{
    BYTE request[HIDPP_LONG_LEN] = {0};
    OVERLAPPED write;
    DWORD bytes = 0;
    ULONGLONG deadline;

    if (port->dead || !port->longPipe.handle) {
        return HIDPP_FAILED;
    }

    request[0] = HIDPP_REPORT_LONG;
    request[1] = index;
    request[2] = feature;
    request[3] = (BYTE)((function << 4) | g_swId);
    request[4] = param0;
    request[5] = param1;

    ZeroMemory(&write, sizeof(write));
    write.hEvent = port->writeEvent;
    if (!WriteFile(port->longPipe.handle, request, sizeof(request), NULL, &write) && GetLastError() != ERROR_IO_PENDING) {
        return HIDPP_FAILED;
    }
    if (WaitForSingleObject(port->writeEvent, g_replyTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(port->longPipe.handle, &write);
        (void)GetOverlappedResult(port->longPipe.handle, &write, &bytes, TRUE);
        return HIDPP_FAILED;
    }
    if (!GetOverlappedResult(port->longPipe.handle, &write, &bytes, FALSE)) {
        return HIDPP_FAILED;
    }

    /* Replies come back on the long collection; receivers report errors for missing devices on the short one. */
    deadline = GetTickCount64() + g_replyTimeoutMs;
    for (;;) {
        HidPipe *pipes[2];
        HANDLE events[2];
        DWORD count = 0;
        ULONGLONG now = GetTickCount64();
        DWORD wait;
        HidPipe *pipe;
        const BYTE *report;

        if (port->longPipe.pending) {
            pipes[count] = &port->longPipe;
            events[count++] = port->longPipe.event;
        }
        if (port->shortPipe.pending) {
            pipes[count] = &port->shortPipe;
            events[count++] = port->shortPipe.event;
        }
        if (count == 0 || now >= deadline) {
            return HIDPP_FAILED;
        }

        wait = WaitForMultipleObjects(count, events, FALSE, (DWORD)(deadline - now));
        if (wait >= WAIT_OBJECT_0 + count) {
            return HIDPP_FAILED;
        }

        pipe = pipes[wait - WAIT_OBJECT_0];
        if (!CompletePipeRead(port, pipe, &bytes)) {
            return HIDPP_FAILED;
        }

        report = pipe->report;
        if (bytes >= HIDPP_SHORT_LEN && report[1] == index) {
            if (report[0] == HIDPP_REPORT_LONG && bytes == HIDPP_LONG_LEN && report[2] == feature && report[3] == request[3]) {
                CopyMemory(reply, report, HIDPP_LONG_LEN);
                RearmPipe(port, pipe);
                return HIDPP_OK;
            }
            if ((report[2] == HIDPP20_ERROR || report[2] == HIDPP10_ERROR) && report[3] == feature && report[4] == request[3]) {
                /* A receiver error means the device is not there; a device error (busy, ...) is only a failed request. */
                RearmPipe(port, pipe);
                return report[2] == HIDPP10_ERROR ? HIDPP_REJECTED : HIDPP_FAILED;
            }
        }

        HandleReport(port, report, bytes);
        RearmPipe(port, pipe);
    }
}

/* Looks up a feature with the root feature's getFeature. HIDPP_OK with *feature == 0 means the device lacks it. */
static HidppStatus HidppFeatureIndex(HidppPort *port, BYTE index, USHORT featureId, BYTE *feature)
{
    BYTE reply[HIDPP_LONG_LEN];
    HidppStatus status = HidppRequest(port, index, 0x00, 0x00, (BYTE)(featureId >> 8), (BYTE)featureId, reply);

    *feature = 0;
    if (status == HIDPP_OK && reply[4] != 0xFF) {
        *feature = reply[4];
    }
    return status;
}

/* HIDPP_REJECTED: the device has no usable name. Anything short of the full name fails, because a partial
   name would become a second entry for the same device. */
static HidppStatus ReadFirmwareName(HidppPort *port, BYTE index, WCHAR *name, int nameChars)
{
    BYTE feature;
    BYTE reply[HIDPP_LONG_LEN];
    int length;
    int copied = 0;

    if (HidppFeatureIndex(port, index, HIDPP_FEATURE_DEVICE_NAME, &feature) != HIDPP_OK) {
        return HIDPP_FAILED;
    }
    if (!feature) {
        return HIDPP_REJECTED;
    }
    /* getDeviceNameCount */
    if (HidppRequest(port, index, feature, 0x00, 0, 0, reply) != HIDPP_OK) {
        return HIDPP_FAILED;
    }

    length = reply[4];
    if (length <= 0 || length >= nameChars) {
        return HIDPP_REJECTED;
    }

    while (copied < length) {
        int chunkStart = copied;

        /* getDeviceName returns up to 16 characters from the given offset. */
        if (HidppRequest(port, index, feature, 0x01, (BYTE)copied, 0, reply) != HIDPP_OK) {
            return HIDPP_FAILED;
        }
        for (int i = 4; i < HIDPP_LONG_LEN && copied < length && reply[i]; ++i) {
            name[copied++] = IsPrintableAscii(reply[i]) ? (WCHAR)reply[i] : L'?';
        }
        if (copied == chunkStart) {
            /* The name ended early; the device over-reported its length. */
            break;
        }
    }

    name[copied] = L'\0';
    return copied > 0 ? HIDPP_OK : HIDPP_REJECTED;
}

static Device *DeviceForName(const WCHAR *name)
{
    Device *spare = NULL;

    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].used && wcscmp(g_devices[i].name, name) == 0) {
            return &g_devices[i];
        }
        if (!g_devices[i].used && !spare) {
            spare = &g_devices[i];
        }
    }

    if (spare) {
        ZeroMemory(spare, sizeof(*spare));
        spare->used = TRUE;
        spare->state = L"";
        (void)StringCchCopyW(spare->name, ARRAY_LEN(spare->name), name);
    }
    return spare;
}

static HidppStatus ReadSlotBattery(HidppPort *port, int slot)
{
    const DeviceSlot *s = &port->slots[slot];
    BYTE index = IndexForSlot(slot);
    BYTE reply[HIDPP_LONG_LEN];
    HidppStatus status;

    if (s->state != SLOT_READY) {
        return HIDPP_FAILED;
    }

    /* getStatus is function 1 of Unified Battery and function 0 (getBatteryLevelStatus) of Battery Status. */
    status = HidppRequest(port, index, s->batteryFeature, s->batteryFeatureId == HIDPP_FEATURE_UNIFIED_BATTERY ? 0x01 : 0x00, 0, 0, reply);
    if (status == HIDPP_OK) {
        LinkDevice(s->device, port, index);
        ApplyBattery(s->device, s->batteryFeatureId, reply);
    } else if (status == HIDPP_REJECTED) {
        /* The receiver answers for a device that is asleep or switched off. */
        MarkOffline(port, index);
    }
    return status;
}

static HidppStatus ReadBattery(Device *device)
{
    return device->port ? ReadSlotBattery(device->port, SlotForIndex(device->index)) : HIDPP_FAILED;
}

static void IdentifySlot(HidppPort *port, int slot)
{
    DeviceSlot *s = &port->slots[slot];
    BYTE index = IndexForSlot(slot);
    USHORT featureId = HIDPP_FEATURE_UNIFIED_BATTERY;
    BYTE feature;
    BYTE reply[HIDPP_LONG_LEN];
    WCHAR name[DEVICE_NAME_MAX];
    HidppStatus status = HidppFeatureIndex(port, index, featureId, &feature);
    Device *device;
    Device *previous;

    /* Unified Battery only counts when it reports a percentage (getCapabilities flag bit 1), not just coarse levels. */
    if (status == HIDPP_OK && feature) {
        status = HidppRequest(port, index, feature, 0x00, 0, 0, reply);
        if (status == HIDPP_OK && !(reply[5] & 0x02)) {
            feature = 0;
        }
    }
    if (status == HIDPP_OK && !feature) {
        featureId = HIDPP_FEATURE_BATTERY_STATUS;
        status = HidppFeatureIndex(port, index, featureId, &feature);
    }
    if (status == HIDPP_OK && feature) {
        status = ReadFirmwareName(port, index, name, ARRAY_LEN(name));
        if (status == HIDPP_REJECTED) {
            /* No name feature: the HID product string is the best key left. */
            (void)StringCchCopyW(name, ARRAY_LEN(name), DisplayStringOrFallback(port->product, L"Logitech device"));
            status = HIDPP_OK;
        }
    }

    if (status == HIDPP_FAILED) {
        /* No answer: keep what is known and try again on the device's next traffic. */
        if (s->state != SLOT_READY) {
            s->state = SLOT_UNKNOWN;
        }
        return;
    }
    if (status == HIDPP_REJECTED || !feature) {
        MarkOffline(port, index);
        s->state = SLOT_ABSENT;
        return;
    }

    device = DeviceForName(name);
    if (!device) {
        s->state = SLOT_ABSENT;
        return;
    }

    previous = DeviceAt(port, index);
    if (previous && previous != device) {
        /* The slot now holds a different device, so the old entry is no longer reachable here. */
        previous->port = NULL;
        previous->online = FALSE;
    }

    ZeroMemory(s, sizeof(*s));
    s->state = SLOT_READY;
    s->batteryFeature = feature;
    s->batteryFeatureId = featureId;
    s->device = device;
    LinkDevice(device, port, index);
    (void)ReadSlotBattery(port, slot);
}

/* Runs the identifications and battery reads that reports and port changes have queued for this port. */
static void ProcessPortWork(HidppPort *port)
{
    BOOL progress = TRUE;

    while (progress && !port->dead && port->longPipe.handle) {
        progress = FALSE;
        for (int slot = 0; slot < HIDPP_SLOTS && !port->dead; ++slot) {
            BYTE bit = (BYTE)(1u << slot);

            /* Receiver slots need the short collection, or every missing device costs a full timeout. */
            if ((port->pendingSlots & bit) && (slot == 0 || port->shortPipe.handle)) {
                port->pendingSlots &= (BYTE)~bit;
                progress = TRUE;
                if (slot > 0 && port->slots[0].state == SLOT_READY) {
                    continue;
                }

                IdentifySlot(port, slot);
                if (port->slots[slot].state == SLOT_READY) {
                    /* Identification just read the battery; traffic seen meanwhile needs no second pass. */
                    port->pendingSlots &= (BYTE)~bit;
                    port->readSlots &= (BYTE)~bit;
                }
                if (slot == 0 && port->slots[0].state == SLOT_READY) {
                    /* A directly connected device answers on every index. */
                    for (int other = 1; other < HIDPP_SLOTS; ++other) {
                        port->slots[other].state = SLOT_ABSENT;
                    }
                }
            } else if (port->readSlots & bit) {
                port->readSlots &= (BYTE)~bit;
                progress = TRUE;
                (void)ReadSlotBattery(port, slot);
            }
        }
    }
}

static void ProcessAllPorts(void)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        if (g_ports[i].used) {
            ProcessPortWork(&g_ports[i]);
        }
    }
}

static void IdentifyAllPorts(void)
{
    ScanPorts();
    ProcessAllPorts();
}

static void ReadDpiRange(const BYTE *reply, DpiInfo *dpi)
{
    dpi->minDpi = 0;
    dpi->maxDpi = 0;
    for (int i = 5; i + 1 < HIDPP_LONG_LEN; i += 2) {
        int value = ReadBe16(&reply[i]);

        if (value == 0) {
            break;
        }
        if (value > 0xE000) {
            continue;
        }
        if (dpi->minDpi == 0 || value < dpi->minDpi) {
            dpi->minDpi = value;
        }
        if (value > dpi->maxDpi) {
            dpi->maxDpi = value;
        }
    }
}

static void ReadDeviceDetails(Device *device)
{
    HidppPort *port = device->port;
    BYTE index = device->index;
    BYTE reply[HIDPP_LONG_LEN];
    DeviceSlot *s;

    device->dpi.found = FALSE;
    device->profile.found = FALSE;
    device->profile.rawIndex = -1;
    device->profile.currentDpiIndex = -1;
    if (!port || !device->online) {
        return;
    }

    /* Each step stops at the first failure so an unresponsive device costs one timeout, not five. */
    s = SlotOf(device);
    if (!s->detailFeaturesKnown) {
        if (HidppFeatureIndex(port, index, HIDPP_FEATURE_ADJUSTABLE_DPI, &s->dpiFeature) != HIDPP_OK ||
            HidppFeatureIndex(port, index, HIDPP_FEATURE_ONBOARD_PROFILES, &s->profileFeature) != HIDPP_OK) {
            return;
        }
        /* getSensorDpiList (function 1) gives the range; it is read once per connection. */
        if (s->dpiFeature) {
            if (HidppRequest(port, index, s->dpiFeature, 0x01, 0, 0, reply) != HIDPP_OK) {
                return;
            }
            ReadDpiRange(reply, &device->dpi);
        }
        s->detailFeaturesKnown = TRUE;
    }

    /* getSensorDpi (function 2) for sensor 0. */
    if (s->dpiFeature) {
        if (HidppRequest(port, index, s->dpiFeature, 0x02, 0, 0, reply) != HIDPP_OK) {
            return;
        }
        device->dpi.currentDpi = ReadBe16(&reply[5]);
        device->dpi.found = device->dpi.currentDpi > 0;
    }

    /* Onboard profiles: getCurrentProfile (function 4) and getCurrentDpiIndex (function 11). */
    if (s->profileFeature) {
        if (HidppRequest(port, index, s->profileFeature, 0x04, 0, 0, reply) != HIDPP_OK) {
            return;
        }
        device->profile.rawIndex = reply[5];
        device->profile.found = TRUE;
        if (HidppRequest(port, index, s->profileFeature, 0x0B, 0, 0, reply) == HIDPP_OK) {
            device->profile.currentDpiIndex = reply[4];
        }
    }
}

static const WCHAR *BatterySourceName(USHORT featureId)
{
    return featureId == HIDPP_FEATURE_UNIFIED_BATTERY ? L"Unified battery" : L"Battery status";
}

static void DescribeBattery(const Device *device, WCHAR *buffer, size_t bufferChars)
{
    if (!device->batteryKnown) {
        (void)StringCchCopyW(buffer, bufferChars, L"Battery: unknown");
    } else if (!device->online) {
        (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d%% (offline)", device->percent);
    } else if (device->state[0]) {
        (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d%% (%s)", device->percent, device->state);
    } else {
        (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d%%", device->percent);
    }
}

static int PrintListDevices(void)
{
    HidCandidate candidates[MAX_CANDIDATES];
    int count = EnumerateHidppCandidates(candidates, MAX_CANDIDATES);

    if (count == 0) {
        wprintf(L"No Logitech HID++ collections found.\n");
        return 1;
    }

    wprintf(L"Logitech HID++ collections (no serials or USB instance IDs are shown):\n");
    for (int i = 0; i < count; ++i) {
        const HidCandidate *candidate = &candidates[i];

        wprintf(
            L"  %d. %s | VID_%04X PID_%04X | usage 0x%04X/0x%04X | reports in/out %u/%u | HID++ %s\n",
            i + 1,
            DisplayStringOrFallback(candidate->product, L"Logitech HID device"),
            LOGITECH_VID,
            candidate->pid,
            HIDPP_USAGE_PAGE,
            candidate->usage,
            candidate->inputReportBytes,
            candidate->outputReportBytes,
            candidate->inputReportBytes == HIDPP_LONG_LEN ? L"long" : L"short");
    }

    return 0;
}

static int PrintProbe(void)
{
    int ports = 0;

    IdentifyAllPorts();
    wprintf(L"HID++ probe results:\n");
    for (int i = 0; i < MAX_PORTS; ++i) {
        const HidppPort *port = &g_ports[i];
        int ready = 0;

        if (!port->used || !port->longPipe.handle) {
            continue;
        }

        ++ports;
        wprintf(
            L"  %s | VID_%04X PID_%04X | short collection: %s\n",
            DisplayStringOrFallback(port->product, L"Logitech HID device"),
            LOGITECH_VID,
            port->pid,
            port->shortPipe.handle ? L"yes" : L"no");

        for (int slot = 0; slot < HIDPP_SLOTS; ++slot) {
            const DeviceSlot *s = &port->slots[slot];

            if (s->state != SLOT_READY) {
                continue;
            }
            ++ready;
            wprintf(
                L"    device index 0x%02X | %s | battery feature: %s 0x%04X\n",
                IndexForSlot(slot),
                s->device->name,
                BatterySourceName(s->batteryFeatureId),
                s->batteryFeatureId);
        }
        if (ready == 0) {
            wprintf(L"    no device with a battery feature answered\n");
        }
    }

    if (ports == 0) {
        wprintf(L"No Logitech HID++ collections found.\n");
    }
    CloseAllPorts();
    return ports ? 0 : 1;
}

static void JsonPrintWString(const WCHAR *value)
{
    putchar('"');
    for (const WCHAR *p = value ? value : L""; *p; ++p) {
        WCHAR ch = *p;

        switch (ch) {
        case L'"':
            printf("\\\"");
            break;
        case L'\\':
            printf("\\\\");
            break;
        case L'\b':
            printf("\\b");
            break;
        case L'\f':
            printf("\\f");
            break;
        case L'\n':
            printf("\\n");
            break;
        case L'\r':
            printf("\\r");
            break;
        case L'\t':
            printf("\\t");
            break;
        default:
            if (ch < 0x20 || ch > 0x7E) {
                printf("\\u%04X", (unsigned int)ch);
            } else {
                putchar((char)ch);
            }
            break;
        }
    }
    putchar('"');
}

static void PrintJsonNullableInt(const char *name, int value)
{
    printf("\"%s\": ", name);
    if (value >= 0) {
        printf("%d", value);
    } else {
        printf("null");
    }
}

static void PrintDeviceJson(const Device *device)
{
    const DeviceSlot *s = SlotOf(device);

    printf("    {\n");
    printf("      \"battery\": {\n");
    if (device->batteryKnown) {
        printf("        \"percent\": %d,\n", device->percent);
    } else {
        printf("        \"percent\": null,\n");
    }
    printf("        \"state\": ");
    JsonPrintWString(device->state[0] ? device->state : L"Unknown");
    printf(",\n");
    printf("        \"charging\": %s,\n", device->charging ? "true" : "false");
    printf("        \"source\": \"%ls 0x%04X\"\n", BatterySourceName(s->batteryFeatureId), s->batteryFeatureId);
    printf("      },\n");

    printf("      \"device\": {\n");
    printf("        \"name\": ");
    JsonPrintWString(device->name);
    printf(",\n");
    printf("        \"hid_product\": ");
    JsonPrintWString(DisplayStringOrFallback(device->port->product, L"Logitech HID device"));
    printf(",\n");
    printf("        \"transport\": \"%s\",\n", device->index == HIDPP_DIRECT_INDEX ? "wired" : "receiver");
    printf("        \"vid\": \"VID_%04X\",\n", LOGITECH_VID);
    printf("        \"pid\": \"PID_%04X\",\n", device->port->pid);
    printf("        \"hidpp_device_index\": \"0x%02X\"\n", device->index);
    printf("      },\n");

    printf("      \"dpi\": ");
    if (device->dpi.found) {
        printf("{\n");
        printf("        \"current\": %d,\n", device->dpi.currentDpi);
        printf("        ");
        PrintJsonNullableInt("min", device->dpi.minDpi > 0 ? device->dpi.minDpi : -1);
        printf(",\n");
        printf("        ");
        PrintJsonNullableInt("max", device->dpi.maxDpi > 0 ? device->dpi.maxDpi : -1);
        printf("\n");
        printf("      },\n");
    } else {
        printf("null,\n");
    }

    printf("      \"profile\": ");
    if (device->profile.found) {
        printf("{\n");
        printf("        ");
        PrintJsonNullableInt("index", device->profile.rawIndex);
        printf(",\n");
        printf("        ");
        PrintJsonNullableInt("dpi_slot", device->profile.currentDpiIndex);
        printf("\n");
        printf("      }\n");
    } else {
        printf("null\n");
    }
    printf("    }");
}

static int PrintOnce(void)
{
    int printed = 0;

    IdentifyAllPorts();
    printf("{\n  \"devices\": [");
    for (int i = 0; i < MAX_DEVICES; ++i) {
        Device *device = &g_devices[i];

        if (!device->used || !device->online) {
            continue;
        }

        ReadDeviceDetails(device);
        printf(printed ? ",\n" : "\n");
        PrintDeviceJson(device);
        ++printed;
    }
    printf(printed ? "\n  ]\n}\n" : "]\n}\n");

    CloseAllPorts();
    return printed ? 0 : 1;
}

static void PrintHelp(const WCHAR *exeName)
{
    wprintf(L"Usage: %s [--list-devices | --probe | --once | --tray]\n", exeName);
    wprintf(L"\n");
    wprintf(L"  --list-devices  List Logitech HID++ collections without private IDs.\n");
    wprintf(L"  --probe         Probe HID++ devices and their battery feature.\n");
    wprintf(L"  --once          Print one JSON snapshot of every device and exit.\n");
    wprintf(L"  --tray          Run the tray indicator. This is the default.\n");
}

static const WCHAR *BasenameOfPath(const WCHAR *path)
{
    const WCHAR *lastSlash = wcsrchr(path, L'\\');
    const WCHAR *lastForwardSlash = wcsrchr(path, L'/');
    const WCHAR *base = path;

    if (lastSlash && lastSlash + 1 > base) {
        base = lastSlash + 1;
    }
    if (lastForwardSlash && lastForwardSlash + 1 > base) {
        base = lastForwardSlash + 1;
    }

    return base;
}

static HICON g_trayIcon = NULL;
static UINT g_taskbarCreatedMessage = 0;
static HDEVNOTIFY g_hidDeviceNotify = NULL;

#define ICON_MAX_SIZE 64

typedef struct IconTheme {
    DWORD ink;
    DWORD high;
    DWORD mid;
    DWORD low;
    DWORD charging;
} IconTheme;

/* Windows 11 system colours: ink is the outline in the taskbar's text colour. */
static const IconTheme ICON_THEME_DARK = {0xFFFFFF, 0x6CCB5F, 0xFCE100, 0xFF99A4, 0x60CDFF};
static const IconTheme ICON_THEME_LIGHT = {0x1A1A1A, 0x0F7B0F, 0x9D5D00, 0xC42B1C, 0x005FB7};

static float Clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

static void ColorChannels(DWORD color, float rgb[3])
{
    rgb[0] = ((color >> 16) & 0xFF) / 255.0f;
    rgb[1] = ((color >> 8) & 0xFF) / 255.0f;
    rgb[2] = (color & 0xFF) / 255.0f;
}

static void BlendOver(float pixel[4], const float rgb[3], float alpha)
{
    float keep = pixel[3] * (1.0f - alpha);
    float outAlpha = alpha + keep;

    if (outAlpha <= 0.0f) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        pixel[i] = (rgb[i] * alpha + pixel[i] * keep) / outAlpha;
    }
    pixel[3] = outAlpha;
}

/* Antialiased coverage of pixel (x, y) by a rounded rectangle, from its signed distance. */
static float RoundedRectCoverage(int x, int y, float left, float top, float right, float bottom, float radius)
{
    float halfWidth = (right - left) * 0.5f;
    float halfHeight = (bottom - top) * 0.5f;
    float dx = fabsf(x + 0.5f - (left + halfWidth)) - (halfWidth - radius);
    float dy = fabsf(y + 0.5f - (top + halfHeight)) - (halfHeight - radius);
    float outsideX = fmaxf(dx, 0.0f);
    float outsideY = fmaxf(dy, 0.0f);

    return Clamp01(0.5f - (sqrtf(outsideX * outsideX + outsideY * outsideY) + fminf(fmaxf(dx, dy), 0.0f) - radius));
}

static void RenderBatteryIcon(DWORD *pixels, int size, int percent, BOOL charging, BOOL lightTaskbar)
{
    const IconTheme *theme = lightTaskbar ? &ICON_THEME_LIGHT : &ICON_THEME_DARK;
    /* A standing battery on whole pixels keeps the edges sharp; only the corners are antialiased. */
    int stroke = max(1, size / 16);
    int bodyWidth = (size * 10 + 8) / 16;
    int left = (size - bodyWidth) / 2;
    int right = left + bodyWidth;
    int capWidth = bodyWidth * 2 / 5;
    int capLeft = left + (bodyWidth - capWidth) / 2;
    int bodyTop = 2 * stroke;
    int fillLeft = left + 2 * stroke;
    int fillRight = right - 2 * stroke;
    int fillBottom = size - 2 * stroke;
    int fillTop = fillBottom - ((fillBottom - bodyTop - 2 * stroke) * max(percent, 0) + 50) / 100;
    float radius = 2.0f * stroke;
    float ink[3];
    float fill[3];

    if (percent > 0 && fillTop == fillBottom) {
        --fillTop;
    }
    ColorChannels(theme->ink, ink);
    ColorChannels(charging ? theme->charging : (percent > 50 ? theme->high : (percent > 20 ? theme->mid : theme->low)), fill);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float body = RoundedRectCoverage(x, y, (float)left, (float)bodyTop, (float)right, (float)size, radius);
            float inner = RoundedRectCoverage(x, y, (float)(left + stroke), (float)(bodyTop + stroke), (float)(right - stroke), (float)(size - stroke), radius - stroke);
            BYTE alpha;

            BlendOver(pixel, ink, Clamp01(body - inner));
            if (y >= stroke && y < bodyTop && x >= capLeft && x < capLeft + capWidth) {
                BlendOver(pixel, ink, 1.0f);
            }
            if (percent >= 0 && x >= fillLeft && x < fillRight && y >= fillTop && y < fillBottom) {
                BlendOver(pixel, fill, 1.0f);
            }

            alpha = (BYTE)(pixel[3] * 255.0f + 0.5f);
            pixels[y * size + x] = ((DWORD)alpha << 24) |
                ((DWORD)(pixel[0] * alpha + 0.5f) << 16) |
                ((DWORD)(pixel[1] * alpha + 0.5f) << 8) |
                (DWORD)(pixel[2] * alpha + 0.5f);
        }
    }
}

static HICON CreateBatteryTrayIcon(int size, int percent, BOOL charging, BOOL lightTaskbar)
{
    static BYTE zeroMask[ICON_MAX_SIZE * ICON_MAX_SIZE / 8];
    BITMAPINFO bmi;
    void *bits = NULL;
    HBITMAP colorBitmap;
    HBITMAP maskBitmap;
    ICONINFO iconInfo;
    HICON icon = NULL;

    size = max(16, min(size, ICON_MAX_SIZE));

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    colorBitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!colorBitmap) {
        return NULL;
    }
    RenderBatteryIcon((DWORD *)bits, size, percent, charging, lightTaskbar);

    maskBitmap = CreateBitmap(size, size, 1, 1, zeroMask);
    if (maskBitmap) {
        ZeroMemory(&iconInfo, sizeof(iconInfo));
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = colorBitmap;
        iconInfo.hbmMask = maskBitmap;
        icon = CreateIconIndirect(&iconInfo);
        DeleteObject(maskBitmap);
    }

    DeleteObject(colorBitmap);
    return icon;
}

static BOOL TaskbarUsesLightTheme(void)
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);

    return RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        NULL,
        &value,
        &bytes) == ERROR_SUCCESS && value != 0;
}

/* The icon shows the lowest battery among connected devices, or the last known value when none is connected. */
static const Device *IconDevice(void)
{
    const Device *best = NULL;

    for (int i = 0; i < MAX_DEVICES; ++i) {
        const Device *device = &g_devices[i];

        if (!device->used || !device->batteryKnown) {
            continue;
        }
        if (!best || (device->online && !best->online) ||
            (device->online == best->online && device->percent < best->percent)) {
            best = device;
        }
    }

    return best;
}

static void DescribeTrayTip(WCHAR *tip, size_t tipChars)
{
    tip[0] = L'\0';
    for (int i = 0; i < MAX_DEVICES; ++i) {
        const Device *device = &g_devices[i];
        WCHAR line[96];

        if (!device->used) {
            continue;
        }
        if (!device->batteryKnown) {
            (void)StringCchPrintfW(line, ARRAY_LEN(line), L"%s: unknown", device->name);
        } else {
            (void)StringCchPrintfW(
                line,
                ARRAY_LEN(line),
                L"%s: %d%% %s",
                device->name,
                device->percent,
                device->online ? device->state : L"(offline)");
        }
        /* The tooltip holds 127 characters; leave out whole lines rather than cut one off. */
        if (wcslen(tip) + (tip[0] ? 1 : 0) + wcslen(line) >= tipChars) {
            break;
        }
        if (tip[0]) {
            (void)StringCchCatW(tip, tipChars, L"\n");
        }
        (void)StringCchCatW(tip, tipChars, line);
    }

    if (!tip[0]) {
        (void)StringCchCopyW(tip, tipChars, L"SuperLightBattery: no device");
    }
}

static void InitTrayData(NOTIFYICONDATAW *tray, HWND hwnd)
{
    ZeroMemory(tray, sizeof(*tray));
    tray->cbSize = sizeof(*tray);
    tray->hWnd = hwnd;
    tray->uID = TRAY_UID;
}

static void UpdateTray(HWND hwnd)
{
    NOTIFYICONDATAW tray;
    const Device *device = IconDevice();
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd));
    HICON icon = CreateBatteryTrayIcon(size, device ? device->percent : -1, device && device->charging, TaskbarUsesLightTheme());

    InitTrayData(&tray, hwnd);
    /* Without a new icon the shell keeps showing the previous one. */
    tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | (icon ? NIF_ICON : 0);
    tray.uCallbackMessage = WM_TRAYICON;
    tray.hIcon = icon;
    DescribeTrayTip(tray.szTip, ARRAY_LEN(tray.szTip));
    if (!Shell_NotifyIconW(NIM_MODIFY, &tray) && Shell_NotifyIconW(NIM_ADD, &tray)) {
        /* Version 4 sends WM_CONTEXTMENU, which also opens the menu from the keyboard; NIF_SHOWTIP keeps the tooltip. */
        tray.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &tray);
    }

    if (icon) {
        if (g_trayIcon) {
            DestroyIcon(g_trayIcon);
        }
        g_trayIcon = icon;
    }
}

static void RemoveTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW tray;

    InitTrayData(&tray, hwnd);
    (void)Shell_NotifyIconW(NIM_DELETE, &tray);
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = NULL;
    }
}

static void RegisterHidDeviceNotifications(HWND hwnd)
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter;

    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    g_hidDeviceNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static void UnregisterHidDeviceNotifications(void)
{
    if (g_hidDeviceNotify) {
        UnregisterDeviceNotification(g_hidDeviceNotify);
        g_hidDeviceNotify = NULL;
    }
}

static void RequestFullRefresh(void)
{
    for (int i = 0; i < MAX_PORTS; ++i) {
        if (g_ports[i].used) {
            g_ports[i].pendingSlots = HIDPP_ALL_SLOTS;
        }
    }
    g_rescanPending = TRUE;
}

static void ShowTrayMenu(HWND hwnd, POINT anchor)
{
    HMENU menu;
    WCHAR line[128];
    BOOL any = FALSE;

    /* The mouse is awake when it right-clicks; anything slower than this keeps its last values. */
    g_replyTimeoutMs = HIDPP_MENU_TIMEOUT_MS;
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i].used && g_devices[i].online) {
            if (ReadBattery(&g_devices[i]) == HIDPP_OK) {
                ReadDeviceDetails(&g_devices[i]);
            } else {
                g_devices[i].dpi.found = FALSE;
                g_devices[i].profile.found = FALSE;
            }
        }
    }
    g_replyTimeoutMs = HIDPP_REPLY_TIMEOUT_MS;

    menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    for (int i = 0; i < MAX_DEVICES; ++i) {
        const Device *device = &g_devices[i];

        if (!device->used) {
            continue;
        }
        if (any) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        }
        any = TRUE;

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, device->name);
        DescribeBattery(device, line, ARRAY_LEN(line));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);

        if (device->dpi.found && device->dpi.maxDpi > 0) {
            (void)StringCchPrintfW(
                line,
                ARRAY_LEN(line),
                L"DPI: %d (%d-%d)",
                device->dpi.currentDpi,
                device->dpi.minDpi,
                device->dpi.maxDpi);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
        } else if (device->dpi.found) {
            (void)StringCchPrintfW(line, ARRAY_LEN(line), L"DPI: %d", device->dpi.currentDpi);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
        }

        if (device->profile.found) {
            (void)StringCchPrintfW(
                line,
                ARRAY_LEN(line),
                L"Profile: %d, DPI slot: %d",
                device->profile.rawIndex,
                device->profile.currentDpiIndex);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
        }
    }

    if (!any) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Battery: unavailable");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TRAY_MENU_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, TRAY_MENU_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, anchor.x, anchor.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void RunPendingWork(HWND hwnd)
{
    CloseDeadPorts();
    if (g_rescanPending) {
        g_rescanPending = FALSE;
        ScanPorts();
    }
    ProcessAllPorts();
    CloseDeadPorts();

    if (g_trayDirty) {
        g_trayDirty = FALSE;
        UpdateTray(hwnd);
    }
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_taskbarCreatedMessage != 0 && msg == g_taskbarCreatedMessage) {
        g_trayDirty = TRUE;
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_trayWindow = hwnd;
        RegisterHidDeviceNotifications(hwnd);
        UpdateTray(hwnd);
        g_rescanPending = TRUE;
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_CONTEXTMENU) {
            /* wParam carries the click point, or the icon's position when opened from the keyboard. */
            POINT anchor = {(short)LOWORD(wParam), (short)HIWORD(wParam)};

            ShowTrayMenu(hwnd, anchor);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case TRAY_MENU_REFRESH:
            RequestFullRefresh();
            return 0;
        case TRAY_MENU_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL) {
            const DEV_BROADCAST_DEVICEINTERFACE_W *broadcast = (const DEV_BROADCAST_DEVICEINTERFACE_W *)lParam;

            if (broadcast && broadcast->dbcc_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
                ContainsNoCase(broadcast->dbcc_name, L"VID_046D")) {
                g_rescanPending = TRUE;
            }
        } else if (wParam == DBT_DEVICEQUERYREMOVE) {
            const DEV_BROADCAST_HANDLE *broadcast = (const DEV_BROADCAST_HANDLE *)lParam;

            if (broadcast && broadcast->dbch_devicetype == DBT_DEVTYP_HANDLE) {
                /* Release the device so it can be disabled or updated; look again later in case the removal is cancelled. */
                ClosePortWithHandle(broadcast->dbch_handle);
                SetTimer(hwnd, TRAY_RESCAN_TIMER, TRAY_RESCAN_DELAY_MS, NULL);
            }
        }
        break;

    case WM_TIMER:
        if (wParam == TRAY_RESCAN_TIMER) {
            KillTimer(hwnd, TRAY_RESCAN_TIMER);
            g_rescanPending = TRUE;
        }
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        /* Theme, scaling or display changes: redraw at the current size and colours. Redrawing is cheap and rare. */
        g_trayDirty = TRUE;
        return 0;

    case WM_DESTROY:
        UnregisterHidDeviceNotifications();
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Sleeps until a window message arrives or a device sends a report; there is no polling. */
static int RunTrayLoop(HWND hwnd)
{
    for (;;) {
        HANDLE events[MAX_PORTS * 2];
        HidppPort *ports[MAX_PORTS * 2];
        HidPipe *pipes[MAX_PORTS * 2];
        DWORD count = 0;
        DWORD wait;

        RunPendingWork(hwnd);
        for (int i = 0; i < MAX_PORTS; ++i) {
            HidPipe *portPipes[2] = {&g_ports[i].longPipe, &g_ports[i].shortPipe};

            for (int j = 0; j < 2; ++j) {
                if (g_ports[i].used && portPipes[j]->pending) {
                    ports[count] = &g_ports[i];
                    pipes[count] = portPipes[j];
                    events[count++] = portPipes[j]->event;
                }
            }
        }

        wait = MsgWaitForMultipleObjectsEx(count, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait < WAIT_OBJECT_0 + count) {
            HidppPort *port = ports[wait - WAIT_OBJECT_0];
            HidPipe *pipe = pipes[wait - WAIT_OBJECT_0];
            DWORD bytes;

            if (CompletePipeRead(port, pipe, &bytes)) {
                HandleReport(port, pipe->report, bytes);
                RearmPipe(port, pipe);
            }
        } else if (wait == WAIT_OBJECT_0 + count) {
            MSG msg;

            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    return (int)msg.wParam;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else {
            return 1;
        }
    }
}

static int RunTray(HINSTANCE instance)
{
    const WCHAR className[] = L"SuperLightBatteryTrayWindow";
    HANDLE mutex;
    DWORD mutexStatus;
    WNDCLASSW wc;
    HWND hwnd;
    int result;

    mutex = CreateMutexW(NULL, TRUE, L"Local\\SuperLightBatteryTray");
    if (!mutex) {
        return 1;
    }
    mutexStatus = GetLastError();
    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        DWORD waitResult = WaitForSingleObject(mutex, 0);

        if (waitResult == WAIT_TIMEOUT) {
            CloseHandle(mutex);
            return 0;
        }
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return 1;
        }
    }

    g_swId = HIDPP_TRAY_SW_ID;
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 1;
    }

    hwnd = CreateWindowExW(0, className, L"SuperLightBattery", 0, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!hwnd) {
        CloseHandle(mutex);
        return 1;
    }

    result = RunTrayLoop(hwnd);
    CloseAllPorts();
    CloseHandle(mutex);
    return result;
}

static int RunApp(int argc, WCHAR **argv)
{
    if (argc > 1) {
        if (wcscmp(argv[1], L"--list-devices") == 0) {
            return PrintListDevices();
        }
        if (wcscmp(argv[1], L"--probe") == 0) {
            return PrintProbe();
        }
        if (wcscmp(argv[1], L"--once") == 0) {
            return PrintOnce();
        }
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"/?") == 0) {
            PrintHelp(BasenameOfPath(argv[0]));
            return 0;
        }
        if (wcscmp(argv[1], L"--tray") != 0) {
            fwprintf(stderr, L"Unknown option: %s\n\n", argv[1]);
            PrintHelp(BasenameOfPath(argv[0]));
            return 2;
        }
    }

    FreeConsole();
    return RunTray(GetModuleHandleW(NULL));
}

int wmain(int argc, WCHAR **argv)
{
    return RunApp(argc, argv);
}
