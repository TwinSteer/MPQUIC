/*
 * Minimal round-robin path scheduler for the TwinSteer MP-QUIC reference
 * implementation.
 *
 * This file intentionally contains only one scheduling policy. The learning-
 * based TwinSteer scheduler is maintained separately from this repository.
 */

#include "scheduler.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <arpa/inet.h>
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define SCHED_ATOMIC_INT _Atomic int
#define SCHED_ATOMIC_U64 _Atomic uint64_t
#define SCHED_LOAD(x) atomic_load(&(x))
#define SCHED_STORE(x, v) atomic_store(&(x), (v))
#define SCHED_INC(x) atomic_fetch_add(&(x), 1)
#else
#define SCHED_ATOMIC_INT volatile int
#define SCHED_ATOMIC_U64 volatile uint64_t
#define SCHED_LOAD(x) (x)
#define SCHED_STORE(x, v) ((x) = (v))
#define SCHED_INC(x) ((x)++)
#endif

static char g_pc5_peer_ip[64] = "";
static uint32_t g_pc5_peer_addr = 0;

static SCHED_ATOMIC_INT g_last_sel_rat = -1;
static SCHED_ATOMIC_U64 g_sel_cnt_uu = 0;
static SCHED_ATOMIC_U64 g_sel_cnt_pc5 = 0;
static SCHED_ATOMIC_U64 g_rr_turn = 0;

void scheduler_set_pc5_peer_ip(const char* ip)
{
    if (ip == NULL) {
        return;
    }

    snprintf(g_pc5_peer_ip, sizeof(g_pc5_peer_ip), "%s", ip);
    g_pc5_peer_addr = 0;
#ifdef __linux__
    (void)inet_pton(AF_INET, g_pc5_peer_ip, &g_pc5_peer_addr);
#endif
}

static int path_is_pc5(const picoquic_path_t* path_x)
{
    if (path_x == NULL || path_x->first_tuple == NULL || g_pc5_peer_addr == 0) {
        return 0;
    }

    const struct sockaddr_in* peer =
        (const struct sockaddr_in*)&path_x->first_tuple->peer_addr;

    return peer->sin_family == AF_INET &&
           peer->sin_addr.s_addr == g_pc5_peer_addr;
}

static int path_is_usable(const picoquic_path_t* path_x)
{
    if (path_x == NULL || path_x->first_tuple == NULL) {
        return 0;
    }
    if (path_x->path_is_backup || path_x->path_is_demoted) {
        return 0;
    }
    if (!path_x->first_tuple->challenge_verified) {
        return 0;
    }
    return 1;
}

/* Data scheduling also respects picoquic pacing and congestion windows. */
static int path_is_data_ready(
    picoquic_cnx_t* cnx,
    picoquic_path_t* path_x,
    uint64_t current_time)
{
    uint64_t pacing_time_next = UINT64_MAX;

    if (!path_is_usable(path_x)) {
        return 0;
    }
    if (path_x->bytes_in_transit >= path_x->cwin ||
        path_x->bytes_in_transit >= cnx->quic->cwin_max) {
        return 0;
    }
    if (!picoquic_is_sending_authorized_by_pacing(
            cnx, path_x, current_time, &pacing_time_next)) {
        return 0;
    }
    return 1;
}

picoquic_path_t* scheduler_pick_path(
    picoquic_cnx_t* cnx,
    int default_idx,
    uint64_t current_time)
{
    picoquic_path_t* available[PICOQUIC_NB_PATH_TARGET];
    int available_count = 0;
    uint64_t turn;
    picoquic_path_t* selected;
    int selected_rat;

    if (cnx == NULL || cnx->nb_paths <= 0) {
        SCHED_STORE(g_last_sel_rat, -1);
        return NULL;
    }

    if (default_idx < 0 || default_idx >= cnx->nb_paths) {
        default_idx = 0;
    }

    for (int i = 0;
         i < cnx->nb_paths && available_count < PICOQUIC_NB_PATH_TARGET;
         i++) {
        if (path_is_data_ready(cnx, cnx->path[i], current_time)) {
            available[available_count++] = cnx->path[i];
        }
    }

    if (available_count == 0) {
        selected = cnx->path[default_idx];
    }
    else {
        turn = SCHED_LOAD(g_rr_turn);
        selected = available[turn % (uint64_t)available_count];
        SCHED_STORE(g_rr_turn, turn + 1);
    }

    selected_rat = path_is_pc5(selected) ? 1 : 0;
    SCHED_STORE(g_last_sel_rat, selected_rat);
    if (selected_rat == 1) {
        SCHED_INC(g_sel_cnt_pc5);
    }
    else {
        SCHED_INC(g_sel_cnt_uu);
    }

    return selected;
}

int scheduler_get_last_sel_rat(void)
{
    return SCHED_LOAD(g_last_sel_rat);
}

void scheduler_get_sel_counts(uint64_t* uu, uint64_t* pc5)
{
    if (uu != NULL) {
        *uu = SCHED_LOAD(g_sel_cnt_uu);
    }
    if (pc5 != NULL) {
        *pc5 = SCHED_LOAD(g_sel_cnt_pc5);
    }
}

void scheduler_path_status(
    picoquic_cnx_t* cnx,
    scheduler_path_info_t* uu,
    scheduler_path_info_t* pc5)
{
    if (uu != NULL) {
        memset(uu, 0, sizeof(*uu));
        uu->path_index = -1;
    }
    if (pc5 != NULL) {
        memset(pc5, 0, sizeof(*pc5));
        pc5->path_index = -1;
    }
    if (cnx == NULL) {
        return;
    }

    for (int i = 0; i < cnx->nb_paths; i++) {
        picoquic_path_t* path_x = cnx->path[i];
        scheduler_path_info_t* slot;
        int usable;

        if (path_x == NULL || path_x->first_tuple == NULL) {
            continue;
        }

        slot = path_is_pc5(path_x) ? pc5 : uu;
        if (slot == NULL) {
            continue;
        }

        usable = path_is_usable(path_x);
        if (slot->present && !usable && slot->usable) {
            continue;
        }

        slot->present = 1;
        slot->usable = usable;
        slot->demoted = path_x->path_is_demoted ? 1 : 0;
        slot->verified = path_x->first_tuple->challenge_verified ? 1 : 0;
        slot->path_index = i;
        slot->smoothed_rtt = path_x->smoothed_rtt;
        slot->rtt_sample = path_x->rtt_sample;
    }
}
