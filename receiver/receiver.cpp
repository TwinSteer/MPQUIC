/*
 * V2X MP-QUIC Receiver (수신/로그 전용)
 *
 *   [터미널] 1초 주기 상태 블록 + 이벤트 로그   ← 이번 수정
 *
 * NOTE: CSV 로깅 코드는 원본 그대로다 (metrics 는 2초 주기 유지).
 *       화면 출력만 별도 스레드로 분리했다.
 *       추가 헤더 파일 없음 — 출력 유틸은 이 파일 안에 들어있다.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <optional>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_map>
#include <deque>

extern "C"{

    // 메시지 타입 헤더 — 반드시 이 순서대로, seamless_app_v2x_client.h 보다 먼저
    #include "etf-types.h"
    #include "etf_ksr1600_j2735_enum.h"
    #include "etf_sdk_wsmp_psid_kr.h"
    #include "etf_j2540.h"                     // ★ 추가: J2540_ITIScodes 등 정의
    #include "etf_ksr1600_j2735_data_element.h"
    #include "etf_ksr1600_kor_data_element.h"
    #include "etf_ksr1600_j2735_data_frame.h"
    #include "etf_ksr1600_kor_data_frame.h"
    #include "etf_ksr1600_j2735_message.h"
    #include "etf_j3224.h"

    #include "seamless_app_v2x_client.h"

    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/time.h>

    #include "picoquic.h"
    #include "picosocks.h"
    #include "picoquic_utils.h"
    #include "picoquic_internal.h"
}

// ═════════════════════════════════════════════════════════════
//  터미널 출력 유틸 (화면 전용 — CSV 와 무관)
//
//    ●ACTIVE   지금 이 경로로 데이터가 흐름 (구간 수신율 ≥ 5 RTP/s)
//    ○standby  keepalive/ACK 만 오는 중 — 경로는 살아있으나 데이터는 없음
//    ?unknown  3초 이상 무수신. 수신측은 "미사용"과 "장애"를 구분할 수 없다
//    -none-    한 번도 수신 없음
// ═════════════════════════════════════════════════════════════
static bool g_color = true;
static inline const char* CC(const char* s) { return g_color ? s : ""; }
static inline const char* cRST() { return CC("\033[0m");  }
static inline const char* cBLD() { return CC("\033[1m");  }
static inline const char* cDIM() { return CC("\033[2m");  }
static inline const char* cRED() { return CC("\033[31m"); }
static inline const char* cGRN() { return CC("\033[32m"); }
static inline const char* cYEL() { return CC("\033[33m"); }
static inline const char* cCYN() { return CC("\033[36m"); }
static inline const char* cGRY() { return CC("\033[90m"); }

// 경로별 QUIC 패킷 수신 — recv 스레드에서 직접 세므로 100% 정확하다.
// (RTP 카운트는 스트림 버퍼 파싱 시점에 경로를 추정하므로 오태깅이 있다)
static std::atomic<uint64_t> g_rxp_uu{0},  g_rxp_pc5{0};
static std::atomic<uint64_t> g_rxb_uu{0},  g_rxb_pc5{0};

static uint64_t g_ui_t0 = 0;

static inline uint64_t ui_wall_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}
static inline void   ui_start()     { g_ui_t0 = ui_wall_us(); }
static inline double ui_elapsed_s() {
    return g_ui_t0 ? (ui_wall_us() - g_ui_t0) / 1e6 : 0.0;
}

static thread_local char g_ui_pool[8][40];
static thread_local int  g_ui_pi = 0;
static inline char* ui_buf() { g_ui_pi = (g_ui_pi + 1) & 7; return g_ui_pool[g_ui_pi]; }

static inline const char* ui_stamp() {
    static thread_local char b[48];
    time_t now = time(nullptr); struct tm tmv; localtime_r(&now, &tmv);
    snprintf(b, sizeof(b), "[+%06.1fs|%02d:%02d:%02d]",
             ui_elapsed_s(), tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}
static inline const char* ui_stamp_s() {
    static thread_local char b[24];
    snprintf(b, sizeof(b), "[+%06.1fs]", ui_elapsed_s());
    return b;
}

enum UiPathState { PS_ACTIVE, PS_STANDBY, PS_UNKNOWN, PS_DEAD, PS_NONE };

static inline const char* ui_state(UiPathState s) {
    static thread_local char b[2][64];
    static thread_local int  i = 0;
    i ^= 1;
    const char* lbl; const char* col;
    switch (s) {
        case PS_ACTIVE:  lbl = "\u25CFACTIVE "; col = cGRN(); break;  // ●
        case PS_STANDBY: lbl = "\u25CBstandby"; col = cYEL(); break;  // ○
        case PS_UNKNOWN: lbl = "?unknown";      col = cGRY(); break;
        case PS_DEAD:    lbl = "\u2717DEAD   "; col = cRED(); break;  // ✗
        default:         lbl = "-none-  ";      col = cGRY(); break;
    }
    snprintf(b[i], sizeof(b[i]), "%s%s%s", col, lbl, cRST());
    return b[i];
}

static inline const char* ui_age(uint64_t last_us, uint64_t now) {
    char* b = ui_buf();
    if (last_us == 0)        snprintf(b, 40, "--");
    else if (now <= last_us) snprintf(b, 40, "0.0s");   // 미래 시각 → 0 클램프
    else                     snprintf(b, 40, "%.1fs", (now - last_us) / 1e6);
    return b;
}

static void ui_ev(const char* col, const char* icon, const char* fmt, ...) {
    char m[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    printf("  %s%s%s %s%s%s %s\n",
           col, icon, cRST(), cDIM(), ui_stamp_s(), cRST(), m);
    fflush(stdout);
}
#define UI_OK(...)   ui_ev(cGRN(), "\u2713", __VA_ARGS__)   // ✓
#define UI_BAD(...)  ui_ev(cRED(), "\u2717", __VA_ARGS__)   // ✗
#define UI_WARN(...) ui_ev(cYEL(), "\u26A0", __VA_ARGS__)   // ⚠
#define UI_SW(...)   ui_ev(cCYN(), "\u21C4", __VA_ARGS__)   // ⇄

static void ui_rule() {
    char l[96]; memset(l, '=', 79); l[79] = 0;
    printf("%s%s%s\n", cDIM(), l, cRST());
}

// 구간 처리율
struct UiRate {
    uint64_t pb = 0, pt = 0;
    double mbps(uint64_t cur, uint64_t now) {
        double r = 0.0;
        if (pt && now > pt && cur >= pb)
            r = (cur - pb) * 8.0 / ((now - pt) / 1e6) / 1e6;
        pb = cur; pt = now; return r;
    }
};

// 지연 분포용 링버퍼 (락 없음 — 표시 전용이라 약간의 경합 허용)
struct UiDelayRing {
    static const int N = 4096;
    float                 v[N];
    std::atomic<uint32_t> n;
    UiDelayRing() : n(0) { memset(v, 0, sizeof(v)); }
    void add(float ms) {
        uint32_t i = n.fetch_add(1, std::memory_order_relaxed);
        v[i % N] = ms;
    }
    bool stats(double& p50, double& p95) const {
        uint32_t total = n.load(std::memory_order_relaxed);
        if (total == 0) return false;
        int k = (int)std::min<uint32_t>(total, N);
        std::vector<float> s(v, v + k);
        std::sort(s.begin(), s.end());
        p50 = s[(size_t)(k * 0.50)];
        p95 = s[(size_t)std::min(k - 1, (int)(k * 0.95))];
        return true;
    }
};

// ── CSV 파일 ─────────────────────────────────────────────────
static FILE*      g_metrics_csv  = nullptr;  // 2초 주기 상태
static FILE*      g_events_csv   = nullptr;  // 경로 이벤트
static FILE*      g_freeze_csv   = nullptr;  // freeze(수신 끊김) 이벤트
static std::mutex g_csv_mtx;

static inline uint64_t now_us();  // 전방 선언

static void csv_write_event(const char* event, const char* path, const char* detail) {
    std::lock_guard<std::mutex> lk(g_csv_mtx);
    if (!g_events_csv) return;
    fprintf(g_events_csv, "%llu,%s,%s,%s\n",
            (unsigned long long)now_us(), event, path, detail);
    fflush(g_events_csv);
}

std::atomic<bool> keepRunning(true);
std::mutex quic_mutex;

static std::vector<uint8_t> g_recvBuf_s4;
static std::vector<uint8_t> g_recvBuf_s8;

// ── freeze(수신 끊김) 감지 ────────────────────────────────────
static uint64_t g_last_rtp_rx_us  = 0;
static std::mutex g_freeze_mtx;
static const uint64_t FREEZE_THRESHOLD_US = 500000ULL; // 500ms

// ── FrameMeta ────────────────────────────────────────────────
#pragma pack(push, 1)
struct FrameMetaPacket {
    uint64_t frame_id;
    uint64_t t_tx_us;
    uint8_t  scheduler_id;
    float    speed_mps;
    double   lat;
    double   lon;
    double    heading_deg;
    uint8_t  gps_fixed;
};

struct RtpPacketEnvelope {
    uint64_t packet_seq;
    uint64_t frame_id;
    uint64_t t_tx_us;
    uint32_t rtp_len;
    uint8_t  tx_path;
};

struct MetaSnapshot {
    float speed_mps = 0.0f;
    double lat = 0.0;
    double lon = 0.0;
    double heading_deg = 0.0;
    bool gps_fixed = false;
};
#pragma pack(pop)

// ── 경로별 수신 통계 (콘솔 출력용) ──────────────────────────
struct PathStats {
    std::atomic<uint64_t> pkt_count{0};
    std::atomic<uint64_t> rtp_count{0};
    std::atomic<uint64_t> rtp_bytes{0};    // 처리량 계산용
    std::atomic<double>   last_lat_ms{0};
    std::atomic<uint64_t> last_rx_us{0};
};
static PathStats g_stats_uu, g_stats_pc5;
static std::atomic<uint64_t> g_freeze_count{0};
static std::mutex g_meta_mtx;
static std::unordered_map<uint64_t, MetaSnapshot> g_meta_by_frame;
static std::deque<uint64_t> g_meta_order;              // ★ 추가: frame_id 삽입 순서 기록
static const size_t META_CACHE_MAX = 512;
static FILE* g_packet_csv = nullptr;
static std::atomic<double> g_uu_kbps_snapshot{0.0};
static std::atomic<double> g_pc5_kbps_snapshot{0.0};
static std::atomic<int> g_uu_rssi_snapshot{0};
static std::atomic<int> g_pc5_rssi_snapshot{0};
// ── [터미널] 화면 출력용 (CSV 와 무관) ───────────────────────
static UiDelayRing g_delay_uu, g_delay_pc5;
static std::atomic<uint64_t> g_seq_next{0};        // 다음에 기대하는 packet_seq
static std::atomic<uint64_t> g_seq_lost{0};        // 건너뛴 seq 수
static std::atomic<uint64_t> g_seq_reorder{0};     // 역전 수
static std::atomic<uint64_t> g_frame_meta_cnt{0};
static std::string g_exp_name_disp = "default";

// ── 물리 인터페이스별 전체 QUIC 송신 카운터 ─────────────────
static std::atomic<uint64_t> g_txp_uu{0},   g_txp_pc5{0};
static std::atomic<uint64_t> g_txb_uu{0},   g_txb_pc5{0};

// 경로 생존 판단: 3초간 패킷 없으면 dead
static const uint64_t PATH_DEAD_US = 3000000ULL;

// [제거됨] PC5 RTT Ping/Pong 구조체 — QUIC 레벨 keepalive 로 대체

static picoquic_cnx_t *g_server_cnx = nullptr;
static std::atomic<int> g_incoming_path{-1}; // 0=Uu, 1=PC5

struct sockaddr_storage pc5_remote_addr;
struct sockaddr_storage pc5_local_addr;

static void getPathAckCounts(uint64_t& ack_uu, uint64_t& ack_pc5)
{
    ack_uu = 0;
    ack_pc5 = 0;

    std::lock_guard<std::mutex> lk(quic_mutex);

    picoquic_cnx_t* cnx = g_server_cnx;
    if (cnx == nullptr) {
        return;
    }

    struct sockaddr_in* pc5 = (struct sockaddr_in*)&pc5_remote_addr;

    for (int i = 0; i < cnx->nb_paths; i++) {
        picoquic_path_t* path_x = cnx->path[i];

        if (path_x == nullptr ||
            path_x->first_tuple == nullptr) {
            continue;
        }

        struct sockaddr_in* peer =
            (struct sockaddr_in*)&path_x->first_tuple->peer_addr;

        /*
         * PC5 peer 주소와 같으면 PC5 path.
         * 나머지는 Uu path로 계산.
         */
        if (peer->sin_family == AF_INET &&
            peer->sin_addr.s_addr == pc5->sin_addr.s_addr &&
            peer->sin_port == pc5->sin_port) {

            ack_pc5 += path_x->nb_path_ack_sent;
        }
        else {
            ack_uu += path_x->nb_path_ack_sent;
        }
    }
}

