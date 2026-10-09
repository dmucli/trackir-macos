/*
 * Drop-in NPClient.dll / NPClient64.dll for games running under Wine or CrossOver on macOS.
 *
 * It keeps the exported API and return codes of NaturalPoint's NPClient 5.5.3 (see docs/PROTOCOL.md section 1)
 * but, instead of the "Local\SharedTrackIRData" mapping and SendMessage to the TrackIR window, it talks to the
 * native trackir-mac daemon through a shared file (src/common/tir_bridge.h).
 *
 * Install: copy into a folder inside the Wine prefix and point
 *   HKCU\Software\NaturalPoint\NATURALPOINT\NPClient Location
 * at that folder (with a trailing backslash), exactly as TrackIR does on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "common/np_shared.h"
#include "common/tir_bridge.h"

/* Exports, names and ordinals come from NPClient.def / NPClient64.def only, so 32-bit names stay undecorated. */
#define NP_EXPORT

static HANDLE g_file = INVALID_HANDLE_VALUE;
static HANDLE g_mapping;
static tir_bridge *g_bridge;
static uint32_t g_last_heartbeat;
static DWORD g_last_heartbeat_tick;

static void bridge_open(void)
{
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA(TIR_BRIDGE_ENV, path, sizeof(path));
    if (n == 0 || n >= sizeof(path))
        lstrcpynA(path, TIR_BRIDGE_WINE_PATH, sizeof(path));

    g_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    g_mapping = CreateFileMappingA(g_file, NULL, PAGE_READWRITE, 0, TIR_BRIDGE_FILE_SIZE, NULL);
    if (!g_mapping)
        return;
    g_bridge = (tir_bridge *)MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, TIR_BRIDGE_FILE_SIZE);
}

static void bridge_close(void)
{
    if (g_bridge)
        UnmapViewOfFile(g_bridge);
    if (g_mapping)
        CloseHandle(g_mapping);
    if (g_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_file);
    g_bridge = NULL;
    g_mapping = NULL;
    g_file = INVALID_HANDLE_VALUE;
}

/* Equivalent of FindWindowA(NULL, "NaturalPoint") succeeding: the daemon exists and its heartbeat moves. */
static int tracker_running(void)
{
    if (!g_bridge)
        bridge_open();
    if (!g_bridge || g_bridge->magic != TIR_BRIDGE_MAGIC || g_bridge->daemon_pid == 0)
        return 0;
    DWORD now = GetTickCount();
    uint32_t beat = g_bridge->heartbeat;
    if (beat != g_last_heartbeat || g_last_heartbeat_tick == 0) {
        g_last_heartbeat = beat;
        g_last_heartbeat_tick = now;
        return 1;
    }
    return now - g_last_heartbeat_tick < 2000;
}

/* SendMessageA(hwnd, 0x405, code, arg) */
static int send_command(uint32_t code, uint32_t arg)
{
    if (!tracker_running())
        return NP_ERR_DEVICE_NOT_PRESENT;
    uint32_t index = (uint32_t)InterlockedIncrement((LONG volatile *)&g_bridge->cmd_head) - 1;
    tir_bridge_command *slot = &g_bridge->cmds[index % TIR_BRIDGE_RING];
    slot->code = code;
    slot->arg = arg;
    MemoryBarrier();
    slot->sequence = index + 1;
    return NP_OK;
}

static int read_data(np_trackir_data *out)
{
    if (!tracker_running())
        return NP_ERR_DEVICE_NOT_PRESENT;
    for (int attempt = 0; attempt < 1000; attempt++) {
        uint32_t before = g_bridge->data_seq;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        memcpy(out, (const void *)&g_bridge->block.data, sizeof(*out));
        MemoryBarrier();
        if (g_bridge->data_seq == before)
            return NP_OK;
    }
    return NP_ERR_INTERNAL_DATA; /* the original returns 6 when the 25 ms mutex wait times out */
}

static void exe_name(char *out, size_t size)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    const char *base = path;
    for (DWORD i = 0; i < n; i++)
        if (path[i] == '\\' || path[i] == '/')
            base = path + i + 1;
    lstrcpynA(out, n ? base : "", (int)size);
}

/* ---- exports, in ordinal order of the original ---------------------------------------------------------------- */

NP_EXPORT int __stdcall NPPriv_ClientNotify(unsigned short field, unsigned short value)
{
    (void)field;
    (void)value;
    return g_bridge ? NP_OK : NP_ERR_INTERNAL_DATA;
}

NP_EXPORT int __stdcall NPPriv_GetLastError(void)
{
    return g_bridge ? (int)g_bridge->block.last_result : NP_ERR_INTERNAL_DATA;
}

NP_EXPORT int __stdcall NPPriv_SetData(np_trackir_data *data)
{
    (void)data; /* only TrackIR5.exe publishes; the daemon owns the data here */
    return tracker_running() ? NP_OK : NP_ERR_DEVICE_NOT_PRESENT;
}

NP_EXPORT void __stdcall NPPriv_SetLastError(int error)
{
    if (g_bridge)
        g_bridge->block.last_result = (uint32_t)error;
}

NP_EXPORT void __stdcall NPPriv_SetParameter(int index, int value)
{
    if (g_bridge && index >= 0 && index <= 2)
        g_bridge->block.param[index] = (uint32_t)value;
}

