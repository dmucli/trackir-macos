#include "np_shared.h"

#include <string.h>

typedef char np_check_data_size[sizeof(np_trackir_data) == NP_DATA_SIZE ? 1 : -1];
typedef char np_check_block_size[sizeof(np_shared_block) == NP_SHARED_SIZE ? 1 : -1];
typedef char np_check_data_offset[offsetof(np_shared_block, data) == 0x1A2 ? 1 : -1];

const char NP_SIGNATURE_DLL[] =
    "precise head tracking\n put your head into the game\n now go look around\n\n Copyright EyeControl Technologies";
const char NP_SIGNATURE_APP[] =
    "hardware camera\n software processing data\n track user movement\n\n Copyright EyeControl Technologies";

void np_block_init_dll_defaults(np_shared_block *block)
{
    memset(block, 0, sizeof(*block));
    block->version = NP_VERSION_DLL_DEFAULT;
    memcpy(block->sig_dll, "Not Set DLL", 12);
    memcpy(block->sig_app, "Not Set App", 12);
}

void np_block_init_trackir(np_shared_block *block)
{
    block->version = NP_VERSION_TRACKIR_5_5;
    block->param[0] = 1;
    memset(block->sig_dll, 0, sizeof(block->sig_dll));
    memset(block->sig_app, 0, sizeof(block->sig_app));
    memcpy(block->sig_dll, NP_SIGNATURE_DLL, sizeof(NP_SIGNATURE_DLL));
    memcpy(block->sig_app, NP_SIGNATURE_APP, sizeof(NP_SIGNATURE_APP));
}

/* NPClient reads the 16-bit words as signed and shifts with sign extension (MSVC int >> n). */
static int32_t asr(uint32_t v, int n)
{
    return (int32_t)v >> n;
}

int32_t np_data_hash(const uint8_t data[NP_DATA_SIZE])
{
    uint32_t h = NP_DATA_SIZE;
    for (int i = 0; i < NP_DATA_SIZE; i += 4) {
        int32_t s0 = (int16_t)(data[i] | data[i + 1] << 8);
        int32_t s1 = (int16_t)(data[i + 2] | data[i + 3] << 8);
        uint32_t t = h + (uint32_t)s0;
        h = t ^ (((uint32_t)s1 ^ (t << 5)) << 11);
        h += (uint32_t)asr(h, 11);
    }
    h ^= h << 3;
    h += (uint32_t)asr(h, 5);
    h ^= h << 4;
    h += (uint32_t)asr(h, 17);
    h ^= h << 25;
    h += (uint32_t)asr(h, 6);
    return (int32_t)h;
}

void np_data_encrypt(np_trackir_data *data, const uint8_t key[8], uint32_t (*random32)(void))
{
    float *unused = &data->raw_x;
    for (int i = 0; i < 9; i++)
        unused[i] = (float)(random32() & 0x7FFF); /* MSVC rand() range */

    data->io_data = 0;
    data->io_data = (uint32_t)np_data_hash((const uint8_t *)data);

    uint8_t *bytes = (uint8_t *)data;
    uint8_t s = 0x88;
    int j = 0;
    for (int i = NP_DATA_SIZE - 1; i >= 0; i--) {
        uint8_t plain = bytes[i];
        bytes[i] = key[j] ^ plain ^ s;
        s = (uint8_t)(s + plain + i);
        j = (j + 1) & 7;
    }
}

int np_data_decrypt(np_trackir_data *data, const uint8_t key[8])
{
    uint8_t *bytes = (uint8_t *)data;
    uint8_t s = 0x88;
    int j = 0;
    for (int i = NP_DATA_SIZE - 1; i >= 0; i--) {
        uint8_t plain = key[j] ^ bytes[i] ^ s;
        bytes[i] = plain;
        s = (uint8_t)(s + plain + i);
        j = (j + 1) & 7;
    }

    int32_t stored = (int32_t)data->io_data;
    data->io_data = 0;
    int32_t computed = np_data_hash(bytes);
    memset(&data->raw_x, 0, 9 * sizeof(float));
    return stored == computed ? NP_OK : NP_ERR_CHECKSUM;
}
