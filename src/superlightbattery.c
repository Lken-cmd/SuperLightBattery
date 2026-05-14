#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dbt.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <shellapi.h>
#include <strsafe.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
typedef int (WINAPI *GetSystemMetricsForDpiFn)(int, UINT);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);

#define LOGITECH_VID 0x046D
#define HIDPP_REPORT_SHORT 0x10
#define HIDPP_REPORT_LONG 0x11
#define HIDPP_SHORT_LEN 7
#define HIDPP_LONG_LEN 20
#define HIDPP_SW_ID 0x01
#define HIDPP10_GET_REGISTER 0x81
#define HIDPP10_BATTERY_REGISTER 0x07

#define HIDPP_FEATURE_FEATURE_SET 0x0001
#define HIDPP_FEATURE_DEVICE_NAME_TYPE 0x0005
#define HIDPP_FEATURE_ADJUSTABLE_DPI 0x2201
#define HIDPP_FEATURE_ONBOARD_PROFILES 0x8100

#define HIDPP_DEVICE_NAME_MAX 160

#define MAX_HID_DEVICES 16
#define MAX_HID_PATH_CHARS 512
#define HID_IO_BUFFER_LEN 256
#define HIDPP_READ_TIMEOUT_MS 250
#define HIDPP_CANCEL_WAIT_MS 100
#define HIDPP_READ_ATTEMPTS 8

#define WM_TRAYICON (WM_APP + 1)
#define WM_REFRESH_COMPLETE (WM_APP + 2)
#define TRAY_UID 1
#define TRAY_MENU_BATTERY 1001
#define TRAY_MENU_REFRESH 1002
#define TRAY_MENU_EXIT 1003
#define HOVER_REFRESH_COOLDOWN_MS 60000
#define HID_QUERY_LOCK_TIMEOUT_MS 15000

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef enum TransportKind {
    TRANSPORT_UNKNOWN = 0,
    TRANSPORT_RECEIVER,
    TRANSPORT_WIRED
} TransportKind;

typedef enum BatteryConfidence {
    BATTERY_CONFIDENCE_NONE = 0,
    BATTERY_CONFIDENCE_PERCENT,
    BATTERY_CONFIDENCE_COARSE,
    BATTERY_CONFIDENCE_VOLTAGE
} BatteryConfidence;

typedef struct HidCandidate {
    WCHAR path[MAX_HID_PATH_CHARS];
    WCHAR product[160];
    USHORT vid;
    USHORT pid;
    USHORT usagePage;
    USHORT usage;
    USHORT inputReportBytes;
    USHORT outputReportBytes;
    USHORT featureReportBytes;
    BOOL vendorDefined;
    BOOL likelyHidpp;
    TransportKind transport;
} HidCandidate;

typedef struct HidppIo {
    HANDLE handle;
    DWORD inputReportBytes;
    DWORD outputReportBytes;
} HidppIo;

typedef struct HidOverlappedOp {
    OVERLAPPED overlapped;
    HANDLE event;
    struct HidOverlappedOp *next;
} HidOverlappedOp;

typedef struct ProbeResult {
    BOOL opened;
    BOOL hidpp;
    BOOL hasBatteryFeature;
    BYTE deviceIndex;
    WCHAR featureName[48];
} ProbeResult;

typedef struct BatteryResult {
    BOOL found;
    BatteryConfidence confidence;
    TransportKind transport;
    BYTE deviceIndex;
    USHORT vid;
    USHORT pid;
    WCHAR product[160];
    WCHAR source[48];
    int percent;
    int millivolts;
    WCHAR state[32];
    BOOL charging;
} BatteryResult;

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

typedef struct DeviceDetails {
    BOOL firmwareNameFound;
    WCHAR firmwareName[HIDPP_DEVICE_NAME_MAX];
    DpiInfo dpi;
    ProfileInfo profile;
} DeviceDetails;

typedef struct DeviceSnapshot {
    BatteryResult battery;
    HidCandidate device;
    DeviceDetails details;
} DeviceSnapshot;

static const WCHAR *TransportName(TransportKind transport)
{
    switch (transport) {
    case TRANSPORT_RECEIVER:
        return L"receiver";
    case TRANSPORT_WIRED:
        return L"wired";
    default:
        return L"unknown";
    }
}

static BOOL ContainsNoCase(const WCHAR *haystack, const WCHAR *needle)
{
    if (!haystack || !needle) {
        return FALSE;
    }

    return FindStringOrdinal(FIND_FROMSTART, haystack, -1, needle, -1, TRUE) >= 0;
}

static BOOL MultiSzContainsNoCase(const WCHAR *multiSz, DWORD bytes, const WCHAR *needle)
{
    size_t chars = bytes / sizeof(WCHAR);
    size_t offset = 0;

    while (offset < chars && multiSz[offset]) {
        size_t start = offset;

        while (offset < chars && multiSz[offset]) {
            ++offset;
        }
        if (offset < chars && ContainsNoCase(&multiSz[start], needle)) {
            return TRUE;
        }
        ++offset;
    }

    return FALSE;
}

static BOOL DeviceNodeMayBeLogitech(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA *devInfoData)
{
    WCHAR stackIds[512];
    WCHAR *heapIds = NULL;
    WCHAR *ids = stackIds;
    DWORD idsBytes = sizeof(stackIds);
    DWORD requiredBytes = 0;
    DWORD propertyType = 0;
    BOOL ok;
    BOOL result = TRUE;

    ok = SetupDiGetDeviceRegistryPropertyW(
        deviceInfoSet,
        devInfoData,
        SPDRP_HARDWAREID,
        &propertyType,
        (PBYTE)ids,
        idsBytes,
        &requiredBytes);

    if (!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER && requiredBytes > 0) {
        heapIds = (WCHAR *)malloc((size_t)requiredBytes + sizeof(WCHAR));
        if (!heapIds) {
            return TRUE;
        }

        ids = heapIds;
        idsBytes = requiredBytes + sizeof(WCHAR);
        ZeroMemory(ids, idsBytes);
        ok = SetupDiGetDeviceRegistryPropertyW(
            deviceInfoSet,
            devInfoData,
            SPDRP_HARDWAREID,
            &propertyType,
            (PBYTE)ids,
            requiredBytes,
            &requiredBytes);
    }

    if (ok && (propertyType == REG_MULTI_SZ || propertyType == REG_SZ)) {
        result = MultiSzContainsNoCase(ids, requiredBytes, L"VID_046D");
    }

    free(heapIds);
    return result;
}

static TransportKind GuessTransport(const HidCandidate *device)
{
    if (ContainsNoCase(device->product, L"receiver") ||
        ContainsNoCase(device->product, L"lightspeed") ||
        device->pid == 0xC539 ||
        device->pid == 0xC53A ||
        device->pid == 0xC547 ||
        device->pid == 0xC548 ||
        device->pid == 0xC52B) {
        return TRANSPORT_RECEIVER;
    }

    if (device->vendorDefined) {
        return TRANSPORT_WIRED;
    }

    return TRANSPORT_UNKNOWN;
}

static const WCHAR *DisplayStringOrFallback(const WCHAR *value, const WCHAR *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static HANDLE g_hidQueryMutex = NULL;

static BOOL AcquireHidQueryLock(void)
{
    DWORD waitResult;

    if (!g_hidQueryMutex) {
        g_hidQueryMutex = CreateMutexW(NULL, FALSE, L"Local\\SuperLightBatteryHidQuery");
        if (!g_hidQueryMutex) {
            return FALSE;
        }
    }

    waitResult = WaitForSingleObject(g_hidQueryMutex, HID_QUERY_LOCK_TIMEOUT_MS);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        return FALSE;
    }

    return TRUE;
}

static void ReleaseHidQueryLock(void)
{
    if (g_hidQueryMutex) {
        ReleaseMutex(g_hidQueryMutex);
    }
}

static void CloseHidQueryLockHandle(void)
{
    if (g_hidQueryMutex) {
        CloseHandle(g_hidQueryMutex);
        g_hidQueryMutex = NULL;
    }
}

static HidCandidate g_cachedDevice = {0};
static BOOL g_cachedDeviceValid = FALSE;
static SRWLOCK g_deviceCacheLock = SRWLOCK_INIT;

static void InvalidateDeviceCache(void)
{
    AcquireSRWLockExclusive(&g_deviceCacheLock);
    ZeroMemory(&g_cachedDevice, sizeof(g_cachedDevice));
    g_cachedDeviceValid = FALSE;
    ReleaseSRWLockExclusive(&g_deviceCacheLock);
}

static BOOL CopyCachedDevice(HidCandidate *device)
{
    BOOL valid;

    AcquireSRWLockShared(&g_deviceCacheLock);
    valid = g_cachedDeviceValid;
    if (valid) {
        *device = g_cachedDevice;
    }
    ReleaseSRWLockShared(&g_deviceCacheLock);
    return valid;
}

static void StoreCachedDevice(const HidCandidate *device)
{
    AcquireSRWLockExclusive(&g_deviceCacheLock);
    g_cachedDevice = *device;
    g_cachedDeviceValid = TRUE;
    ReleaseSRWLockExclusive(&g_deviceCacheLock);
}