#define PKT_TYPE_RTP        0x01
#define PKT_TYPE_FRAME_META 0x04

static char g_pc5_sender_ip[64]   = "10.254.52.19";
static char g_pc5_receiver_ip[64] = "10.254.52.18";
static char g_uu_receiver_ip[64]  = "100.73.62.97";
static int  g_uu_receiver_port    = 36230;

#define PC5_SENDER_IP    g_pc5_sender_ip
#define PC5_RECEIVER_IP  g_pc5_receiver_ip
#define UU_RECEIVER_IP   g_uu_receiver_ip
#define UU_RECEIVER_PORT g_uu_receiver_port

// draft-21 §2.1 initial_max_path_id — sender 와 같은 값이어야 한다.
#define MAX_PATH_ID      64

// 클럭 오차 허용 범위
static const uint64_t CLOCK_TOLERANCE_US = 50000ULL;

static int g_uu_sock = -1;


static inline uint64_t now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// static inline bool uplink_valid(uint64_t rx, uint64_t tx) {
//     if (rx == 0) return false;
//     return (rx >= tx) ? true : ((tx - rx) < CLOCK_TOLERANCE_US);
// }

// static inline double uplink_ms(uint64_t rx, uint64_t tx) {
//     return (rx >= tx ? (rx - tx) : (tx - rx)) / 1000.0;
// }