NP_EXPORT int __stdcall NPPriv_SetSignature(const void *signature)
{
    (void)signature;
    return g_bridge ? NP_OK : NP_ERR_INTERNAL_DATA;
}

NP_EXPORT void __stdcall NPPriv_SetVersion(unsigned short version)
{
    (void)version;
}

NP_EXPORT int __stdcall NP_GetData(np_trackir_data *data)
{
    return read_data(data);
}

/* Decrypts "TrackIR Enhanced" data with the caller's key, exactly like the original (0x10001410). */
NP_EXPORT int __stdcall NP_GetDataEX(np_trackir_data *data, unsigned int key_lo, unsigned int key_hi)
{
    int rc = read_data(data);
    if (rc != NP_OK)
        return rc;
    if (key_lo == 0 && key_hi == 0)
        return NP_ERR_NO_KEY;
    uint8_t key[8];
    for (int i = 0; i < 4; i++) {
        key[i] = (uint8_t)(key_lo >> (8 * i));
        key[4 + i] = (uint8_t)(key_hi >> (8 * i));
    }
    return np_data_decrypt(data, key);
}

NP_EXPORT int __stdcall NP_GetParameter(int index, int *value)
{
    if (!value)
        return NP_ERR_INVALID_ARG;
    if (!tracker_running())
        return NP_ERR_INTERNAL_DATA;
    if (index < 0 || index > 2)
        return NP_ERR_INVALID_ARG;
    *value = (int)g_bridge->block.param[index];
    return NP_OK;
}

NP_EXPORT int __stdcall NP_GetSignature(void *signature)
{
    if (!tracker_running())
        return NP_ERR_INTERNAL_DATA;
    memcpy(signature, g_bridge->block.sig_dll, sizeof(g_bridge->block.sig_dll) + sizeof(g_bridge->block.sig_app));
    return NP_OK;
}

NP_EXPORT int __stdcall NP_QueryVersion(unsigned short *version)
{
    *version = 0;
    if (!tracker_running())
        return NP_ERR_DEVICE_NOT_PRESENT;
    *version = g_bridge->block.version;
    return NP_OK;
}

NP_EXPORT int __stdcall NP_ReCenter(void)
{
    return send_command(NP_CMD_RECENTER, 0);
}

NP_EXPORT int __stdcall NP_RegisterProgramProfileID(unsigned short id)
{
    char exe[MAX_PATH];
    uint32_t profile = id;
    exe_name(exe, sizeof(exe));
    if (id == 0 && lstrcmpiA(exe, "LockOn.exe") == 0)
        profile = 0x3EA;
    else if (id == 0x3EA && lstrcmpiA(exe, "nks.exe") == 0)
        profile = 0x2A95;
    return send_command(NP_CMD_REGISTER_PROFILE_ID, profile);
}

NP_EXPORT int __stdcall NP_RegisterWindowHandle(HWND window)
{
    return send_command(NP_CMD_REGISTER_WINDOW, (uint32_t)(uintptr_t)window);
}

NP_EXPORT int __stdcall NP_RequestData(unsigned short fields)
{
    return send_command(NP_CMD_REQUEST_DATA, fields);
}

NP_EXPORT int __stdcall NP_SetParameter(int index, int value)
{
    if (index == 0 || index == 1)
        return NP_ERR_READ_ONLY;
    if (index == 2 && value == 0) {
        if (!g_bridge)
            return NP_ERR_INTERNAL_DATA;
        g_bridge->block.param[2] = 0;
        return NP_OK;
    }
    return NP_ERR_INVALID_ARG;
}

NP_EXPORT int __stdcall NP_StartCursor(void)
{
    return send_command(NP_CMD_START_CURSOR, 0);
}

/* Some old titles never register a profile id; the original fills it in from the executable name. */
NP_EXPORT int __stdcall NP_StartDataTransmission(void)
{
    static const struct { const char *exe; unsigned short id; } known[] = {
        {"SwgClient_r.exe", 0x1771}, {"targetware.exe", 0x4B2}, {"tir2joy.exe", 0x89A}, {"ww2.exe", 0xCE5},
        {"TIRF4.exe", 0x89B},        {"CRC_DEMO.exe", 0x12C1},  {"mf.exe", 0x13ED},     {"Trainz.exe", 0x125D},
    };
    char exe[MAX_PATH];
    exe_name(exe, sizeof(exe));
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (lstrcmpiA(exe, known[i].exe) == 0) {
            NP_RegisterProgramProfileID(known[i].id);
            break;
        }
    return send_command(NP_CMD_START_TRANSMISSION, 0);
}

NP_EXPORT int __stdcall NP_StopCursor(void)
{
    return send_command(NP_CMD_STOP_CURSOR, 0);
}

NP_EXPORT int __stdcall NP_StopDataTransmission(void)
{
    return send_command(NP_CMD_STOP_TRANSMISSION, 0);
}

NP_EXPORT int __stdcall NP_UnregisterWindowHandle(void)
{
    return send_command(NP_CMD_UNREGISTER_WINDOW, 0);
}

NP_EXPORT int __stdcall GetThreadExeName(char *name)
{
    exe_name(name, MAX_PATH);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        bridge_open();
    else if (reason == DLL_PROCESS_DETACH)
        bridge_close();
    return TRUE;
}