static void CopyHidString(WCHAR *dst, size_t dstChars, HANDLE handle, BOOL product)
{
    if (dstChars == 0) {
        return;
    }

    dst[0] = L'\0';
    if (product) {
        (void)HidD_GetProductString(handle, dst, (ULONG)(dstChars * sizeof(WCHAR)));
    } else {
        (void)HidD_GetManufacturerString(handle, dst, (ULONG)(dstChars * sizeof(WCHAR)));
    }
    dst[dstChars - 1] = L'\0';
}

static int EnumerateLogitechHidDevices(HidCandidate *devices, int maxDevices)
{
    GUID hidGuid;
    HDEVINFO deviceInfoSet;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = NULL;
    DWORD detailDataBytes = 0;
    int count = 0;

    HidD_GetHidGuid(&hidGuid);
    deviceInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return 0;
    }

    for (DWORD index = 0; count < maxDevices; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData;
        DWORD requiredSize = 0;
        SP_DEVINFO_DATA devInfoData;
        HANDLE handle = INVALID_HANDLE_VALUE;
        HIDD_ATTRIBUTES attributes;
        PHIDP_PREPARSED_DATA preparsedData = NULL;
        HIDP_CAPS caps;
        HidCandidate candidate;

        ZeroMemory(&interfaceData, sizeof(interfaceData));
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, index, &interfaceData)) {
            break;
        }

        (void)SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, NULL, 0, &requiredSize, NULL);
        if (requiredSize == 0) {
            continue;
        }

        if (requiredSize > detailDataBytes) {
            void *newDetailData = realloc(detailData, requiredSize);
            if (!newDetailData) {
                break;
            }
            detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)newDetailData;
            detailDataBytes = requiredSize;
        }

        ZeroMemory(detailData, detailDataBytes);
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        ZeroMemory(&devInfoData, sizeof(devInfoData));
        devInfoData.cbSize = sizeof(devInfoData);

        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, detailData, requiredSize, NULL, &devInfoData)) {
            continue;
        }

        if (!DeviceNodeMayBeLogitech(deviceInfoSet, &devInfoData)) {
            continue;
        }

        handle = CreateFileW(
            detailData->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        ZeroMemory(&attributes, sizeof(attributes));
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(handle, &attributes) || attributes.VendorID != LOGITECH_VID) {
            CloseHandle(handle);
            continue;
        }

        ZeroMemory(&candidate, sizeof(candidate));
        candidate.vid = attributes.VendorID;
        candidate.pid = attributes.ProductID;
        if (FAILED(StringCchCopyW(candidate.path, ARRAY_LEN(candidate.path), detailData->DevicePath))) {
            CloseHandle(handle);
            continue;
        }
        CopyHidString(candidate.product, ARRAY_LEN(candidate.product), handle, TRUE);

        if (HidD_GetPreparsedData(handle, &preparsedData)) {
            if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS) {
                candidate.usagePage = caps.UsagePage;
                candidate.usage = caps.Usage;
                candidate.inputReportBytes = caps.InputReportByteLength;
                candidate.outputReportBytes = caps.OutputReportByteLength;
                candidate.featureReportBytes = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsedData);
        }

        candidate.vendorDefined = (candidate.usagePage >= 0xFF00);
        candidate.likelyHidpp =
            candidate.vendorDefined &&
            candidate.inputReportBytes >= HIDPP_SHORT_LEN &&
            (candidate.outputReportBytes >= HIDPP_SHORT_LEN || candidate.featureReportBytes >= HIDPP_SHORT_LEN);
        candidate.transport = GuessTransport(&candidate);

        devices[count++] = candidate;

        CloseHandle(handle);
    }

    free(detailData);
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return count;
}

static BOOL OpenHidppIo(const HidCandidate *device, HidppIo *io)
{
    HANDLE handle = CreateFileW(
        device->path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);

    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    io->handle = handle;
    io->inputReportBytes = device->inputReportBytes;
    io->outputReportBytes = device->outputReportBytes;
    return TRUE;
}

static void CloseHidppIo(HidppIo *io)
{
    if (io->handle && io->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(io->handle);
    }
    io->handle = INVALID_HANDLE_VALUE;
}

static HidOverlappedOp *g_abandonedHidIo = NULL;
static SRWLOCK g_abandonedHidIoLock = SRWLOCK_INIT;

static void DestroyHidOverlappedOp(HidOverlappedOp *op)
{
    if (!op) {
        return;
    }

    if (op->event) {
        CloseHandle(op->event);
    }
    free(op);
}

static void ReapAbandonedHidIo(void)
{
    HidOverlappedOp **link;

    AcquireSRWLockExclusive(&g_abandonedHidIoLock);
    link = &g_abandonedHidIo;
    while (*link) {
        HidOverlappedOp *op = *link;
        DWORD wait = WaitForSingleObject(op->event, 0);

        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
            *link = op->next;
            DestroyHidOverlappedOp(op);
        } else {
            link = &op->next;
        }
    }
    ReleaseSRWLockExclusive(&g_abandonedHidIoLock);
}

static HidOverlappedOp *CreateHidOverlappedOp(void)
{
    HidOverlappedOp *op;

    ReapAbandonedHidIo();
    op = (HidOverlappedOp *)calloc(1, sizeof(*op));
    if (!op) {
        return NULL;
    }

    op->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!op->event) {
        free(op);
        return NULL;
    }

    op->overlapped.hEvent = op->event;
    return op;
}

static void AbandonHidOverlappedOp(HidOverlappedOp *op)
{
    AcquireSRWLockExclusive(&g_abandonedHidIoLock);
    op->next = g_abandonedHidIo;
    g_abandonedHidIo = op;
    ReleaseSRWLockExclusive(&g_abandonedHidIoLock);
}

static void CancelAndReleaseHidOverlapped(HANDLE handle, HidOverlappedOp *op)
{
    DWORD transferred = 0;

    if (!CancelIoEx(handle, &op->overlapped) && GetLastError() == ERROR_NOT_FOUND) {
        DestroyHidOverlappedOp(op);
        return;
    }

    if (WaitForSingleObject(op->event, HIDPP_CANCEL_WAIT_MS) == WAIT_OBJECT_0) {
        (void)GetOverlappedResult(handle, &op->overlapped, &transferred, FALSE);
        DestroyHidOverlappedOp(op);
        return;
    }

    AbandonHidOverlappedOp(op);
}

static BOOL OverlappedWriteWithTimeout(HANDLE handle, const BYTE *buffer, DWORD length, DWORD timeoutMs)
{
    HidOverlappedOp *op = CreateHidOverlappedOp();
    DWORD transferred = 0;
    BOOL ok;

    if (!op) {
        return FALSE;
    }

    ok = WriteFile(handle, buffer, length, NULL, &op->overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(op->event, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &op->overlapped, &transferred, FALSE);
        } else {
            CancelAndReleaseHidOverlapped(handle, op);
            return FALSE;
        }
    } else if (ok) {
        ok = GetOverlappedResult(handle, &op->overlapped, &transferred, FALSE);
    }

    DestroyHidOverlappedOp(op);
    return ok && transferred == length;
}

static BOOL OverlappedReadWithTimeout(HANDLE handle, BYTE *buffer, DWORD length, DWORD timeoutMs, DWORD *bytesRead)
{
    HidOverlappedOp *op = CreateHidOverlappedOp();
    DWORD transferred = 0;
    BOOL ok;

    if (!op) {
        return FALSE;
    }

    *bytesRead = 0;
    ok = ReadFile(handle, buffer, length, NULL, &op->overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(op->event, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(handle, &op->overlapped, &transferred, FALSE);
        } else {
            CancelAndReleaseHidOverlapped(handle, op);
            return FALSE;
        }
    } else if (ok) {
        ok = GetOverlappedResult(handle, &op->overlapped, &transferred, FALSE);
    }

    if (ok) {
        *bytesRead = transferred;
    }
    DestroyHidOverlappedOp(op);
    return ok;
}

static BOOL IsHidppErrorResponse(const BYTE *response, DWORD length)
{
    if (length < HIDPP_SHORT_LEN) {
        return FALSE;
    }

    return response[2] == 0x8F || response[2] == 0xFF;
}