static inline bool uplink_valid(uint64_t rx, uint64_t tx) {
    if (rx == 0) return false;
    return true; // 
}

static inline double uplink_ms(uint64_t rx, uint64_t tx) {
    // 실제 시간 차이가 얼마인지 그대로 계산해서 출력
    return (rx >= tx ? (rx - tx) : (tx - rx)) / 1000.0;
}

// ── 스트림 버퍼 처리 ─────────────────────────────────────────
// [수정] KVS/GStreamer로 넘기는 부분 삭제 - 수신량/지연/freeze만 집계
static void logRtpPacket(const RtpPacketEnvelope& env, uint64_t rx_us, int path, uint32_t pktLen, bool is_redundant) {
    // ── [터미널] seq 연속성 집계 (CSV 와 무관) ──
    if (!is_redundant) {
        uint64_t expect = g_seq_next.load();
        if (expect != 0 && env.packet_seq > expect)
            g_seq_lost.fetch_add(env.packet_seq - expect);
        else if (env.packet_seq < expect)
            g_seq_reorder.fetch_add(1);
        if (env.packet_seq + 1 > expect)
            g_seq_next.store(env.packet_seq + 1);
    }

    double delay_ms = 0.0;
    bool valid = uplink_valid(rx_us, env.t_tx_us);
    if (valid) delay_ms = uplink_ms(rx_us, env.t_tx_us);

    // ── [터미널] 지연 분포 집계 ──
    if (valid) {
        if (path == 0)      g_delay_uu.add((float)delay_ms);
        else if (path == 1) g_delay_pc5.add((float)delay_ms);
    }

    // In the RR reference implementation, consecutive RTP packets may alternate
    // paths by design. Do not interpret per-packet path changes as handovers.

    MetaSnapshot meta;
    {
        std::lock_guard<std::mutex> lk(g_meta_mtx);
        auto it = g_meta_by_frame.find(env.frame_id);
        if (it != g_meta_by_frame.end()) meta = it->second;
    }

    const char* net = path == 0 ? "Uu" : path == 1 ? "PC5" : "?";
    int uu_rssi = g_uu_rssi_snapshot.load();
    int pc5_rssi = g_pc5_rssi_snapshot.load();
    double uu_kbps = g_uu_kbps_snapshot.load();
    double pc5_kbps = g_pc5_kbps_snapshot.load();

    std::lock_guard<std::mutex> lk(g_csv_mtx);
    if (g_packet_csv) {
        fprintf(g_packet_csv,
            "%llu,%llu,%llu,%llu,%.3f,%u,%s,%d,%.0f,%d,%.0f,%.1f,%.6f,%.6f,%.3f,%d\n",
            (unsigned long long)env.packet_seq,
            (unsigned long long)env.frame_id,
            (unsigned long long)env.t_tx_us,
            (unsigned long long)rx_us,
            valid ? delay_ms : -1.0,
            pktLen, net, uu_rssi, uu_kbps, pc5_rssi, pc5_kbps, meta.speed_mps,
            meta.lat, meta.lon, meta.heading_deg, meta.gps_fixed ? 1 : 0);
        fflush(g_packet_csv);
    }
}

