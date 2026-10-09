/*
 * File-backed bridge between the macOS tracking daemon and the Windows NPClient.dll replacement running
 * under Wine/CrossOver. Both sides mmap the same file (Wine sees it as Z:\tmp\TrackIR-macOS.bridge).
 *
 * The daemon owns `block` (the exact 0x1E6-byte TrackIR shared block) and publishes it under a seqlock.
 * Clients send the NP_WM_COMMAND codes through a small ring instead of SendMessage.
 */
#ifndef TIR_BRIDGE_H
#define TIR_BRIDGE_H

#include "np_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIR_BRIDGE_MAGIC 0x31524954u /* "TIR1" */
#define TIR_BRIDGE_VERSION 1u
#define TIR_BRIDGE_UNIX_PATH "/tmp/TrackIR-macOS.bridge"
#define TIR_BRIDGE_WINE_PATH "Z:\\tmp\\TrackIR-macOS.bridge"
#define TIR_BRIDGE_ENV "TRACKIR_MACOS_BRIDGE"
#define TIR_BRIDGE_RING 32
#define TIR_BRIDGE_FILE_SIZE 4096

/* pose_flags */
#define TIR_POSE_TRACKING 1u /* the clip is visible and `pose` is current */
#define TIR_POSE_PAUSED 2u   /* the user paused tracking; `pose` is frozen */

typedef struct tir_bridge_command {
    volatile uint32_t sequence; /* index + 1 once code/arg are valid */
    uint32_t code;              /* enum np_command */
    uint32_t arg;
} tir_bridge_command;

typedef struct tir_bridge {
    uint32_t magic;
    uint32_t version;
    volatile uint32_t data_seq;     /* odd while the daemon writes `block` */
    volatile uint32_t heartbeat;    /* incremented by the daemon at least every 100 ms */
    volatile int32_t daemon_pid;
    volatile uint32_t tracking;     /* 1 while a pose is being produced */
    np_shared_block block;
    uint8_t pad0[2];
    volatile uint32_t cmd_head;     /* next ring index to be claimed by a client */
    tir_bridge_command cmds[TIR_BRIDGE_RING];

    /* For native macOS clients (the X-Plane plugin): the pose after the profile, unscaled, under its own seqlock.
     * Convention: yaw + = turn left, pitch + = look up, roll + = tilt left, x + = right, y + = up,
     * z + = towards the screen; degrees and centimetres. */
    volatile uint32_t pose_seq;   /* odd while the daemon writes `pose` */
    volatile uint32_t pose_flags; /* TIR_POSE_* */
    uint32_t pad1;
    double pose[6];               /* yaw, pitch, roll, x, y, z */
} tir_bridge;

#if !defined(_WIN32)
/* Queues an np_command for the daemon (what NPClient does with SendMessage on Windows). */
static inline void tir_bridge_push_command(tir_bridge *b, uint32_t code, uint32_t arg)
{
    uint32_t index = __atomic_fetch_add(&b->cmd_head, 1u, __ATOMIC_ACQ_REL);
    tir_bridge_command *slot = &b->cmds[index % TIR_BRIDGE_RING];
    slot->code = code;
    slot->arg = arg;
    __atomic_store_n(&slot->sequence, index + 1, __ATOMIC_RELEASE);
}

/* Copies the native pose; returns 0 if the daemon kept writing during every attempt. */
static inline int tir_bridge_read_pose(const tir_bridge *b, double pose[6], uint32_t *flags)
{
    for (int attempt = 0; attempt < 100; attempt++) {
        uint32_t before = __atomic_load_n(&b->pose_seq, __ATOMIC_ACQUIRE);
        if (before & 1u)
            continue;
        for (int i = 0; i < 6; i++)
            pose[i] = b->pose[i];
        *flags = b->pose_flags;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&b->pose_seq, __ATOMIC_RELAXED) == before)
            return 1;
    }
    return 0;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