static BOOL HidppCall(
    HidppIo *io,
    BYTE deviceIndex,
    BYTE featureIndex,
    BYTE functionId,
    const BYTE params[3],
    BYTE response[HID_IO_BUFFER_LEN])
{
    BYTE frame[HIDPP_SHORT_LEN] = {0};
    BYTE output[HID_IO_BUFFER_LEN];
    BYTE input[HID_IO_BUFFER_LEN];
    DWORD outputLen = io->outputReportBytes;
    BYTE reportId = (io->outputReportBytes >= HIDPP_LONG_LEN && io->inputReportBytes >= HIDPP_LONG_LEN)
        ? HIDPP_REPORT_LONG
        : HIDPP_REPORT_SHORT;

    if (outputLen < HIDPP_SHORT_LEN || outputLen > HID_IO_BUFFER_LEN ||
        io->inputReportBytes < HIDPP_SHORT_LEN || io->inputReportBytes > HID_IO_BUFFER_LEN) {
        return FALSE;
    }

    frame[0] = reportId;
    frame[1] = deviceIndex;
    frame[2] = featureIndex;
    frame[3] = (BYTE)((functionId << 4) | HIDPP_SW_ID);
    if (params) {
        frame[4] = params[0];
        frame[5] = params[1];
        frame[6] = params[2];
    }

    ZeroMemory(output, outputLen);
    CopyMemory(output, frame, sizeof(frame));

    (void)HidD_FlushQueue(io->handle);
    if (!OverlappedWriteWithTimeout(io->handle, output, outputLen, HIDPP_READ_TIMEOUT_MS)) {
        return FALSE;
    }

    for (int attempt = 0; attempt < HIDPP_READ_ATTEMPTS; ++attempt) {
        DWORD bytesRead = 0;

        if (!OverlappedReadWithTimeout(io->handle, input, io->inputReportBytes, HIDPP_READ_TIMEOUT_MS, &bytesRead)) {
            return FALSE;
        }

        if (bytesRead < HIDPP_SHORT_LEN || input[0] != reportId || input[1] != deviceIndex) {
            continue;
        }

        if (IsHidppErrorResponse(input, bytesRead)) {
            return FALSE;
        }

        if (input[2] == featureIndex && (input[3] & 0xF0) == (frame[3] & 0xF0)) {
            CopyMemory(response, input, bytesRead);
            if (bytesRead < HID_IO_BUFFER_LEN) {
                ZeroMemory(response + bytesRead, HID_IO_BUFFER_LEN - bytesRead);
            }
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL HidppGetFeatureIndex(HidppIo *io, BYTE deviceIndex, USHORT featureId, BYTE *featureIndex)
{
    BYTE params[3];
    BYTE response[HID_IO_BUFFER_LEN];

    params[0] = (BYTE)((featureId >> 8) & 0xFF);
    params[1] = (BYTE)(featureId & 0xFF);
    params[2] = 0;

    if (!HidppCall(io, deviceIndex, 0x00, 0x00, params, response)) {
        return FALSE;
    }

    if (response[4] == 0 || response[4] == 0xFF) {
        return FALSE;
    }

    *featureIndex = response[4];
    return TRUE;
}

static int ReadBe16(const BYTE *bytes)
{
    return ((int)bytes[0] << 8) | bytes[1];
}

static BOOL IsPrintableAscii(BYTE value)
{
    return value >= 0x20 && value <= 0x7E;
}

static BOOL QueryFirmwareDeviceName(HidppIo *io, BYTE deviceIndex, DeviceDetails *details)
{
    BYTE featureIndex;
    BYTE response[HID_IO_BUFFER_LEN];
    BYTE params[3] = {0, 0, 0};
    int nameLen;
    int copied = 0;

    if (!HidppGetFeatureIndex(io, deviceIndex, HIDPP_FEATURE_DEVICE_NAME_TYPE, &featureIndex)) {
        return FALSE;
    }

    if (!HidppCall(io, deviceIndex, featureIndex, 0x00, params, response)) {
        return FALSE;
    }

    nameLen = response[4];
    if (nameLen <= 0 || nameLen >= HIDPP_DEVICE_NAME_MAX) {
        return FALSE;
    }

    ZeroMemory(details->firmwareName, sizeof(details->firmwareName));
    while (copied < nameLen && copied < HIDPP_DEVICE_NAME_MAX - 1) {
        int chunkOffset = copied;

        params[0] = (BYTE)chunkOffset;
        params[1] = 0;
        params[2] = 0;
        if (!HidppCall(io, deviceIndex, featureIndex, 0x01, params, response)) {
            break;
        }

        for (int i = 4; i < 20 && copied < nameLen && copied < HIDPP_DEVICE_NAME_MAX - 1; ++i) {
            BYTE ch = response[i];

            if (ch == 0) {
                break;
            }
            details->firmwareName[copied++] = IsPrintableAscii(ch) ? (WCHAR)ch : L'?';
        }

        if (copied == chunkOffset) {
            break;
        }
    }

    if (copied <= 0) {
        details->firmwareName[0] = L'\0';
        return FALSE;
    }

    details->firmwareName[copied] = L'\0';
    details->firmwareNameFound = TRUE;

    return TRUE;
}

static BOOL QueryDpiInfo(HidppIo *io, BYTE deviceIndex, DpiInfo *dpi)
{
    BYTE featureIndex;
    BYTE response[HID_IO_BUFFER_LEN];
    BYTE params[3] = {0, 0, 0};
    int sensorCount;

    if (!HidppGetFeatureIndex(io, deviceIndex, HIDPP_FEATURE_ADJUSTABLE_DPI, &featureIndex)) {
        return FALSE;
    }

    if (!HidppCall(io, deviceIndex, featureIndex, 0x00, params, response)) {
        return FALSE;
    }

    sensorCount = response[4];
    if (sensorCount <= 0 || sensorCount > 16) {
        return FALSE;
    }

    params[0] = 0;
    params[1] = 0;
    params[2] = 0;
    if (HidppCall(io, deviceIndex, featureIndex, 0x01, params, response)) {
        int minDpi = 0;
        int maxDpi = 0;

        for (int i = 5; i + 1 < 20; i += 2) {
            int value = ReadBe16(&response[i]);

            if (value == 0) {
                break;
            }

            if (value > 0xE000) {
                continue;
            } else {
                if (minDpi == 0 || value < minDpi) {
                    minDpi = value;
                }
                if (value > maxDpi) {
                    maxDpi = value;
                }
            }
        }

        dpi->minDpi = minDpi;
        dpi->maxDpi = maxDpi;
    }

    if (!HidppCall(io, deviceIndex, featureIndex, 0x02, params, response)) {
        return FALSE;
    }

    dpi->currentDpi = ReadBe16(&response[5]);
    dpi->found = dpi->currentDpi > 0;

    return dpi->found;
}

static BOOL QueryProfileInfo(HidppIo *io, BYTE deviceIndex, ProfileInfo *profile)
{
    BYTE featureIndex;
    BYTE response[HID_IO_BUFFER_LEN];
    BYTE params[3] = {0, 0, 0};
    BOOL gotAny = FALSE;

    if (!HidppGetFeatureIndex(io, deviceIndex, HIDPP_FEATURE_ONBOARD_PROFILES, &featureIndex)) {
        return FALSE;
    }

    profile->rawIndex = -1;
    profile->currentDpiIndex = -1;
    if (HidppCall(io, deviceIndex, featureIndex, 0x04, params, response)) {
        profile->rawIndex = response[5];
        gotAny = TRUE;
    }

    if (HidppCall(io, deviceIndex, featureIndex, 0x0B, params, response)) {
        profile->currentDpiIndex = response[4];
        gotAny = TRUE;
    }

    profile->found = gotAny;
    return gotAny;
}

static void QueryDeviceDetails(HidppIo *io, BYTE deviceIndex, DeviceDetails *details)
{
    ZeroMemory(details, sizeof(*details));
    details->profile.rawIndex = -1;
    details->profile.currentDpiIndex = -1;

    (void)QueryFirmwareDeviceName(io, deviceIndex, details);
    (void)QueryDpiInfo(io, deviceIndex, &details->dpi);
    (void)QueryProfileInfo(io, deviceIndex, &details->profile);
}

static void SetBatteryState(BatteryResult *result, const WCHAR *state, BOOL charging)
{
    (void)StringCchCopyW(result->state, ARRAY_LEN(result->state), state);
    result->charging = charging;
}

static BOOL SetBatteryStateFromStatus(BatteryResult *result, BYTE status)
{
    switch (status) {
    case 0x00:
        SetBatteryState(result, L"Discharging", FALSE);
        return TRUE;
    case 0x01:
        SetBatteryState(result, L"Charging", TRUE);
        return TRUE;
    case 0x02:
        SetBatteryState(result, L"Charging", TRUE);
        return TRUE;
    case 0x03:
        SetBatteryState(result, L"Full", FALSE);
        return TRUE;
    case 0x04:
        SetBatteryState(result, L"Charging slowly", TRUE);
        return TRUE;
    default:
        return FALSE;
    }
}

static const WCHAR *CoarseLevelFromPercent(int percent)
{
    if (percent >= 90) {
        return L"Full";
    }
    if (percent >= 20) {
        return L"Good";
    }
    if (percent >= 10) {
        return L"Low";
    }
    return L"Critical";
}

static const WCHAR *CoarseLevelFromEnum(BYTE level)
{
    switch (level) {
    case 1:
        return L"Critical";
    case 2:
        return L"Low";
    case 3:
        return L"Good";
    case 4:
        return L"Full";
    default:
        return NULL;
    }
}

static BOOL ParsePercentBattery(
    BatteryResult *result,
    const HidCandidate *device,
    BYTE deviceIndex,
    USHORT featureId,
    const WCHAR *sourceName,
    const BYTE response[HID_IO_BUFFER_LEN])
{
    BYTE percent = response[4];
    BYTE status = response[6];

    if (percent > 100) {
        return FALSE;
    }

    result->found = TRUE;
    result->confidence = BATTERY_CONFIDENCE_PERCENT;
    result->transport = device->transport;
    result->deviceIndex = deviceIndex;
    result->vid = device->vid;
    result->pid = device->pid;
    result->percent = percent;
    (void)StringCchCopyW(result->product, ARRAY_LEN(result->product), DisplayStringOrFallback(device->product, L"Logitech HID device"));
    (void)StringCchPrintfW(result->source, ARRAY_LEN(result->source), L"%s 0x%04X", sourceName, featureId);
    (void)StringCchCopyW(result->state, ARRAY_LEN(result->state), CoarseLevelFromPercent(percent));
    SetBatteryStateFromStatus(result, status);
    return TRUE;
}

static BOOL ParseVoltageBattery(
    BatteryResult *result,
    const HidCandidate *device,
    BYTE deviceIndex,
    const BYTE response[HID_IO_BUFFER_LEN])
{
    int millivolts = ((int)response[4] << 8) | response[5];
    BYTE status = response[6];

    if (millivolts < 2500 || millivolts > 5500) {
        return FALSE;
    }

    result->found = TRUE;
    result->confidence = BATTERY_CONFIDENCE_VOLTAGE;
    result->transport = device->transport;
    result->deviceIndex = deviceIndex;
    result->vid = device->vid;
    result->pid = device->pid;
    result->millivolts = millivolts;
    (void)StringCchCopyW(result->product, ARRAY_LEN(result->product), DisplayStringOrFallback(device->product, L"Logitech HID device"));
    (void)StringCchCopyW(result->source, ARRAY_LEN(result->source), L"Battery voltage 0x1001");
    (void)StringCchCopyW(result->state, ARRAY_LEN(result->state), L"Voltage only");
    SetBatteryStateFromStatus(result, status);
    return TRUE;
}

static BOOL Hidpp10ReadRegister(HidppIo *io, BYTE deviceIndex, BYTE registerId, BYTE response[HID_IO_BUFFER_LEN])
{
    BYTE output[HID_IO_BUFFER_LEN];
    BYTE input[HID_IO_BUFFER_LEN];
    DWORD outputLen = io->outputReportBytes;
    BYTE reportId = (io->outputReportBytes >= HIDPP_LONG_LEN && io->inputReportBytes >= HIDPP_LONG_LEN)
        ? HIDPP_REPORT_LONG
        : HIDPP_REPORT_SHORT;

    if (outputLen < HIDPP_SHORT_LEN || outputLen > HID_IO_BUFFER_LEN ||
        io->inputReportBytes < HIDPP_SHORT_LEN || io->inputReportBytes > HID_IO_BUFFER_LEN) {
        return FALSE;
    }

    ZeroMemory(output, outputLen);
    output[0] = reportId;
    output[1] = deviceIndex;
    output[2] = HIDPP10_GET_REGISTER;
    output[3] = registerId;

    (void)HidD_FlushQueue(io->handle);
    if (!OverlappedWriteWithTimeout(io->handle, output, outputLen, HIDPP_READ_TIMEOUT_MS)) {
        return FALSE;
    }

    for (int attempt = 0; attempt < HIDPP_READ_ATTEMPTS; ++attempt) {
        DWORD bytesRead = 0;

        if (!OverlappedReadWithTimeout(io->handle, input, io->inputReportBytes, HIDPP_READ_TIMEOUT_MS, &bytesRead)) {
            return FALSE;
        }

        if (bytesRead < HIDPP_SHORT_LEN || input[0] != reportId || input[1] != deviceIndex) {
            continue;
        }

        if (IsHidppErrorResponse(input, bytesRead)) {
            return FALSE;
        }

        if (input[2] == HIDPP10_GET_REGISTER && input[3] == registerId) {
            CopyMemory(response, input, bytesRead);
            if (bytesRead < HID_IO_BUFFER_LEN) {
                ZeroMemory(response + bytesRead, HID_IO_BUFFER_LEN - bytesRead);
            }
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL ParseHidpp10Battery(
    BatteryResult *result,
    const HidCandidate *device,
    BYTE deviceIndex,
    const BYTE response[HID_IO_BUFFER_LEN])
{
    BYTE level = response[4];
    BYTE statusA = response[5];
    BYTE statusB = response[6];
    const WCHAR *coarse = CoarseLevelFromEnum(level);

    result->found = TRUE;
    result->transport = device->transport;
    result->deviceIndex = deviceIndex;
    result->vid = device->vid;
    result->pid = device->pid;
    (void)StringCchCopyW(result->product, ARRAY_LEN(result->product), DisplayStringOrFallback(device->product, L"Logitech HID device"));
    (void)StringCchCopyW(result->source, ARRAY_LEN(result->source), L"HID++ 1.0 battery register");

    if (level > 4 && level <= 100) {
        result->confidence = BATTERY_CONFIDENCE_PERCENT;
        result->percent = level;
        (void)StringCchCopyW(result->state, ARRAY_LEN(result->state), CoarseLevelFromPercent(level));
        (void)SetBatteryStateFromStatus(result, statusA);
        (void)SetBatteryStateFromStatus(result, statusB);
        return TRUE;
    }

    if (coarse) {
        result->confidence = BATTERY_CONFIDENCE_COARSE;
        (void)StringCchCopyW(result->state, ARRAY_LEN(result->state), coarse);
        (void)SetBatteryStateFromStatus(result, statusA);
        (void)SetBatteryStateFromStatus(result, statusB);
        return TRUE;
    }

    if (SetBatteryStateFromStatus(result, statusA) || SetBatteryStateFromStatus(result, statusB)) {
        result->confidence = BATTERY_CONFIDENCE_COARSE;
        return TRUE;
    }

    ZeroMemory(result, sizeof(*result));
    return FALSE;
}

static BOOL QueryHidpp10Battery(HidppIo *io, const HidCandidate *device, BYTE deviceIndex, BatteryResult *result)
{
    BYTE response[HID_IO_BUFFER_LEN];

    if (!Hidpp10ReadRegister(io, deviceIndex, HIDPP10_BATTERY_REGISTER, response)) {
        return FALSE;
    }

    return ParseHidpp10Battery(result, device, deviceIndex, response);
}

static BOOL QueryBatteryFeature(HidppIo *io, const HidCandidate *device, BYTE deviceIndex, BatteryResult *result)
{
    BYTE featureIndex;
    BYTE response[HID_IO_BUFFER_LEN];
    BYTE emptyParams[3] = {0, 0, 0};

    if (HidppGetFeatureIndex(io, deviceIndex, 0x1004, &featureIndex) &&
        HidppCall(io, deviceIndex, featureIndex, 0x01, emptyParams, response) &&
        ParsePercentBattery(result, device, deviceIndex, 0x1004, L"Unified battery", response)) {
        return TRUE;
    }

    if (HidppGetFeatureIndex(io, deviceIndex, 0x1000, &featureIndex) &&
        HidppCall(io, deviceIndex, featureIndex, 0x00, emptyParams, response) &&
        ParsePercentBattery(result, device, deviceIndex, 0x1000, L"Battery status", response)) {
        return TRUE;
    }

    if (HidppGetFeatureIndex(io, deviceIndex, 0x1001, &featureIndex) &&
        HidppCall(io, deviceIndex, featureIndex, 0x00, emptyParams, response) &&
        ParseVoltageBattery(result, device, deviceIndex, response)) {
        return TRUE;
    }

    if (QueryHidpp10Battery(io, device, deviceIndex, result)) {
        return TRUE;
    }

    return FALSE;
}

static int BuildDeviceIndexList(TransportKind transport, BYTE *indexes, int maxIndexes)
{
    static const BYTE receiverIndexes[] = {1, 2, 3, 4, 5, 6};
    static const BYTE wiredIndexes[] = {0xFF, 0x00, 0x01};
    static const BYTE unknownIndexes[] = {0xFF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    const BYTE *source = unknownIndexes;
    int count = (int)ARRAY_LEN(unknownIndexes);

    if (transport == TRANSPORT_RECEIVER) {
        source = receiverIndexes;
        count = (int)ARRAY_LEN(receiverIndexes);
    } else if (transport == TRANSPORT_WIRED) {
        source = wiredIndexes;
        count = (int)ARRAY_LEN(wiredIndexes);
    }

    if (count > maxIndexes) {
        count = maxIndexes;
    }

    for (int i = 0; i < count; ++i) {
        indexes[i] = source[i];
    }

    return count;
}

static BOOL ProbeDevice(const HidCandidate *device, ProbeResult *probe)
{
    HidppIo io;
    BYTE indexes[8];
    int indexCount;

    ZeroMemory(probe, sizeof(*probe));
    if (!device->likelyHidpp || !OpenHidppIo(device, &io)) {
        return FALSE;
    }

    probe->opened = TRUE;
    indexCount = BuildDeviceIndexList(device->transport, indexes, (int)ARRAY_LEN(indexes));
    for (int i = 0; i < indexCount; ++i) {
        BYTE featureIndex;
        BYTE hidpp10Response[HID_IO_BUFFER_LEN];
        BYTE deviceIndex = indexes[i];

        if (HidppGetFeatureIndex(&io, deviceIndex, 0x1004, &featureIndex)) {
            probe->hidpp = TRUE;
            probe->deviceIndex = deviceIndex;
            probe->hasBatteryFeature = TRUE;
            (void)StringCchCopyW(probe->featureName, ARRAY_LEN(probe->featureName), L"Unified battery");
        } else if (HidppGetFeatureIndex(&io, deviceIndex, 0x1000, &featureIndex)) {
            probe->hidpp = TRUE;
            probe->deviceIndex = deviceIndex;
            probe->hasBatteryFeature = TRUE;
            (void)StringCchCopyW(probe->featureName, ARRAY_LEN(probe->featureName), L"Battery status");
        } else if (HidppGetFeatureIndex(&io, deviceIndex, 0x1001, &featureIndex)) {
            probe->hidpp = TRUE;
            probe->deviceIndex = deviceIndex;
            probe->hasBatteryFeature = TRUE;
            (void)StringCchCopyW(probe->featureName, ARRAY_LEN(probe->featureName), L"Battery voltage");
        } else if (Hidpp10ReadRegister(&io, deviceIndex, HIDPP10_BATTERY_REGISTER, hidpp10Response)) {
            probe->hidpp = TRUE;
            probe->deviceIndex = deviceIndex;
            probe->hasBatteryFeature = TRUE;
            (void)StringCchCopyW(probe->featureName, ARRAY_LEN(probe->featureName), L"HID++ 1.0 battery register");
        } else if (HidppGetFeatureIndex(&io, deviceIndex, HIDPP_FEATURE_FEATURE_SET, &featureIndex) ||
                   HidppGetFeatureIndex(&io, deviceIndex, HIDPP_FEATURE_DEVICE_NAME_TYPE, &featureIndex) ||
                   HidppGetFeatureIndex(&io, deviceIndex, HIDPP_FEATURE_ADJUSTABLE_DPI, &featureIndex) ||
                   HidppGetFeatureIndex(&io, deviceIndex, HIDPP_FEATURE_ONBOARD_PROFILES, &featureIndex)) {
            probe->hidpp = TRUE;
            probe->deviceIndex = deviceIndex;
        } else {
            continue;
        }

        CloseHidppIo(&io);
        return TRUE;
    }

    CloseHidppIo(&io);
    return TRUE;
}

static BOOL QueryDeviceSnapshot(const HidCandidate *device, DeviceSnapshot *snapshot)
{
    HidppIo io;
    BYTE indexes[8];
    int indexCount;

    ZeroMemory(snapshot, sizeof(*snapshot));

    if (!device->likelyHidpp || !OpenHidppIo(device, &io)) {
        return FALSE;
    }

    indexCount = BuildDeviceIndexList(device->transport, indexes, (int)ARRAY_LEN(indexes));
    for (int i = 0; i < indexCount; ++i) {
        if (QueryBatteryFeature(&io, device, indexes[i], &snapshot->battery)) {
            snapshot->device = *device;
            QueryDeviceDetails(&io, indexes[i], &snapshot->details);
            CloseHidppIo(&io);
            return TRUE;
        }
    }

    CloseHidppIo(&io);
    return FALSE;
}

static int BatteryResultScore(const BatteryResult *result)
{
    int score = 0;

    if (!result->found) {
        return 0;
    }

    switch (result->confidence) {
    case BATTERY_CONFIDENCE_PERCENT:
        score += 300;
        break;
    case BATTERY_CONFIDENCE_COARSE:
        score += 200;
        break;
    case BATTERY_CONFIDENCE_VOLTAGE:
        score += 100;
        break;
    default:
        break;
    }

    if (result->transport == TRANSPORT_WIRED) {
        score += 50;
    }
    if (result->charging) {
        score += 25;
    }

    return score;
}

static BOOL QueryBestSnapshot(DeviceSnapshot *snapshot)
{
    HidCandidate *devices = NULL;
    HidCandidate cachedDevice;
    int count;
    DeviceSnapshot bestAny;
    DeviceSnapshot bestWired;
    int bestAnyScore = 0;
    int bestWiredScore = 0;

    ZeroMemory(snapshot, sizeof(*snapshot));
    ZeroMemory(&bestAny, sizeof(bestAny));
    ZeroMemory(&bestWired, sizeof(bestWired));
    if (!AcquireHidQueryLock()) {
        return FALSE;
    }

    if (CopyCachedDevice(&cachedDevice)) {
        if (QueryDeviceSnapshot(&cachedDevice, snapshot)) {
            ReleaseHidQueryLock();
            return TRUE;
        }
        InvalidateDeviceCache();
    }

    devices = (HidCandidate *)calloc(MAX_HID_DEVICES, sizeof(*devices));
    if (!devices) {
        ReleaseHidQueryLock();
        return FALSE;
    }

    count = EnumerateLogitechHidDevices(devices, MAX_HID_DEVICES);
    for (int i = 0; i < count; ++i) {
        DeviceSnapshot current;
        int score;

        ZeroMemory(&current, sizeof(current));
        if (!QueryDeviceSnapshot(&devices[i], &current)) {
            continue;
        }

        score = BatteryResultScore(&current.battery);
        if (score > bestAnyScore) {
            bestAny = current;
            bestAnyScore = score;
        }
        if (current.battery.transport == TRANSPORT_WIRED && score > bestWiredScore) {
            bestWired = current;
            bestWiredScore = score;
        }
    }

    if (bestWiredScore > 0) {
        *snapshot = bestWired;
    } else if (bestAnyScore > 0) {
        *snapshot = bestAny;
    } else {
        InvalidateDeviceCache();
        free(devices);
        ReleaseHidQueryLock();
        return FALSE;
    }

    StoreCachedDevice(&snapshot->device);
    free(devices);
    ReleaseHidQueryLock();
    return TRUE;
}

static void DescribeBattery(const BatteryResult *result, WCHAR *buffer, size_t bufferChars)
{
    if (!result->found) {
        (void)StringCchCopyW(buffer, bufferChars, L"Battery: unavailable");
        return;
    }

    if (result->confidence == BATTERY_CONFIDENCE_PERCENT) {
        if (result->state[0]) {
            (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d%% (%s)", result->percent, result->state);
        } else {
            (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d%%", result->percent);
        }
    } else if (result->confidence == BATTERY_CONFIDENCE_COARSE) {
        (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %s", result->state[0] ? result->state : L"coarse state");
    } else if (result->confidence == BATTERY_CONFIDENCE_VOLTAGE) {
        if (result->state[0] && wcscmp(result->state, L"Voltage only") != 0) {
            (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d mV (%s)", result->millivolts, result->state);
        } else {
            (void)StringCchPrintfW(buffer, bufferChars, L"Battery: %d mV", result->millivolts);
        }
    } else {
        (void)StringCchCopyW(buffer, bufferChars, L"Battery: unavailable");
    }
}

static int PrintListDevices(void)
{
    HidCandidate *devices = (HidCandidate *)calloc(MAX_HID_DEVICES, sizeof(*devices));
    int count;
    int shown = 0;

    if (!devices) {
        wprintf(L"Could not allocate the HID device list.\n");
        return 1;
    }

    count = EnumerateLogitechHidDevices(devices, MAX_HID_DEVICES);
    if (count == 0) {
        wprintf(L"No Logitech HID devices found.\n");
        free(devices);
        return 1;
    }

    wprintf(L"Logitech HID candidates (no serials or USB instance IDs are shown):\n");
    for (int i = 0; i < count; ++i) {
        const HidCandidate *device = &devices[i];
        if (!device->likelyHidpp && !device->vendorDefined) {
            continue;
        }

        ++shown;
        wprintf(
            L"  %d. %s | %s | VID_%04X PID_%04X | usage 0x%04X/0x%04X | reports in/out/feature %u/%u/%u | HID++ candidate: %s\n",
            shown,
            TransportName(device->transport),
            DisplayStringOrFallback(device->product, L"Logitech HID device"),
            device->vid,
            device->pid,
            device->usagePage,
            device->usage,
            device->inputReportBytes,
            device->outputReportBytes,
            device->featureReportBytes,
            device->likelyHidpp ? L"yes" : L"no");
    }

    if (shown == 0) {
        wprintf(L"No likely Logitech HID++ candidates found.\n");
        free(devices);
        return 1;
    }

    free(devices);
    return 0;
}

static int PrintProbe(void)
{
    HidCandidate *devices = (HidCandidate *)calloc(MAX_HID_DEVICES, sizeof(*devices));
    int count;
    int probed = 0;

    if (!devices) {
        wprintf(L"Could not allocate the HID device list.\n");
        return 1;
    }

    count = EnumerateLogitechHidDevices(devices, MAX_HID_DEVICES);
    if (count == 0) {
        wprintf(L"No Logitech HID devices found.\n");
        free(devices);
        return 1;
    }

    if (!AcquireHidQueryLock()) {
        wprintf(L"Could not acquire HID query lock.\n");
        free(devices);
        return 1;
    }

    wprintf(L"HID++ probe results:\n");
    for (int i = 0; i < count; ++i) {
        ProbeResult probe;
        const HidCandidate *device = &devices[i];

        if (!device->likelyHidpp) {
            continue;
        }

        ++probed;
        (void)ProbeDevice(device, &probe);
        wprintf(
            L"  %s | %s | VID_%04X PID_%04X | open: %s | HID++: %s",
            TransportName(device->transport),
            DisplayStringOrFallback(device->product, L"Logitech HID device"),
            device->vid,
            device->pid,
            probe.opened ? L"yes" : L"no",
            probe.hidpp ? L"yes" : L"no");

        if (probe.hidpp) {
            wprintf(L" | device index: 0x%02X", probe.deviceIndex);
        }
        if (probe.hasBatteryFeature) {
            wprintf(L" | battery feature: %s", probe.featureName);
        }
        wprintf(L"\n");
    }

    if (probed == 0) {
        wprintf(L"No likely Logitech HID++ candidates found.\n");
        ReleaseHidQueryLock();
        free(devices);
        return 1;
    }

    ReleaseHidQueryLock();
    free(devices);
    return 0;
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

static void JsonPrintAString(const char *value)
{
    putchar('"');
    for (const char *p = value ? value : ""; *p; ++p) {
        unsigned char ch = (unsigned char)*p;

        switch (ch) {
        case '"':
            printf("\\\"");
            break;
        case '\\':
            printf("\\\\");
            break;
        case '\b':
            printf("\\b");
            break;
        case '\f':
            printf("\\f");
            break;
        case '\n':
            printf("\\n");
            break;
        case '\r':
            printf("\\r");
            break;
        case '\t':
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

static int PrintOnce(void)
{
    DeviceSnapshot snapshot;
    const BatteryResult *battery = &snapshot.battery;
    const DeviceDetails *details = &snapshot.details;
    const WCHAR *displayName;

    if (!QueryBestSnapshot(&snapshot)) {
        printf("{\"error\":\"no_supported_device\"}\n");
        return 1;
    }

    displayName = details->firmwareNameFound
        ? details->firmwareName
        : DisplayStringOrFallback(snapshot.device.product, L"Logitech HID device");

    printf("{\n");
    printf("  \"battery\": {\n");
    if (battery->confidence == BATTERY_CONFIDENCE_PERCENT) {
        printf("    \"percent\": %d,\n", battery->percent);
    } else {
        printf("    \"percent\": null,\n");
    }
    printf("    \"state\": ");
    JsonPrintWString(battery->state[0] ? battery->state : L"Unknown");
    printf(",\n");
    printf("    \"charging\": %s,\n", battery->charging ? "true" : "false");
    printf("    \"source\": ");
    JsonPrintWString(battery->source);
    printf("\n");
    printf("  },\n");

    printf("  \"device\": {\n");
    printf("    \"name\": ");
    JsonPrintWString(displayName);
    printf(",\n");
    printf("    \"hid_product\": ");
    JsonPrintWString(DisplayStringOrFallback(snapshot.device.product, L"Logitech HID device"));
    printf(",\n");
    printf("    \"transport\": ");
    JsonPrintWString(TransportName(battery->transport));
    printf(",\n");
    printf("    \"vid\": \"VID_%04X\",\n", battery->vid);
    printf("    \"pid\": \"PID_%04X\",\n", battery->pid);
    printf("    \"hidpp_device_index\": \"0x%02X\"\n", battery->deviceIndex);
    printf("  },\n");

    printf("  \"dpi\": ");
    if (details->dpi.found) {
        printf("{\n");
        printf("    \"current\": %d,\n", details->dpi.currentDpi);
        printf("    ");
        PrintJsonNullableInt("min", details->dpi.minDpi > 0 ? details->dpi.minDpi : -1);
        printf(",\n");
        printf("    ");
        PrintJsonNullableInt("max", details->dpi.maxDpi > 0 ? details->dpi.maxDpi : -1);
        printf("\n");
        printf("  },\n");
    } else {
        printf("null,\n");
    }

    printf("  \"profile\": ");
    if (details->profile.found) {
        printf("{\n");
        printf("    ");
        PrintJsonNullableInt("index", details->profile.rawIndex);
        printf(",\n");
        printf("    ");
        PrintJsonNullableInt("dpi_slot", details->profile.currentDpiIndex);
        printf("\n");
        printf("  }\n");
    } else {
        printf("null\n");
    }

    printf("}\n");
    return 0;
}

static void PrintHelp(const WCHAR *exeName)
{
    wprintf(L"Usage: %s [--list-devices | --probe | --once | --tray]\n", exeName);
    wprintf(L"\n");
    wprintf(L"  --list-devices  List generic Logitech HID++ candidates without private IDs.\n");
    wprintf(L"  --probe         Probe HID++ communication and battery feature support.\n");
    wprintf(L"  --once          Print one JSON device snapshot and exit.\n");
    wprintf(L"  --tray          Run the on-demand tray indicator. This is the default.\n");
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

static NOTIFYICONDATAW g_tray = {0};
static DeviceSnapshot g_traySnapshot = {0};
static DeviceSnapshot g_refreshSnapshot = {0};
static BOOL g_traySnapshotValid = FALSE;
static HICON g_trayIcon = NULL;
static int g_trayIconPercent = -2;
static BOOL g_trayIconCharging = FALSE;
static int g_trayIconSize = 0;
static ULONGLONG g_lastHoverRefreshTick = 0;
static volatile LONG g_refreshInProgress = 0;
static HDEVNOTIFY g_hidDeviceNotify = NULL;

static DWORD Argb(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
}

static BOOL PointInRoundedRect(int x, int y, int left, int top, int right, int bottom, int radius)
{
    int cx = x;
    int cy = y;
    int dx;
    int dy;

    if (x < left || x > right || y < top || y > bottom) {
        return FALSE;
    }

    if (cx < left + radius) {
        cx = left + radius;
    } else if (cx > right - radius) {
        cx = right - radius;
    }

    if (cy < top + radius) {
        cy = top + radius;
    } else if (cy > bottom - radius) {
        cy = bottom - radius;
    }

    dx = x - cx;
    dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static void DrawRoundedRect(DWORD *pixels, int width, int height, int left, int top, int right, int bottom, int radius, DWORD color)
{
    for (int y = top; y <= bottom; ++y) {
        if (y < 0 || y >= height) {
            continue;
        }

        for (int x = left; x <= right; ++x) {
            if (x < 0 || x >= width) {
                continue;
            }

            if (PointInRoundedRect(x, y, left, top, right, bottom, radius)) {
                pixels[(size_t)y * width + x] = color;
            }
        }
    }
}

static BOOL PointInPolygon(int x, int y, const POINT *points, int pointCount)
{
    BOOL inside = FALSE;

    for (int i = 0, j = pointCount - 1; i < pointCount; j = i++) {
        if (((points[i].y > y) != (points[j].y > y)) &&
            (x < (points[j].x - points[i].x) * (y - points[i].y) / (points[j].y - points[i].y) + points[i].x)) {
            inside = !inside;
        }
    }

    return inside;
}

static void DrawPolygon(DWORD *pixels, int width, int height, const POINT *points, int pointCount, int dx, int dy, DWORD color)
{
    POINT moved[8];
    int left;
    int right;
    int top;
    int bottom;

    if (pointCount <= 0 || pointCount > (int)ARRAY_LEN(moved)) {
        return;
    }

    for (int i = 0; i < pointCount; ++i) {
        moved[i].x = points[i].x + dx;
        moved[i].y = points[i].y + dy;
    }

    left = right = moved[0].x;
    top = bottom = moved[0].y;
    for (int i = 1; i < pointCount; ++i) {
        if (moved[i].x < left) {
            left = moved[i].x;
        }
        if (moved[i].x > right) {
            right = moved[i].x;
        }
        if (moved[i].y < top) {
            top = moved[i].y;
        }
        if (moved[i].y > bottom) {
            bottom = moved[i].y;
        }
    }

    for (int y = top; y <= bottom; ++y) {
        if (y < 0 || y >= height) {
            continue;
        }

        for (int x = left; x <= right; ++x) {
            if (x < 0 || x >= width) {
                continue;
            }

            if (PointInPolygon(x, y, moved, pointCount)) {
                pixels[(size_t)y * width + x] = color;
            }
        }
    }
}

static void DrawChargingBolt(DWORD *pixels, int hi)
{
    static const POINT bolt[] = {
        {37, 14}, {25, 36}, {34, 35}, {29, 53}, {46, 28}, {37, 30}
    };
    POINT scaled[ARRAY_LEN(bolt)];
    int shadow = (2 * hi + 32) / 64;

    if (shadow < 1) {
        shadow = 1;
    }

    for (int i = 0; i < (int)ARRAY_LEN(bolt); ++i) {
        scaled[i].x = (bolt[i].x * hi + 32) / 64;
        scaled[i].y = (bolt[i].y * hi + 32) / 64;
    }

    DrawPolygon(pixels, hi, hi, scaled, (int)ARRAY_LEN(scaled), shadow, shadow, Argb(150, 0, 0, 0));
    DrawPolygon(pixels, hi, hi, scaled, (int)ARRAY_LEN(scaled), 0, 0, Argb(255, 255, 211, 75));
}

static HICON CreateBatteryTrayIcon(int targetSize)
{
    enum { SUPERSAMPLE = 4 };
    int hi;
    int samples = SUPERSAMPLE * SUPERSAMPLE;
    BITMAPINFO bmi;
    void *bits = NULL;
    HDC screenDc = NULL;
    HBITMAP colorBitmap = NULL;
    HBITMAP maskBitmap = NULL;
    HICON icon = NULL;
    ICONINFO iconInfo;
    DWORD *highPixels = NULL;
    BYTE *maskBits = NULL;
    DWORD *pixels;
    int maskRowBytes;
    int percent = -1;
    BOOL charging = FALSE;
    DWORD shell = Argb(255, 22, 27, 34);
    DWORD surface = Argb(255, 245, 247, 250);
    DWORD empty = Argb(255, 165, 174, 185);
    DWORD fill = Argb(255, 32, 201, 117);

    if (targetSize <= 0 || targetSize > 256) {
        return NULL;
    }

    hi = targetSize * SUPERSAMPLE;
    maskRowBytes = ((targetSize + 31) / 32) * 4;

    if (g_traySnapshotValid) {
        charging = g_traySnapshot.battery.charging;
        if (g_traySnapshot.battery.confidence == BATTERY_CONFIDENCE_PERCENT) {
            percent = g_traySnapshot.battery.percent;
        }
    }

    if (percent >= 0 && percent <= 20) {
        fill = Argb(255, 238, 76, 76);
    } else if (percent >= 0 && percent <= 50) {
        fill = Argb(255, 245, 183, 66);
    }

    highPixels = (DWORD *)calloc((size_t)hi * (size_t)hi, sizeof(DWORD));
    maskBits = (BYTE *)calloc((size_t)maskRowBytes * (size_t)targetSize, 1);
    if (!highPixels || !maskBits) {
        free(highPixels);
        free(maskBits);
        return NULL;
    }

#define DSX(v) (((v) * hi + 32) / 64)

    DrawRoundedRect(highPixels, hi, hi, DSX(10), DSX(8), DSX(58), DSX(63), DSX(10), Argb(90, 0, 0, 0));
    DrawRoundedRect(highPixels, hi, hi, DSX(22), DSX(0), DSX(42), DSX(10), DSX(4), shell);
    DrawRoundedRect(highPixels, hi, hi, DSX(8), DSX(5), DSX(56), DSX(63), DSX(10), shell);
    DrawRoundedRect(highPixels, hi, hi, DSX(15), DSX(12), DSX(49), DSX(57), DSX(7), surface);

    {
        int fillLeft = DSX(19);
        int fillRight = DSX(45);
        int fillTop = DSX(16);
        int fillBottom = DSX(53);
        int radius = DSX(5);

        if (percent >= 0) {
            int height = ((fillBottom - fillTop + 1) * percent + 99) / 100;
            int visibleTop = fillBottom - height + 1;

            if (percent > 0 && visibleTop > fillBottom) {
                visibleTop = fillBottom;
            }

            for (int y = fillTop; y <= fillBottom; ++y) {
                for (int x = fillLeft; x <= fillRight; ++x) {
                    if (y >= visibleTop && PointInRoundedRect(x, y, fillLeft, fillTop, fillRight, fillBottom, radius)) {
                        highPixels[(size_t)y * hi + x] = fill;
                    }
                }
            }
        } else {
            DrawRoundedRect(highPixels, hi, hi, fillLeft, fillTop, fillRight, fillBottom, radius, empty);
        }
    }

#undef DSX

    if (charging) {
        DrawChargingBolt(highPixels, hi);
    }

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = targetSize;
    bmi.bmiHeader.biHeight = -targetSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    screenDc = GetDC(NULL);
    if (!screenDc) {
        free(highPixels);
        free(maskBits);
        return NULL;
    }

    colorBitmap = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screenDc);
    if (!colorBitmap || !bits) {
        if (colorBitmap) {
            DeleteObject(colorBitmap);
        }
        free(highPixels);
        free(maskBits);
        return NULL;
    }

    pixels = (DWORD *)bits;
    ZeroMemory(pixels, (size_t)targetSize * (size_t)targetSize * sizeof(DWORD));

    for (int y = 0; y < targetSize; ++y) {
        for (int x = 0; x < targetSize; ++x) {
            unsigned int a = 0;
            unsigned int r = 0;
            unsigned int g = 0;
            unsigned int b = 0;

            for (int sy = 0; sy < SUPERSAMPLE; ++sy) {
                for (int sx = 0; sx < SUPERSAMPLE; ++sx) {
                    DWORD c = highPixels[(size_t)(y * SUPERSAMPLE + sy) * hi + (x * SUPERSAMPLE + sx)];
                    BYTE ca = (BYTE)((c >> 24) & 0xFF);

                    a += ca;
                    r += ((c >> 16) & 0xFF) * ca;
                    g += ((c >> 8) & 0xFF) * ca;
                    b += (c & 0xFF) * ca;
                }
            }

            if (a > 0) {
                r /= a;
                g /= a;
                b /= a;
                a /= samples;
                pixels[(size_t)y * targetSize + x] = Argb((BYTE)a, (BYTE)r, (BYTE)g, (BYTE)b);
            }
        }
    }

    maskBitmap = CreateBitmap(targetSize, targetSize, 1, 1, maskBits);
    free(highPixels);
    free(maskBits);
    if (!maskBitmap) {
        DeleteObject(colorBitmap);
        return NULL;
    }

    ZeroMemory(&iconInfo, sizeof(iconInfo));
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = colorBitmap;
    iconInfo.hbmMask = maskBitmap;
    icon = CreateIconIndirect(&iconInfo);

    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return icon;
}

static int GetTrayIconSize(HWND hwnd)
{
    static GetSystemMetricsForDpiFn pGetSystemMetricsForDpi = NULL;
    static GetDpiForWindowFn pGetDpiForWindow = NULL;
    static BOOL resolved = FALSE;
    int size;

    if (!resolved) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            pGetSystemMetricsForDpi = (GetSystemMetricsForDpiFn)
                GetProcAddress(user32, "GetSystemMetricsForDpi");
            pGetDpiForWindow = (GetDpiForWindowFn)
                GetProcAddress(user32, "GetDpiForWindow");
        }
        resolved = TRUE;
    }

    if (hwnd && pGetSystemMetricsForDpi && pGetDpiForWindow) {
        UINT dpi = pGetDpiForWindow(hwnd);

        if (dpi == 0) {
            dpi = 96;
        }
        size = pGetSystemMetricsForDpi(SM_CXSMICON, dpi);
        if (size > 0) {
            return size;
        }
    }

    size = GetSystemMetrics(SM_CXSMICON);
    return size > 0 ? size : 16;
}

static void EnablePerMonitorDpiAwareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetProcessDpiAwarenessContextFn p = NULL;

    if (user32) {
        p = (SetProcessDpiAwarenessContextFn)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    }

    if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }

    if (user32) {
        typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
        SetProcessDPIAwareFn legacy = (SetProcessDPIAwareFn)
            GetProcAddress(user32, "SetProcessDPIAware");

        if (legacy) {
            legacy();
        }
    }
}

static const WCHAR *SnapshotDeviceName(const DeviceSnapshot *snapshot)
{
    if (snapshot->details.firmwareNameFound) {
        return snapshot->details.firmwareName;
    }

    return DisplayStringOrFallback(snapshot->device.product, L"Logitech HID device");
}

static void DescribeTrayTip(WCHAR *buffer, size_t bufferChars)
{
    const BatteryResult *battery = &g_traySnapshot.battery;

    if (!g_traySnapshotValid) {
        (void)StringCchCopyW(buffer, bufferChars, L"SuperLightBattery: unavailable");
        return;
    }

    if (battery->confidence == BATTERY_CONFIDENCE_PERCENT) {
        if (g_traySnapshot.details.dpi.found) {
            (void)StringCchPrintfW(
                buffer,
                bufferChars,
                L"%s: %d%% %s, %d DPI",
                SnapshotDeviceName(&g_traySnapshot),
                battery->percent,
                battery->state[0] ? battery->state : L"",
                g_traySnapshot.details.dpi.currentDpi);
        } else {
            (void)StringCchPrintfW(
                buffer,
                bufferChars,
                L"%s: %d%% %s",
                SnapshotDeviceName(&g_traySnapshot),
                battery->percent,
                battery->state[0] ? battery->state : L"");
        }
    } else {
        WCHAR batteryLine[128];

        DescribeBattery(battery, batteryLine, ARRAY_LEN(batteryLine));
        (void)StringCchPrintfW(buffer, bufferChars, L"%s: %s", SnapshotDeviceName(&g_traySnapshot), batteryLine);
    }
}

static void UpdateTrayTip(HWND hwnd)
{
    WCHAR tip[128];
    HICON oldIcon = g_trayIcon;
    int iconPercent = -1;
    BOOL iconCharging = FALSE;
    int iconSize = GetTrayIconSize(hwnd);
    BOOL iconChanged;

    DescribeTrayTip(tip, ARRAY_LEN(tip));

    if (g_traySnapshotValid) {
        iconCharging = g_traySnapshot.battery.charging;
        if (g_traySnapshot.battery.confidence == BATTERY_CONFIDENCE_PERCENT) {
            iconPercent = g_traySnapshot.battery.percent;
        }
    }

    iconChanged = !g_trayIcon
        || iconPercent != g_trayIconPercent
        || iconCharging != g_trayIconCharging
        || iconSize != g_trayIconSize;
    if (iconChanged) {
        g_trayIcon = CreateBatteryTrayIcon(iconSize);
        g_trayIconPercent = iconPercent;
        g_trayIconCharging = iconCharging;
        g_trayIconSize = iconSize;
    }

    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = TRAY_UID;
    g_tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    if (iconChanged) {
        g_tray.uFlags |= NIF_ICON;
    }
    g_tray.uCallbackMessage = WM_TRAYICON;
    g_tray.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    (void)StringCchCopyW(g_tray.szTip, ARRAY_LEN(g_tray.szTip), tip);
    (void)Shell_NotifyIconW(NIM_MODIFY, &g_tray);
    if (iconChanged && oldIcon) {
        DestroyIcon(oldIcon);
    }
}

static DWORD WINAPI RefreshTrayBatteryWorker(void *context)
{
    HWND hwnd = (HWND)context;
    BOOL ok;

    ZeroMemory(&g_refreshSnapshot, sizeof(g_refreshSnapshot));
    ok = QueryBestSnapshot(&g_refreshSnapshot);

    if (!PostMessageW(hwnd, WM_REFRESH_COMPLETE, ok ? 1 : 0, ok ? (LPARAM)&g_refreshSnapshot : 0)) {
        InterlockedExchange(&g_refreshInProgress, 0);
    }

    return 0;
}

static void RequestTrayBatteryRefresh(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_refreshInProgress, 1, 0) != 0) {
        return;
    }

    if (!QueueUserWorkItem(RefreshTrayBatteryWorker, hwnd, WT_EXECUTEDEFAULT)) {
        InterlockedExchange(&g_refreshInProgress, 0);
    }
}

static void RequestTrayBatteryRefreshForHover(HWND hwnd)
{
    ULONGLONG now = GetTickCount64();

    if (g_lastHoverRefreshTick != 0 && now - g_lastHoverRefreshTick < HOVER_REFRESH_COOLDOWN_MS) {
        return;
    }

    g_lastHoverRefreshTick = now;
    RequestTrayBatteryRefresh(hwnd);
}

static void AddTrayIcon(HWND hwnd)
{
    WCHAR tip[128];
    int iconSize = GetTrayIconSize(hwnd);

    g_trayIcon = CreateBatteryTrayIcon(iconSize);
    g_trayIconSize = iconSize;
    (void)StringCchCopyW(tip, ARRAY_LEN(tip), L"SuperLightBattery: starting");

    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = TRAY_UID;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_tray.uCallbackMessage = WM_TRAYICON;
    g_tray.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    (void)StringCchCopyW(g_tray.szTip, ARRAY_LEN(g_tray.szTip), tip);
    if (Shell_NotifyIconW(NIM_ADD, &g_tray)) {
        g_tray.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &g_tray);
    }
}

static void RemoveTrayIcon(void)
{
    if (g_tray.cbSize != 0) {
        (void)Shell_NotifyIconW(NIM_DELETE, &g_tray);
    }
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = NULL;
    }
    g_trayIconPercent = -2;
    g_trayIconCharging = FALSE;
}

static void RegisterHidDeviceNotifications(HWND hwnd)
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    GUID hidGuid;

    HidD_GetHidGuid(&hidGuid);
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = hidGuid;

    g_hidDeviceNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static void UnregisterHidDeviceNotifications(void)
{
    if (g_hidDeviceNotify) {
        UnregisterDeviceNotification(g_hidDeviceNotify);
        g_hidDeviceNotify = NULL;
    }
}

static void ShowTrayMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    WCHAR batteryText[160];
    WCHAR batteryLine[128];
    WCHAR line[160];

    if (!menu) {
        return;
    }

    if (g_traySnapshotValid) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, SnapshotDeviceName(&g_traySnapshot));

        DescribeBattery(&g_traySnapshot.battery, batteryLine, ARRAY_LEN(batteryLine));
        (void)StringCchPrintfW(batteryText, ARRAY_LEN(batteryText), L"%s", batteryLine);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, TRAY_MENU_BATTERY, batteryText);

        if (g_traySnapshot.details.dpi.found) {
            (void)StringCchPrintfW(
                line,
                ARRAY_LEN(line),
                L"DPI: %d (%d-%d)",
                g_traySnapshot.details.dpi.currentDpi,
                g_traySnapshot.details.dpi.minDpi,
                g_traySnapshot.details.dpi.maxDpi);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
        }

        if (g_traySnapshot.details.profile.found) {
            (void)StringCchPrintfW(
                line,
                ARRAY_LEN(line),
                L"Profile: %d, DPI slot: %d",
                g_traySnapshot.details.profile.rawIndex,
                g_traySnapshot.details.profile.currentDpiIndex);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line);
        }
    } else {
        AppendMenuW(
            menu,
            MF_STRING | MF_GRAYED,
            TRAY_MENU_BATTERY,
            g_refreshInProgress ? L"Battery: refreshing..." : L"Battery: unavailable");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TRAY_MENU_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, TRAY_MENU_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        RegisterHidDeviceNotifications(hwnd);
        RequestTrayBatteryRefresh(hwnd);
        return 0;

    case WM_REFRESH_COMPLETE:
    {
        DeviceSnapshot *snapshot = (DeviceSnapshot *)lParam;

        if (wParam && snapshot) {
            g_traySnapshot = *snapshot;
            g_traySnapshotValid = TRUE;
        } else {
            ZeroMemory(&g_traySnapshot, sizeof(g_traySnapshot));
            g_traySnapshotValid = FALSE;
        }

        InterlockedExchange(&g_refreshInProgress, 0);
        UpdateTrayTip(hwnd);
        return 0;
    }

    case WM_TRAYICON:
    {
        UINT trayEvent = LOWORD(lParam);

        if (trayEvent == NIN_POPUPOPEN || trayEvent == WM_MOUSEMOVE) {
            /* Some shells do not reliably send NIN_POPUPOPEN; the cooldown keeps mousemove cheap. */
            RequestTrayBatteryRefreshForHover(hwnd);
        } else if (trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
            RequestTrayBatteryRefresh(hwnd);
            ShowTrayMenu(hwnd);
        } else if (trayEvent == WM_LBUTTONDBLCLK) {
            RequestTrayBatteryRefresh(hwnd);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case TRAY_MENU_REFRESH:
            RequestTrayBatteryRefresh(hwnd);
            return 0;
        case TRAY_MENU_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
            InvalidateDeviceCache();
            RequestTrayBatteryRefresh(hwnd);
        }
        return 0;

    case WM_DESTROY:
        UnregisterHidDeviceNotifications();
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunTray(HINSTANCE instance)
{
    const WCHAR className[] = L"SuperLightBatteryTrayWindow";
    HANDLE mutex;
    DWORD mutexStatus;
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

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

    EnablePerMonitorDpiAwareness();

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fwprintf(stderr, L"Could not register tray window class.\n");
        CloseHandle(mutex);
        return 1;
    }

    hwnd = CreateWindowExW(
        0,
        className,
        L"SuperLightBattery",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        NULL,
        instance,
        NULL);

    if (!hwnd) {
        fwprintf(stderr, L"Could not create tray window.\n");
        CloseHandle(mutex);
        return 1;
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    int result;

    if (argc > 1) {
        if (wcscmp(argv[1], L"--list-devices") == 0) {
            result = PrintListDevices();
            CloseHidQueryLockHandle();
            return result;
        }
        if (wcscmp(argv[1], L"--probe") == 0) {
            result = PrintProbe();
            CloseHidQueryLockHandle();
            return result;
        }
        if (wcscmp(argv[1], L"--once") == 0) {
            result = PrintOnce();
            CloseHidQueryLockHandle();
            return result;
        }
        if (wcscmp(argv[1], L"--tray") == 0) {
            FreeConsole();
            result = RunTray(instance);
            CloseHidQueryLockHandle();
            return result;
        }
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"/?") == 0) {
            PrintHelp(BasenameOfPath(argv[0]));
            return 0;
        }

        fwprintf(stderr, L"Unknown option: %s\n\n", argv[1]);
        PrintHelp(BasenameOfPath(argv[0]));
        return 2;
    }

    FreeConsole();
    result = RunTray(instance);
    CloseHidQueryLockHandle();
    return result;
}