static void processStreamBuf(std::vector<uint8_t>& buf, bool is_redundant) {
    while (buf.size() >= 5) {
        uint8_t type = buf[0];
        uint32_t net_len;
        memcpy(&net_len, buf.data() + 1, 4);
        uint32_t pktLen = ntohl(net_len);
        if (pktLen > 1000000) { buf.clear(); break; }
        if (buf.size() < 5 + pktLen) break;

        int path = g_incoming_path.load();
        uint64_t t = now_us();

        if (type == PKT_TYPE_RTP) {
            if (pktLen >= sizeof(RtpPacketEnvelope)) {
                RtpPacketEnvelope env;
                memcpy(&env, buf.data() + 5, sizeof(env));
                if (env.tx_path <= 1) path = (int)env.tx_path;
                uint32_t actual_rtp_len = pktLen - (uint32_t)sizeof(RtpPacketEnvelope);
                if (env.rtp_len <= actual_rtp_len) actual_rtp_len = env.rtp_len;

                if (path == 0) {
                    g_stats_uu.rtp_count++;
                    g_stats_uu.rtp_bytes += actual_rtp_len;
                    g_stats_uu.last_rx_us.store(t);
                } else if (path == 1) {
                    g_stats_pc5.rtp_count++;
                    g_stats_pc5.rtp_bytes += actual_rtp_len;
                    g_stats_pc5.last_rx_us.store(t);
                }

                logRtpPacket(env, t, path, actual_rtp_len, is_redundant);
            }

            if (!is_redundant) {
                std::lock_guard<std::mutex> lk(g_freeze_mtx);
                if (g_last_rtp_rx_us > 0 && t > g_last_rtp_rx_us) {
                    uint64_t gap = t - g_last_rtp_rx_us;
                    if (gap > FREEZE_THRESHOLD_US) {
                        g_freeze_count++;
                        double freeze_ms = gap / 1000.0;
                        std::lock_guard<std::mutex> cl(g_csv_mtx);
                        if (g_freeze_csv) {
                            fprintf(g_freeze_csv, "%llu,%.3f,%s\n",
                                    (unsigned long long)g_last_rtp_rx_us, freeze_ms,
                                    path == 0 ? "Uu" : path == 1 ? "PC5" : "?");
                            fflush(g_freeze_csv);
                        }
                    }
                }
                g_last_rtp_rx_us = t;
            }
        } else if (type == PKT_TYPE_FRAME_META) {
            if (pktLen == sizeof(FrameMetaPacket)) {
                g_frame_meta_cnt.fetch_add(1);          // [터미널]
                FrameMetaPacket fm;
                memcpy(&fm, buf.data() + 5, sizeof(fm));
                {
                    std::lock_guard<std::mutex> lk(g_meta_mtx);
                    if (g_meta_by_frame.find(fm.frame_id) == g_meta_by_frame.end()) {
                        g_meta_order.push_back(fm.frame_id);
                    }
                    g_meta_by_frame[fm.frame_id] = {fm.speed_mps, fm.lat, fm.lon, fm.heading_deg, fm.gps_fixed != 0};

                    while (g_meta_by_frame.size() > META_CACHE_MAX) {
                        uint64_t oldest = g_meta_order.front();
                        g_meta_order.pop_front();
                        g_meta_by_frame.erase(oldest);
                    }
                }
                if (uplink_valid(t, fm.t_tx_us)) {
                    double lat = uplink_ms(t, fm.t_tx_us);
                    if (path == 0) {
                        g_stats_uu.pkt_count++;
                        g_stats_uu.last_lat_ms.store(lat);
                        g_stats_uu.last_rx_us.store(t);
                    } else if (path == 1) {
                        g_stats_pc5.pkt_count++;
                        g_stats_pc5.last_lat_ms.store(lat);
                        g_stats_pc5.last_rx_us.store(t);
                    }
                }
            }
        }
        buf.erase(buf.begin(), buf.begin() + 5 + pktLen);
    }
}

int customQuicCallback(picoquic_cnx_t *cnx,
                       uint64_t stream_id, uint8_t *bytes, size_t length,
                       picoquic_call_back_event_t event,
                       void *ctx, void *sctx) {
    g_server_cnx = cnx;
    if (event == picoquic_callback_stream_data && length > 0) {
        if (stream_id == 8) {
            g_recvBuf_s8.insert(g_recvBuf_s8.end(), bytes, bytes + length);
            processStreamBuf(g_recvBuf_s8, true);
        } else {
            g_recvBuf_s4.insert(g_recvBuf_s4.end(), bytes, bytes + length);
            processStreamBuf(g_recvBuf_s4, false);
        }
    } else if (event == picoquic_callback_ready) {
        UI_OK("QUIC 연결 성공 (multipath=%d)", cnx->is_multipath_enabled);
    } else if (event == picoquic_callback_close ||
               event == picoquic_callback_application_close) {
        UI_BAD("QUIC 연결 종료");
    }
    return 0;
}

void dispatchQuicPacket(uint8_t *data, size_t len,
                        struct sockaddr_storage *peer_addr,
                        struct sockaddr_storage *local_addr) {

    struct sockaddr_in *ps = (struct sockaddr_in *)peer_addr;
    struct sockaddr_in *pc5= (struct sockaddr_in *)&pc5_remote_addr;

    if (ps->sin_addr.s_addr == pc5->sin_addr.s_addr &&
        ps->sin_port        == pc5->sin_port) {
        g_txp_pc5.fetch_add(1);
        g_txb_pc5.fetch_add(len);
        Seamless_SendPc5Raw((uint8_t*)data, (uint16_t)len);

    } else {
        g_txp_uu.fetch_add(1);
        g_txb_uu.fetch_add(len);
        if (g_uu_sock >= 0)
            sendto(g_uu_sock, data, len, 0,
                   (struct sockaddr *)peer_addr, sizeof(struct sockaddr_in));
    }
}

bool initSirius(const char* pc5_ip) {
    int ret = Seamless_InitializePc5Raw((char*)pc5_ip);
    if (ret <= 0) {
        UI_BAD("SIRIUS SDK 연결 실패 (%s)  ret=%d", pc5_ip, ret);
        return false;
    }
    UI_OK("SIRIUS SDK 연결 (%s)", pc5_ip);
    return true;
}

int initUuSocket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int opt = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, UU_RECEIVER_IP, &addr.sin_addr);
    addr.sin_port = htons(UU_RECEIVER_PORT);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock); return -1;
        }
        UI_WARN("Uu 소켓 INADDR_ANY:%d 로 바인드 (%s 사용 불가)",
                UU_RECEIVER_PORT, UU_RECEIVER_IP);
    } else {
        UI_OK("Uu 소켓 바인드 %s:%d", UU_RECEIVER_IP, UU_RECEIVER_PORT);
    }
    return sock;
}

void initAddresses() {
    memset(&pc5_remote_addr, 0, sizeof(pc5_remote_addr));
    memset(&pc5_local_addr,  0, sizeof(pc5_local_addr));
    struct sockaddr_in *r = (struct sockaddr_in*)&pc5_remote_addr;
    r->sin_family = AF_INET;
    inet_pton(AF_INET, PC5_SENDER_IP, &r->sin_addr);
    r->sin_port = htons(4433);
    struct sockaddr_in *l = (struct sockaddr_in*)&pc5_local_addr;
    l->sin_family = AF_INET;
    inet_pton(AF_INET, PC5_RECEIVER_IP, &l->sin_addr);
    l->sin_port = htons(9999);
}

picoquic_quic_t *initQuic() {
    uint64_t t = picoquic_current_time();
    picoquic_quic_t *quic = picoquic_create(
        8, "../certs/cert.pem", "../certs/key.pem", NULL, "v2x_test",
        customQuicCallback, NULL, NULL, NULL, NULL,
        t, NULL, NULL, NULL, 0);
    if (!quic) return nullptr;
    quic->default_multipath_option = 1;
    // draft-21 §2.1 — 실제 한도는 양쪽 광고값 중 작은 쪽이 지배하므로
    // sender 의 MAX_PATH_ID 와 반드시 같은 값으로 올려야 한다.
    quic->default_tp.initial_max_path_id = MAX_PATH_ID;
    return quic;
}

// ===================== 스레드 =====================
void inputThread() {
    char c;
    while (keepRunning) { std::cin >> c; if (c=='q'||c=='Q') keepRunning=false; }
}

