#ifndef TWINSTEER_MPQUIC_SCHEDULER_H
#define TWINSTEER_MPQUIC_SCHEDULER_H

#include <stdint.h>
#include "picoquic_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal path information exposed to the reference application.
 * Values are snapshots of picoquic's own path state.
 */
typedef struct scheduler_path_info {
    int present;
    int usable;
    int demoted;
    int verified;
    int path_index;
    uint64_t smoothed_rtt;
    uint64_t rtt_sample;
} scheduler_path_info_t;

/* Configure the peer IPv4 address used to identify the PC5 path. */
void scheduler_set_pc5_peer_ip(const char* ip);

/*
 * Round-robin packet-path selection hook.
 * Only paths that are verified, non-backup, and non-demoted are considered.
 */
picoquic_path_t* scheduler_pick_path(
    picoquic_cnx_t* cnx,
    int default_idx,
    uint64_t current_time);

/* Last path selected by the round-robin scheduler: 0=Uu, 1=PC5, -1=unknown. */
int scheduler_get_last_sel_rat(void);

/* Cumulative round-robin selections per access path. */
void scheduler_get_sel_counts(uint64_t* uu, uint64_t* pc5);

/* Snapshot Uu/PC5 path state from picoquic. */
void scheduler_path_status(
    picoquic_cnx_t* cnx,
    scheduler_path_info_t* uu,
    scheduler_path_info_t* pc5);

#ifdef __cplusplus
}
#endif

#endif /* TWINSTEER_MPQUIC_SCHEDULER_H */
