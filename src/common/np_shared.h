/*
 * The NPClient shared-memory contract, as reverse engineered from NPClient.dll 5.5.3.
 * Plain C so it builds both into the macOS daemon and into the Windows (Wine) NPClient DLL.
 * See docs/PROTOCOL.md section 1.
 */
#ifndef NP_SHARED_H
#define NP_SHARED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_MAPPING_NAME "Local\\SharedTrackIRData"
#define NP_MUTEX_NAME "NPClientMutex"
#define NP_WINDOW_NAME "NaturalPoint"
#define NP_WM_COMMAND 0x405 /* WM_USER + 5 */

/* wParam values sent with NP_WM_COMMAND */
enum np_command {
    NP_CMD_REGISTER_PROFILE_ID = 0x3F2,
    NP_CMD_REGISTER_WINDOW = 0x3FC,
    NP_CMD_UNREGISTER_WINDOW = 0x3FD,
    NP_CMD_REQUEST_DATA = 0x406,
    NP_CMD_START_TRANSMISSION = 0x7DA,
    NP_CMD_STOP_TRANSMISSION = 0x7E4,
    NP_CMD_START_CURSOR = 0xBC2,
    NP_CMD_STOP_CURSOR = 0xBCC,
    NP_CMD_RECENTER = 0xBD6
};

/* Return codes used by the NP_* exports */
enum np_result {
    NP_OK = 0,
    NP_ERR_DEVICE_NOT_PRESENT = 1,
    NP_ERR_UNSUPPORTED_OS = 2,
    NP_ERR_INVALID_ARG = 3,
    NP_ERR_DLL_NOT_FOUND = 4,
    NP_ERR_NO_DATA = 5,
    NP_ERR_INTERNAL_DATA = 6,
    NP_ERR_READ_ONLY = 9,
    NP_ERR_NO_KEY = 0x65,
    NP_ERR_CHECKSUM = 100
};

/* Bits of TrackIRData.status / NP_RequestData mask */
enum np_data_field {
    NP_ROLL = 0x0001,
    NP_PITCH = 0x0002,
    NP_YAW = 0x0004,
    NP_X = 0x0010,
    NP_Y = 0x0020,
    NP_Z = 0x0040,
    NP_RAW_X = 0x0080,
    NP_RAW_Y = 0x0100,
    NP_RAW_Z = 0x0200,
    NP_DELTA_X = 0x0400,
    NP_DELTA_Y = 0x0800,
    NP_DELTA_Z = 0x1000,
    NP_SMOOTH_X = 0x2000,
    NP_SMOOTH_Y = 0x4000,
    NP_SMOOTH_Z = 0x8000
};

#define NP_VERSION_TRACKIR_5_5 0x0505
#define NP_VERSION_DLL_DEFAULT 0x0100
#define NP_AXIS_LIMIT 16383.0f

#pragma pack(push, 1)
typedef struct np_trackir_data {
    uint16_t status;
    uint16_t frame_signature;
    uint32_t io_data;
    float roll, pitch, yaw;
    float x, y, z;
    float raw_x, raw_y, raw_z;
    float delta_x, delta_y, delta_z;
    float smooth_x, smooth_y, smooth_z;
} np_trackir_data;

typedef struct np_shared_block {
    uint16_t version;
    uint32_t last_result;
    uint32_t param[3];
    char sig_dll[200];
    char sig_app[200];
    np_trackir_data data;
} np_shared_block;
#pragma pack(pop)

#define NP_DATA_SIZE 0x44
#define NP_SHARED_SIZE 0x1E6

extern const char NP_SIGNATURE_DLL[];
extern const char NP_SIGNATURE_APP[];

/* Fill a block the way NPClient.dll does when it creates the mapping. */
void np_block_init_dll_defaults(np_shared_block *block);
/* Fill the version/parameter/signature fields the way TrackIR5.exe does. */
void np_block_init_trackir(np_shared_block *block);

/* SuperFastHash variant over the 68 data bytes (io_data must be zero). */
int32_t np_data_hash(const uint8_t data[NP_DATA_SIZE]);

/* TrackIR side: randomise the unused fields, store the hash, encrypt with an 8-byte key. */
void np_data_encrypt(np_trackir_data *data, const uint8_t key[8], uint32_t (*random32)(void));
/* Game side (NP_GetDataEX): decrypt, wipe raw/delta/smooth, verify. Returns NP_OK or NP_ERR_CHECKSUM. */
int np_data_decrypt(np_trackir_data *data, const uint8_t key[8]);

#ifdef __cplusplus
}
#endif

#endif