void pc5RecvThread(picoquic_quic_t *quic) {
    uint8_t rxBuf[9100] = {0};
    while (keepRunning) {
        // 1. 새 SDK 수신 함수 (데이터 길이 반환)
        int msgLen = Seamless_RecvPc5Raw(rxBuf);
        if (msgLen <= 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // [제거됨] 0xEE ping/pong 응답.
        //   QUIC 롱헤더의 첫 바이트는 0x80~0xFF 범위라 0xEE 가 정상적인
        //   QUIC 패킷일 수 있다. 그 패킷을 ping 으로 오인해 삼키면
        //   핸드셰이크/재전송이 조용히 유실된다.
        //   유휴 경로 생존 확인은 이제 sender 의 QUIC 레벨 keepalive 가
        //   담당하므로(draft-21 §5.9) 이 분기는 필요 없다.

        // 2. QUIC으로 데이터 넘기기
        std::lock_guard<std::mutex> lk(quic_mutex);
        g_rxp_pc5.fetch_add(1);
        g_rxb_pc5.fetch_add((uint64_t)msgLen);
        g_incoming_path.store(1);
        g_stats_pc5.last_rx_us.store(now_us());   // [추가] ACK 전용 패킷도 반영
        picoquic_incoming_packet(quic, rxBuf, msgLen,
            (struct sockaddr*)&pc5_remote_addr,
            (struct sockaddr*)&pc5_local_addr,
            0, 0, picoquic_current_time());
    }
}


void uuRecvThread(picoquic_quic_t *quic) {
    uint8_t rxBuf[2048];
    while (keepRunning) {
        struct sockaddr_storage from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_uu_sock, rxBuf, sizeof(rxBuf), 0,
                             (struct sockaddr*)&from, &fl);
        if (n > 0) {
            g_rxp_uu.fetch_add(1);
            g_rxb_uu.fetch_add((uint64_t)n);
            g_stats_uu.last_rx_us.store(now_us());
            // [추가] RTP 가 실려 있지 않은 패킷(ACK 전용, keepalive 응답 등)도
            // "이 경로로 QUIC 패킷이 오고 있다"는 증거다. 이걸 반영하지 않으면
            // keepalive 주기와 PATH_DEAD_US 가 맞물려 표시가 깜빡인다.
            struct sockaddr_in uu_local; memset(&uu_local, 0, sizeof(uu_local));
            uu_local.sin_family = AF_INET;
            inet_pton(AF_INET, UU_RECEIVER_IP, &uu_local.sin_addr);
            uu_local.sin_port = htons(UU_RECEIVER_PORT);
            std::lock_guard<std::mutex> lk(quic_mutex);
            g_incoming_path.store(0);
            picoquic_incoming_packet(quic, rxBuf, n,
                (struct sockaddr*)&from, (struct sockaddr*)&uu_local,
                0, 0, picoquic_current_time());
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // 에러 무시
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void quicResponseThread(picoquic_quic_t *quic) {
    uint8_t sendBuf[1536];
    while (keepRunning) {
        size_t sendLen = 0;
        struct sockaddr_storage peer, local;
        int if_idx = 0;
        picoquic_connection_id_t log_cid;
        picoquic_cnx_t *last_cnx = nullptr;
        size_t msg_sz = 0;
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            int ret = picoquic_prepare_next_packet_ex(quic, picoquic_current_time(),
                sendBuf, sizeof(sendBuf), &sendLen, &peer, &local,
                &if_idx, &log_cid, &last_cnx, &msg_sz);
            if (ret != 0) continue;
        }
        if (sendLen > 0)
            dispatchQuicPacket(sendBuf, sendLen, &peer, &local);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ── CSV/이벤트 스레드 (2초 주기) — CSV 기록 로직은 원본 그대로 ──
void statusThread() {
    bool prev_uu_alive  = false;
    bool prev_pc5_alive = false;
    bool started        = false;

    uint64_t prev_uu_bytes  = 0;
    uint64_t prev_pc5_bytes = 0;
    const double INTERVAL_SEC = 2.0;

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // 1. SDK Metrics API 호출
        Seamless_Pc5Metrics pc5_metric = {0};
        Seamless_UuMetrics  uu_metric  = {0};
        Seamless_GetPc5Metrics(&pc5_metric);
        Seamless_GetUuMetrics(&uu_metric);
        g_uu_rssi_snapshot.store(uu_metric.rssi);
        g_pc5_rssi_snapshot.store(std::max(pc5_metric.rssiAnt1Max, pc5_metric.rssiAnt2Max));

        uint64_t t = now_us();

        uint64_t uu_last  = g_stats_uu.last_rx_us.load();
        uint64_t pc5_last = g_stats_pc5.last_rx_us.load();
        bool uu_alive  = (uu_last  > 0 && (t - uu_last)  < PATH_DEAD_US);
        bool pc5_alive = (pc5_last > 0 && (t - pc5_last) < PATH_DEAD_US);

        uint64_t cur_uu_bytes  = g_stats_uu.rtp_bytes.load();
        uint64_t cur_pc5_bytes = g_stats_pc5.rtp_bytes.load();

        double uu_kbps  = (cur_uu_bytes  - prev_uu_bytes)  * 8.0 / INTERVAL_SEC / 1000.0;
        double pc5_kbps = (cur_pc5_bytes - prev_pc5_bytes) * 8.0 / INTERVAL_SEC / 1000.0;

        prev_uu_bytes  = cur_uu_bytes;
        prev_pc5_bytes = cur_pc5_bytes;
        g_uu_kbps_snapshot.store(uu_kbps);
        g_pc5_kbps_snapshot.store(pc5_kbps);

        bool freeze_now = false;
        {
            std::lock_guard<std::mutex> lk(g_freeze_mtx);
            if (g_last_rtp_rx_us > 0)
                freeze_now = (t - g_last_rtp_rx_us > FREEZE_THRESHOLD_US);
        }

        if (started) {
            if (!prev_uu_alive && uu_alive) {
                UI_OK("Uu  경로 수신 재개");
                csv_write_event("RECOVER", "Uu", "");
            } else if (prev_uu_alive && !uu_alive) {
                UI_WARN("Uu  수신 끊김 (마지막 %.1f초 전) \u2014 "
                        "미사용인지 장애인지는 수신측에서 구분 불가",
                        (t - uu_last) / 1e6);
                char detail[32]; snprintf(detail, sizeof(detail), "last_rx_%.1fs_ago", (t - uu_last) / 1e6);
                csv_write_event("DOWN", "Uu", detail);
            }
            if (!prev_pc5_alive && pc5_alive) {
                UI_OK("PC5 경로 수신 재개");
                csv_write_event("RECOVER", "PC5", "");
            } else if (prev_pc5_alive && !pc5_alive) {
                UI_WARN("PC5 수신 끊김 (마지막 %.1f초 전)",
                        pc5_last > 0 ? (t - pc5_last) / 1e6 : 0.0);
                char detail[32]; snprintf(detail, sizeof(detail), "last_rx_%.1fs_ago", (t - pc5_last) / 1e6);
                csv_write_event("DOWN", "PC5", detail);
            }
        }
        if (uu_alive || pc5_alive) started = true;

        prev_uu_alive  = uu_alive;
        prev_pc5_alive = pc5_alive;

        {
            std::lock_guard<std::mutex> cl(g_csv_mtx);
            if (g_metrics_csv) {
                fprintf(g_metrics_csv,
                        "%llu,%d,%llu,%.1f,%.0f,%d,%d,%d,%d,"      // Uu 데이터 + 물리망
                        "%d,%llu,%.1f,%.0f,%d,%d,%d,%d,%llu,%d\n", // PC5 데이터 + 물리망 + Freeze
                        (unsigned long long)t,
                        (int)uu_alive, (unsigned long long)g_stats_uu.rtp_count.load(), g_stats_uu.last_lat_ms.load(), uu_alive ? uu_kbps : 0.0,
                        uu_metric.rsrp, uu_metric.rsrq, uu_metric.rssi, uu_metric.sinr, // ← Uu 망 상태
                        (int)pc5_alive, (unsigned long long)g_stats_pc5.rtp_count.load(), g_stats_pc5.last_lat_ms.load(), pc5_alive ? pc5_kbps : 0.0,
                        pc5_metric.cbr, pc5_metric.bler, pc5_metric.rssiAnt1Max, pc5_metric.rssiAnt2Max, // ← PC5 망 상태
                        (unsigned long long)g_freeze_count.load(), (int)freeze_now);
                fflush(g_metrics_csv);
            }
        }
    }
}

// ── 터미널 상태 블록 (1초 주기, 화면 전용) ───────────────────
void consoleThread() {
    UiRate uu_rate, pc5_rate;

    uint64_t blk_prev_uu = 0, blk_prev_pc5 = 0, blk_prev_us = 0;
    uint64_t prev_freeze = 0;

    int      prev_path   = -1;
    uint64_t prev_uu_rtp = 0, prev_pc5_rtp = 0;

    int      tick = 0;
    uint64_t blk_prev_pu = 0, blk_prev_pp = 0;

    // 직전 터미널 출력 시점까지의 PATH_ACK 누적값
    uint64_t prev_ack_uu  = 0;
    uint64_t prev_ack_pc5 = 0;

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!keepRunning) break;

        uint64_t t = now_us();

        // ── 수신 경로 전환 감지 (200ms 해상도) ──
        uint64_t uu_rtp  = g_stats_uu.rtp_count.load();
        uint64_t pc5_rtp = g_stats_pc5.rtp_count.load();
        int now_path = -1;
        if      (pc5_rtp > prev_pc5_rtp) now_path = 1;
        else if (uu_rtp  > prev_uu_rtp)  now_path = 0;
        prev_uu_rtp = uu_rtp; prev_pc5_rtp = pc5_rtp;
        if (now_path >= 0) {
            if (prev_path >= 0 && now_path != prev_path) {
                UI_SW("수신경로 전환  %s \u2192 %s",
                      prev_path == 0 ? "Uu" : "PC5",
                      now_path  == 0 ? "Uu" : "PC5");
            }
            prev_path = now_path;
        }

        if (++tick < 5) continue;       // 1초마다만 블록 출력
        tick = 0;

        // [수정] t 는 루프 맨 위(200ms 틱)에서 잡은 값이라 여기선 최대
        // 800ms 낡았다. 그 사이 도착한 패킷의 last_rx 가 t 보다 미래가 되어
        // age 가 언더플로하고 "rx --전 / ?unknown" 으로 잘못 찍혔다.
        t = now_us();

        // 표시 전용으로 직접 읽는다 (스냅샷 전역은 건드리지 않음 → CSV 불변)
        Seamless_Pc5Metrics pm = {0};
        Seamless_UuMetrics  um = {0};
        Seamless_GetPc5Metrics(&pm);
        Seamless_GetUuMetrics(&um);

        uint64_t uu_last  = g_stats_uu.last_rx_us.load();
        uint64_t pc5_last = g_stats_pc5.last_rx_us.load();

        auto age = [&](uint64_t x) -> uint64_t {
            if (x == 0) return UINT64_MAX;
            return (t > x) ? (t - x) : 0;     // 미래 시각이면 0으로 클램프
        };

        bool first_blk = (blk_prev_us == 0);

        double   el_s = blk_prev_us ? (t - blk_prev_us) / 1e6 : 1.0;
        if (el_s <= 0.0) el_s = 1.0;
        double rps_uu  = (uu_rtp  >= blk_prev_uu)
                       ? (uu_rtp  - blk_prev_uu)  / el_s : 0.0;
        double rps_pc5 = (pc5_rtp >= blk_prev_pc5)
                       ? (pc5_rtp - blk_prev_pc5) / el_s : 0.0;
        if (first_blk) { rps_uu = 0.0; rps_pc5 = 0.0; }
        blk_prev_uu = uu_rtp; 
        blk_prev_pc5 = pc5_rtp; 
        blk_prev_us = t;

        uint64_t rxp_uu  = g_rxp_uu.load();
        uint64_t rxp_pc5 = g_rxp_pc5.load();
        double pps_uu  = (rxp_uu  >= blk_prev_pu)
                       ? (rxp_uu  - blk_prev_pu)  / el_s : 0.0;
        double pps_pc5 = (rxp_pc5 >= blk_prev_pp)
                       ? (rxp_pc5 - blk_prev_pp) / el_s : 0.0;
        if (first_blk) { pps_uu = 0.0; pps_pc5 = 0.0; }
        blk_prev_pu = rxp_uu; blk_prev_pp = rxp_pc5;

        // QUIC 패킷 수신율로 판정한다. RTP 카운트는 스트림 파싱 시점의
        // 경로 추정에 의존해 오태깅되지만, QUIC 패킷은 recv 스레드가
        // 직접 세므로 정확하다.
        //   keepalive 만: ~0.5 pkt/s,  영상 흐름: ~100 pkt/s
        const double ACTIVE_PPS = 10.0;
        auto classify = [&](uint64_t last, double pps) -> UiPathState {
            if (last == 0)               return PS_NONE;
            if (age(last) >= 3000000ULL) return PS_UNKNOWN;
            return (pps >= ACTIVE_PPS) ? PS_ACTIVE : PS_STANDBY;
        };
        UiPathState st_uu  = classify(uu_last,  pps_uu);
        UiPathState st_pc5 = classify(pc5_last, pps_pc5);

        double uu_mbps  = uu_rate.mbps(g_rxb_uu.load(),  t);
        double pc5_mbps = pc5_rate.mbps(g_rxb_pc5.load(), t);

        const char* cur = (st_pc5 == PS_ACTIVE) ? "PC5"
                        : (st_uu  == PS_ACTIVE) ? "Uu " : "-- ";

        bool freeze_now = false;
        {
            std::lock_guard<std::mutex> lk(g_freeze_mtx);
            if (g_last_rtp_rx_us > 0)
                freeze_now = (t - g_last_rtp_rx_us > FREEZE_THRESHOLD_US);
        }

        // 1행 — 헤더
        printf("\n%s %s\u25C0 RX %s%s \n", ui_stamp(), cBLD(), cur, cRST());

        // 2행 — Uu
        printf("|  Uu  |  %s  |  %5.2fMbps  |  %4.0fpkt/s  |  누적 %5llu pkt  "
               "|  %4.0fRTP/s  |  누적 %5llu RTP  |  rx %s전%s\n",
               ui_state(st_uu),
               uu_mbps,
               pps_uu,                              // pkt/s
               (unsigned long long)rxp_uu,          // 누적 pkt
               rps_uu,                              // RTP/s               
               (unsigned long long)uu_rtp,          // 누적 RTP
               ui_age(uu_last, t),
               (st_uu == PS_UNKNOWN) ? "  \u26A0" : "");

        // 3행 — PC5
        printf("|  PC5 |  %s  |  %5.2fMbps  |  %4.0fpkt/s  |  누적 %5llu pkt  "
               "|  %4.0fRTP/s  |  누적 %5llu RTP  |  rx %s전\n",
               ui_state(st_pc5),
               pc5_mbps,
               pps_pc5,                             // pkt/s
               (unsigned long long)rxp_pc5,         // 누적 pkt
               rps_pc5,                             // RTP/s
               (unsigned long long)pc5_rtp,         // 누적 RTP
               ui_age(pc5_last, t));

        // 4행 — 지연 분포
        double p50, p95;
        char ubuf[48], pbuf[48];
        if (g_delay_uu.stats(p50, p95))
            snprintf(ubuf, sizeof(ubuf), "p50 %6.1f / p95 %6.1fms", p50, p95);
        else
            snprintf(ubuf, sizeof(ubuf), "--");
        if (g_delay_pc5.stats(p50, p95))
            snprintf(pbuf, sizeof(pbuf), "p50 %6.1f / p95 %6.1fms", p50, p95);
        else
            snprintf(pbuf, sizeof(pbuf), "--");
        printf("  지연  Uu %-26s  PC5 %-26s  frame %llu\n",
               ubuf, pbuf, (unsigned long long)g_frame_meta_cnt.load());

        // ── 최근 1초 구간의 PATH_ACK 송신 분포 ─────────────────────
        uint64_t cur_ack_uu  = 0;
        uint64_t cur_ack_pc5 = 0;

        getPathAckCounts(cur_ack_uu, cur_ack_pc5);

        // 직전 상태 출력 이후 새로 발생한 ACK 개수
        // path 재생성 등으로 카운터가 감소한 경우에도 underflow 방지
        uint64_t ack_uu =
            (cur_ack_uu >= prev_ack_uu)
            ? (cur_ack_uu - prev_ack_uu)
            : cur_ack_uu;

        uint64_t ack_pc5 =
            (cur_ack_pc5 >= prev_ack_pc5)
            ? (cur_ack_pc5 - prev_ack_pc5)
            : cur_ack_pc5;

        // 다음 출력 비교를 위해 현재 누적값 저장
        prev_ack_uu  = cur_ack_uu;
        prev_ack_pc5 = cur_ack_pc5;

        uint64_t ack_tot = ack_uu + ack_pc5;

        printf("  %sPATH_ACK(1s) Uu %llu(%.0f%%)   PC5 %llu(%.0f%%)   total %llu%s\n",
            cDIM(),
            (unsigned long long)ack_uu,
            ack_tot ? 100.0 * ack_uu / ack_tot : 0.0,
            (unsigned long long)ack_pc5,
            ack_tot ? 100.0 * ack_pc5 / ack_tot : 0.0,
            (unsigned long long)ack_tot,
            cRST());
        fflush(stdout);

        // freeze 신규 발생 시 이벤트 한 줄
        uint64_t fz = g_freeze_count.load();
        if (fz > prev_freeze) {
            UI_WARN("freeze 발생 (누적 %llu회)", (unsigned long long)fz);
            prev_freeze = fz;
        }
    }
}

static bool copy_arg(char* dst, size_t dst_len, const char* value)
{
    if (dst == nullptr || dst_len == 0 || value == nullptr || strlen(value) >= dst_len) {
        return false;
    }
    snprintf(dst, dst_len, "%s", value);
    return true;
}

static void print_usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --uu-bind IP          local Uu/server address\n"
        "  --uu-port PORT        local Uu UDP port\n"
        "  --pc5-local IP        local PC5/RSU address\n"
        "  --pc5-peer IP         remote PC5/OBU address\n"
        "  --exp NAME            experiment label\n"
        "  --no-color            disable ANSI colors\n",
        prog);
}

// ===================== MAIN =====================
int main(int argc, char **argv) {
    g_color = (isatty(1) != 0);

    std::string exp_name = "default";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uu-bind") == 0 && i + 1 < argc) {
            if (!copy_arg(g_uu_receiver_ip, sizeof(g_uu_receiver_ip), argv[++i])) return -1;
        }
        else if (strcmp(argv[i], "--uu-port") == 0 && i + 1 < argc) {
            g_uu_receiver_port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--pc5-local") == 0 && i + 1 < argc) {
            if (!copy_arg(g_pc5_receiver_ip, sizeof(g_pc5_receiver_ip), argv[++i])) return -1;
        }
        else if (strcmp(argv[i], "--pc5-peer") == 0 && i + 1 < argc) {
            if (!copy_arg(g_pc5_sender_ip, sizeof(g_pc5_sender_ip), argv[++i])) return -1;
        }
        else if (strcmp(argv[i], "--exp") == 0 && i + 1 < argc) {
            exp_name = argv[++i];
        }
        else if (strcmp(argv[i], "--no-color") == 0) {
            g_color = false;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    g_exp_name_disp = exp_name;
    initAddresses();

    // ── CSV 파일 열기 ────────────────────────────────────────
    std::string base = "./logs/";
    if (system(("mkdir -p " + base).c_str()) != 0) {
        printf("경고: logs 디렉토리 생성 실패\n");
    }

    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm_info);

    std::string prefix = base + exp_name + "_" + ts;

    g_metrics_csv = fopen((prefix + "_metrics.csv").c_str(), "w");
    if (g_metrics_csv)
        fprintf(g_metrics_csv,
                "timestamp_us,"
                "uu_alive,uu_rtp,uu_latency_ms,uu_kbps,"
                "uu_rsrp,uu_rsrq,uu_rssi,uu_sinr,"           // ← Uu 물리망 지표
                "pc5_alive,pc5_rtp,pc5_latency_ms,pc5_kbps,"
                "pc5_cbr,pc5_bler,pc5_rssiAnt1,pc5_rssiAnt2,"// ← PC5 물리망 지표
                "freeze_count,freeze_now\n");

    g_events_csv = fopen((prefix + "_events.csv").c_str(), "w");
    if (g_events_csv) {
        fprintf(g_events_csv, "timestamp_us,event,path,detail\n");
        fflush(g_events_csv);
    }

    g_freeze_csv = fopen((prefix + "_freeze.csv").c_str(), "w");
    if (g_freeze_csv) {
        fprintf(g_freeze_csv, "freeze_start_us,duration_ms,path\n");
        fflush(g_freeze_csv);                 // 헤더가 날아가지 않도록
    }

    g_packet_csv = fopen((prefix + "_packet.csv").c_str(), "w");
    if (g_packet_csv)
        fprintf(g_packet_csv,
                "packet_seq,frame_id,tx_time_us,rx_time_us,delay_ms,rtp_bytes,select_net,uu_rssi,uu_throughput_kbps,pc5_rssi,pc5_throughput_kbps,speed_mps,latitude,longitude,heading_deg,gps_fixed\n");
    
    ui_start();
    ui_rule();
    printf("  %sV2X MP-QUIC Receiver%s                      %sexp=%s  role=RSU%s\n",
           cBLD(), cRST(), cDIM(), exp_name.c_str(), cRST());
    printf("  %sUu  %s:%d       PC5  %s \u2190 %s%s\n",
           cDIM(), UU_RECEIVER_IP, UU_RECEIVER_PORT,
           PC5_RECEIVER_IP, PC5_SENDER_IP, cRST());
    printf("  %slog  %s_{metrics,events,freeze,packet}.csv%s\n",
           cDIM(), prefix.c_str(), cRST());
    printf("  %s종료: q + Enter%s\n", cDIM(), cRST());
    ui_rule();

    if (!initSirius(PC5_RECEIVER_IP)) {
        UI_BAD("V2X ERROR");
        return -1;
    }

    g_uu_sock = initUuSocket();
    if (g_uu_sock < 0) return -1;

    picoquic_quic_t *quic = initQuic();
    if (!quic) return -1;

    UI_OK("수신 대기 시작");

    std::thread input_thr(inputThread);
    std::thread pc5_thr(pc5RecvThread, quic);
    std::thread uu_thr(uuRecvThread, quic);
    std::thread resp_thr(quicResponseThread, quic);
    std::thread status_thr(statusThread);
    std::thread console_thr(consoleThread);

    while (keepRunning)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    keepRunning = false;

    pc5_thr.join(); uu_thr.join();
    input_thr.join(); resp_thr.join(); status_thr.join();
    console_thr.join();

    picoquic_free(quic);
    if (g_uu_sock >= 0) close(g_uu_sock);

    if (g_metrics_csv) fclose(g_metrics_csv);
    if (g_events_csv)  fclose(g_events_csv);
    if (g_freeze_csv)  fclose(g_freeze_csv);
    if (g_packet_csv)  fclose(g_packet_csv);

    Seamless_FinalizePc5Raw();

    // ── 종료 요약 ──
    uint64_t uu_rtp  = g_stats_uu.rtp_count.load();
    uint64_t pc5_rtp = g_stats_pc5.rtp_count.load();
    double p50, p95;
    ui_rule();
    printf("  %s실험 %s 종료  (%.1f초)%s\n",
           cBLD(), exp_name.c_str(), ui_elapsed_s(), cRST());
    printf("  %s수신  RTP %llu  (Uu %llu / PC5 %llu)   누락 %llu  역전 %llu  "
           "freeze %llu회%s\n",
           cDIM(),
           (unsigned long long)(uu_rtp + pc5_rtp),
           (unsigned long long)uu_rtp, (unsigned long long)pc5_rtp,
           (unsigned long long)g_seq_lost.load(),
           (unsigned long long)g_seq_reorder.load(),
           (unsigned long long)g_freeze_count.load(), cRST());
    if (g_delay_uu.stats(p50, p95))
        printf("  %s지연  Uu  p50 %.1f / p95 %.1f ms%s\n", cDIM(), p50, p95, cRST());
    if (g_delay_pc5.stats(p50, p95))
        printf("  %s지연  PC5 p50 %.1f / p95 %.1f ms%s\n", cDIM(), p50, p95, cRST());
    {
        uint64_t a = g_txp_uu.load();
        uint64_t b = g_txp_pc5.load();
        uint64_t t2 = a + b;

        printf("  %sQUIC송신 Uu %llu(%.1f%%)   PC5 %llu(%.1f%%)%s\n",
            cDIM(),
            (unsigned long long)a,
            t2 ? 100.0 * a / t2 : 0.0,
            (unsigned long long)b,
            t2 ? 100.0 * b / t2 : 0.0,
            cRST());

        uint64_t ack_uu = 0;
        uint64_t ack_pc5 = 0;

        getPathAckCounts(ack_uu, ack_pc5);

        uint64_t ack_tot = ack_uu + ack_pc5;

        printf("  %sPATH_ACK 누적 Uu %llu(%.1f%%)   PC5 %llu(%.1f%%)   total %llu%s\n",
            cDIM(),
            (unsigned long long)ack_uu,
            ack_tot ? 100.0 * ack_uu / ack_tot : 0.0,
            (unsigned long long)ack_pc5,
            ack_tot ? 100.0 * ack_pc5 / ack_tot : 0.0,
            (unsigned long long)ack_tot,
            cRST());
    }
    ui_rule();
    return 0;
}