/*
 * TwinSteer MP-QUIC reference sender.
 *
 * The public reference implementation intentionally uses a single
 * round-robin packet scheduler over the available Uu and PC5 paths.
 * TwinSteer learning/DQN components are maintained separately.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <algorithm>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <optional>
#include <mutex>
#include <vector>

#include <opencv2/opencv.hpp>

extern "C"{
    #include "scheduler.h"

    #include "etf-types.h"
    #include "gps.h"

    // ============ [ V2X Message Headers ] ============
    // Enum Values
    #include "etf_ksr1600_j2735_enum.h"
    #include "etf_sdk_wsmp_psid_kr.h"
    #include "etf_j2540.h" // For J2540_ITIScodes, J2540_ITIStext
    // Data Elements
    #include "etf_ksr1600_j2735_data_element.h"
    #include "etf_ksr1600_kor_data_element.h"
    // Data Frames
    #include "etf_ksr1600_j2735_data_frame.h"
    #include "etf_ksr1600_kor_data_frame.h"
    // V2X Messages
    #include "etf_ksr1600_j2735_message.h" // For BSM, PVD, MAP, SPaT, RSA, TIM Message
    #include "etf_j3224.h" // For SDSM Message

    // ============== [ SDK API Headers ] ==============
    // V2X Message API
    #include "etf_sdk_ksr1600_api.h"
    // Seamless SDK API
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
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

// ═════════════════════════════════════════════════════════════
//  터미널 출력 유틸 (화면 전용 — CSV 와 무관)
//
//    ●ACTIVE   지금 이 경로로 데이터가 흐름
//    ○standby  경로 살아있고 미사용 (응답 확인됨)
//    ?unknown  경로는 있으나 생존 미확인 — 유휴가 길어 확인 수단 없음
//    ✗DEAD     경로 없음 / demoted
// ═════════════════════════════════════════════════════════════
static bool g_color = true;
static inline const char* CC(const char* s) { return g_color ? s : ""; }
static inline const char* cRST() { return CC("\033[0m");  }
static inline const char* cBLD() { return CC("\033[1m");  }
static inline const char* cDIM() { return CC("\033[2m");  }
static inline const char* cRED() { return CC("\033[31m"); }
static inline const char* cGRN() { return CC("\033[32m"); }
static inline const char* cYEL() { return CC("\033[33m"); }
static inline const char* cMAG() { return CC("\033[35m"); }
static inline const char* cCYN() { return CC("\033[36m"); }
static inline const char* cGRY() { return CC("\033[90m"); }

static uint64_t g_ui_t0 = 0;

static inline uint64_t ui_wall_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}
static inline void   ui_start()     { g_ui_t0 = ui_wall_us(); }
static inline double ui_elapsed_s() {
    return g_ui_t0 ? (ui_wall_us() - g_ui_t0) / 1e6 : 0.0;
}

static bool g_verbose = false;

// 같은 printf 안에서 여러 번 써도 안 깨지도록 회전 버퍼 사용
static thread_local char g_ui_pool[8][40];
static thread_local int  g_ui_pi = 0;
static inline char* ui_buf() { g_ui_pi = (g_ui_pi + 1) & 7; return g_ui_pool[g_ui_pi]; }

// "[+0045.0s|16:03:33]"
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

// 표시 폭 8칸 고정
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

static inline const char* ui_bytes(uint64_t v) {
    char* b = ui_buf();
    if (v >= (1ULL << 30)) snprintf(b, 40, "%.1fGB", v / 1073741824.0);
    else                   snprintf(b, 40, "%.1fMB", v / 1048576.0);
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
#define UI_OK(...)    ui_ev(cGRN(), "\u2713", __VA_ARGS__)   // ✓
#define UI_BAD(...)   ui_ev(cRED(), "\u2717", __VA_ARGS__)   // ✗
#define UI_WARN(...)  ui_ev(cYEL(), "\u26A0", __VA_ARGS__)   // ⚠
#define UI_SW(...)    ui_ev(cCYN(), "\u21C4", __VA_ARGS__)   // ⇄
#define UI_PROBE(...) ui_ev(cMAG(), "\u21BB", __VA_ARGS__)   // ↻

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

// ===================== 전역 변수 =====================
std::atomic<bool> keepRunning(true);

// ── GPS 스냅샷 ──
struct GpsSnapshot {
    double lat        = 0.0;
    double lon        = 0.0;
    float  speed_mps  = 0.0f;
    double heading_deg = 0.0;
    bool   fixed      = false;
};
static GpsSnapshot g_gps_snap;
static std::mutex  g_gps_mtx;

// ── FrameMeta ──
#pragma pack(push, 1)
struct FrameMetaPacket {
    uint64_t frame_id;
    uint64_t t_tx_us;
    uint8_t  scheduler_id;
    float    speed_mps;
    double   lat;
    double   lon;
    double   heading_deg;
    uint8_t  gps_fixed;
};
#pragma pack(pop)

std::mutex quic_mutex;
static picoquic_cnx_t* g_cnx = nullptr;

static const uint8_t RR_SCHEDULER_ID = 0;
static const char* g_exp_name = "default";

static GstAppSrc*  g_appsrc  = nullptr;
static GstAppSink* g_appsink = nullptr;

#define PKT_TYPE_RTP        0x01
#define PKT_TYPE_FRAME_META 0x04


#pragma pack(push, 1)
struct RtpPacketEnvelope {
    uint64_t packet_seq;
    uint64_t frame_id;
    uint64_t t_tx_us;
    uint32_t rtp_len;
    uint8_t  tx_path;
};
#pragma pack(pop)

static std::atomic<uint64_t> g_rtp_packet_seq{0};
static std::atomic<uint64_t> g_current_frame_id{0};

static std::atomic<int>      g_uu_rssi_snapshot{0};
static std::atomic<int>      g_pc5_rssi_snapshot{0};
static char g_pc5_sender_ip[64]   = "10.254.52.19";
static char g_pc5_receiver_ip[64] = "10.254.52.18";
static char g_uu_receiver_ip[64]  = "100.73.62.97";
static int  g_uu_receiver_port    = 36230;
static char g_uu_sender_ip[64]    = "0.0.0.0";
static int  g_uu_sender_port      = 0;
static char g_gps_host[64]        = "192.168.70.102";
static char g_gps_port[16]        = "4545";
static int  g_camera_index        = 0;

#define PC5_SENDER_IP    g_pc5_sender_ip
#define PC5_RECEIVER_IP  g_pc5_receiver_ip
#define UU_RECEIVER_IP   g_uu_receiver_ip
#define UU_RECEIVER_PORT g_uu_receiver_port
#define UU_SENDER_IP     g_uu_sender_ip
#define UU_SENDER_PORT   g_uu_sender_port

struct sockaddr_storage pc5_remote_addr;
struct sockaddr_storage pc5_local_addr;
struct sockaddr_storage uu_remote_addr;
struct sockaddr_storage uu_local_addr;
static int g_uu_sock = -1;

#define CAMERA_WIDTH  1280   // 640 → 1280
#define CAMERA_HEIGHT 720    // 480 → 720
#define CAMERA_FPS    30

// draft-21 §2.1 initial_max_path_id. 수신측도 같은 값으로 올려야 한다.
// path ID 는 재사용이 금지되므로(§3.4) 재접속 횟수만큼 소모된다.
#define MAX_PATH_ID   64

// 이 값을 넘는 PC5 RSSI 는 "RSU 방송이 닿았다 = 커버리지 진입"으로
// 보고 재probe 를 즉시 시도하는 트리거로 쓴다.
// 차단 조건이 아니라 촉진 조건이다 (신호가 없어도 30초마다는 시도).
// 실제 값은 현장 측정 후 조정할 것 — 커버리지 밖 노이즈 플로어 기준.
#define PC5_PROBE_RSSI_FLOOR (-90)

static std::atomic<bool> g_uu_handshake_done(false);
static std::atomic<bool> g_pc5_path_added(false);
static std::atomic<bool> g_pc5_need_probe(false);

// QUIC path liveness and RTT are taken from picoquic's own path state.
// The reference sender does not inject custom RTT samples.

static inline uint64_t now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static std::atomic<uint64_t> g_tx_pkt_uu{0};
static std::atomic<uint64_t> g_tx_pkt_pc5{0};

// ── [터미널] 화면 출력용 카운터 (CSV 와 무관) ────────────────
static std::atomic<uint64_t> g_tx_byte_uu{0};
static std::atomic<uint64_t> g_tx_byte_pc5{0};
static std::atomic<uint64_t> g_last_tx_uu_us{0};
static std::atomic<uint64_t> g_last_tx_pc5_us{0};
static std::atomic<uint64_t> g_last_rx_uu_us{0};    // Uu 로 뭐든 들어온 시각

static std::atomic<int> g_uu_rsrp{0}, g_uu_rsrq{0}, g_uu_sinr{0};
static std::atomic<int> g_pc5_ant1{-140}, g_pc5_ant2{-140};

// ── 경로 상태 캐시 (picoquic 이 유일한 진실, 200ms 주기 갱신) ──
//   앱이 자체 플래그로 생존을 판단하면 picoquic 실제 상태와 어긋나
//   "죽었다고 믿으며 계속 쓰는" 상태가 발생한다. 그래서 여기엔
//   scheduler_path_status() 결과만 담는다.
static std::mutex             g_pinfo_mtx;
static scheduler_path_info_t  g_pinfo_uu  = {};
static scheduler_path_info_t  g_pinfo_pc5 = {};
static std::atomic<uint64_t>  g_pinfo_at_us{0};

static std::atomic<uint64_t> g_enc_bytes{0};
static std::atomic<double>   g_cam_fps{0.0};
static std::atomic<uint64_t> g_cam_drop{0};

static std::atomic<int> g_probe_ok{0};
static std::atomic<int> g_probe_fail{0};

// ===================== 함수 =====================

void sendTypedData(uint8_t type, const uint8_t* data, uint32_t len) {
    uint32_t net_len = htonl(len);
    std::lock_guard<std::mutex> lock(quic_mutex);
    if (!g_cnx) return;

    /* A single QUIC stream is used. Packet-level path selection is handled
     * by the round-robin scheduler inside picoquic. */
    picoquic_add_to_stream(g_cnx, 4, &type, 1, 0);
    picoquic_add_to_stream(g_cnx, 4, (const uint8_t*)&net_len, 4, 0);
    picoquic_add_to_stream(g_cnx, 4, data, len, 0);
}

void sendFrameMeta(uint64_t frameId) {
    FrameMetaPacket meta;
    memset(&meta, 0, sizeof(meta));
    meta.frame_id     = frameId;
    meta.t_tx_us      = now_us();
    meta.scheduler_id = RR_SCHEDULER_ID;

    {
        std::lock_guard<std::mutex> lk(g_gps_mtx);
        meta.speed_mps  = g_gps_snap.speed_mps;
        meta.lat        = g_gps_snap.lat;
        meta.lon        = g_gps_snap.lon;
        meta.heading_deg = g_gps_snap.heading_deg;
        meta.gps_fixed   = g_gps_snap.fixed ? 1 : 0;
    }

    sendTypedData(PKT_TYPE_FRAME_META,
                  (const uint8_t*)&meta, sizeof(meta));
}

GstFlowReturn onNewSample(GstAppSink* appsink, gpointer) {
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        RtpPacketEnvelope env;
        env.packet_seq = g_rtp_packet_seq.fetch_add(1);
        env.frame_id   = g_current_frame_id.load();
        env.t_tx_us    = now_us();
        env.rtp_len    = (uint32_t)map.size;
        int sel = scheduler_get_last_sel_rat();
        env.tx_path = (sel < 0) ? 0xFF : (uint8_t)sel;

        g_enc_bytes.fetch_add(map.size);      // [터미널] 인코더 비트레이트용

        std::vector<uint8_t> packet(sizeof(env) + map.size);
        memcpy(packet.data(), &env, sizeof(env));
        memcpy(packet.data() + sizeof(env), map.data, map.size);

        sendTypedData(PKT_TYPE_RTP,
                      packet.data(), (uint32_t)packet.size());

        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void dispatchQuicPacket(uint8_t* data, size_t len,
                        struct sockaddr_storage* peer_addr,
                        struct sockaddr_storage* local_addr) {
    (void)local_addr;

    struct sockaddr_in* peer_sin = (struct sockaddr_in*)peer_addr;
    struct sockaddr_in* pc5_sin  = (struct sockaddr_in*)&pc5_remote_addr;
    bool is_pc5 = (peer_sin->sin_addr.s_addr == pc5_sin->sin_addr.s_addr &&
                   peer_sin->sin_port        == pc5_sin->sin_port);

    if (is_pc5) {
        g_tx_pkt_pc5++;
        g_tx_byte_pc5.fetch_add(len);
        g_last_tx_pc5_us.store(now_us());
        (void)Seamless_SendPc5Raw(data, (uint16_t)len);
    } else if (g_uu_sock >= 0) {
        g_tx_pkt_uu++;
        g_tx_byte_uu.fetch_add(len);
        g_last_tx_uu_us.store(now_us());
        (void)sendto(g_uu_sock, data, len, 0,
                     (struct sockaddr*)peer_addr, sizeof(struct sockaddr_in));
    }
}

// ===================== 초기화 =====================
bool initSirius(const char* pc5_ip) {
    int ret = Seamless_InitializePc5Raw(NULL);
    if (ret <= 0) {
        UI_BAD("Seamless_InitializePc5Raw 실패  ret=%d errno=%d(%s)",
               ret, errno, strerror(errno));
        return false;
    }
    UI_OK("SIRIUS SDK 연결 (%s)", pc5_ip);
    return true;
}

void initGStreamerApp(GstElement* pipeline){
    g_appsrc  = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "src"));
    g_appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "sink"));
    GstAppSinkCallbacks cbs = {}; cbs.new_sample = onNewSample;
    gst_app_sink_set_callbacks(g_appsink, &cbs, nullptr, nullptr);
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "BGR",
        "width",  G_TYPE_INT,    CAMERA_WIDTH,
        "height", G_TYPE_INT,    CAMERA_HEIGHT,
        "framerate", GST_TYPE_FRACTION, CAMERA_FPS, 1, nullptr);
    gst_app_src_set_caps(g_appsrc, caps);
    gst_caps_unref(caps);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

GstElement* initGStreamerPipeline(){
    const char* pipe =
        "appsrc name=src is-live=true format=time ! "
        "videoconvert ! "
        "x264enc tune=zerolatency bitrate=1000 speed-preset=ultrafast "
        "key-int-max=30 rc-lookahead=0 bframes=0 pass=cbr ! "
        "video/x-h264,profile=baseline ! "
        "rtph264pay config-interval=-1 pt=96 mtu=1400 ! "
        "appsink name=sink sync=false drop=false";
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(pipe, &err);
    if (!pipeline || err) {
        UI_BAD("GStreamer 파이프라인 실패: %s", err ? err->message : "?");
        return nullptr;
    }
    return pipeline;
}

int initUuSocket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    //setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, UU_INTERFACE, strlen(UU_INTERFACE));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, UU_SENDER_IP, &addr.sin_addr);
    addr.sin_port = htons(UU_SENDER_PORT);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }
    socklen_t sl = sizeof(addr);
    getsockname(sock, (struct sockaddr*)&addr, &sl);
    UI_OK("Uu 소켓 바인드 %s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    memset(&uu_local_addr, 0, sizeof(uu_local_addr));
    memcpy(&uu_local_addr, &addr, sizeof(addr));
    return sock;
}

void initAddresses() {
    memset(&pc5_remote_addr, 0, sizeof(pc5_remote_addr));
    memset(&pc5_local_addr,  0, sizeof(pc5_local_addr));
    struct sockaddr_in* r = (struct sockaddr_in*)&pc5_remote_addr;
    r->sin_family = AF_INET;
    inet_pton(AF_INET, PC5_RECEIVER_IP, &r->sin_addr);
    r->sin_port = htons(4433);
    struct sockaddr_in* l = (struct sockaddr_in*)&pc5_local_addr;
    l->sin_family = AF_INET;
    inet_pton(AF_INET, PC5_SENDER_IP, &l->sin_addr);
    l->sin_port = htons(12345);
    memset(&uu_remote_addr, 0, sizeof(uu_remote_addr));
    struct sockaddr_in* ur = (struct sockaddr_in*)&uu_remote_addr;
    ur->sin_family = AF_INET;
    inet_pton(AF_INET, UU_RECEIVER_IP, &ur->sin_addr);
    ur->sin_port = htons(UU_RECEIVER_PORT);
}

picoquic_quic_t* initQuic(){
    uint64_t t = picoquic_current_time();
    picoquic_quic_t *quic = picoquic_create(
        8, "../certs/cert.pem", "../certs/key.pem", NULL, "v2x_test",
        NULL, NULL, NULL, NULL, NULL, t, NULL, NULL, NULL, 0);
    if (!quic) return nullptr;
    quic->default_multipath_option = 1;
    // draft-21 §2.1 — path ID 는 재사용 금지(§3.4)이므로 재접속 횟수만큼
    // 소모된다. 4 는 커버리지 경계에서 몇 초 만에 고갈된다.
    // 최댓값은 2^32-1 이지만 §7.1(경로별 메모리) 고려해 64 로.
    quic->default_tp.initial_max_path_id = MAX_PATH_ID;
    picoquic_set_default_congestion_algorithm_by_name(quic, "bbr");
    picoquic_set_cwin_max(quic, 10000000);
    picoquic_set_default_idle_timeout(quic, 120000);

    // ── qlog: 경로별 RTT 샘플이 실제로 들어오는지 확인용 ──────
    //   PC5 smoothed_rtt 가 갱신되지 않는 원인을 가리려면
    //   metrics_updated 이벤트의 path_id / smoothed_rtt 를 봐야 한다.
    //   함수명이 picoquic 버전마다 다르니 먼저 확인할 것:
    //     grep -n "set_qlog\|set_binlog\|set_log_level" picoquic.h
    //   확인 후 아래 두 줄의 주석을 풀고 --qlog 플래그로 켠다.
    // if (g_qlog_on) {
    //     if (system("mkdir -p ./logs/qlog") != 0) {}
    //     picoquic_set_qlog(quic, "./logs/qlog");
    //     picoquic_set_log_level(quic, 1);
    //     UI_OK("qlog 활성화 → ./logs/qlog");
    // }

    picoquic_cnx_t *cnx = picoquic_create_cnx(quic,
        picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr*)&uu_remote_addr, t, 0, "test.sni", "v2x_test", 1);
    if (!cnx) return nullptr;
    cnx->local_parameters.initial_max_path_id = MAX_PATH_ID;
    cnx->idle_timeout = 120000;

    // PC5 상대 주소를 스케줄러에 주입 (하드코딩 중복 제거)
    scheduler_set_pc5_peer_ip(PC5_RECEIVER_IP);

    picoquic_set_callback(cnx, [](picoquic_cnx_t* cnx,
        uint64_t stream_id, uint8_t* bytes, size_t length,
        picoquic_call_back_event_t event, void* ctx, void* sctx) -> int {
        switch (event) {
        case picoquic_callback_ready:
            UI_OK("QUIC 핸드셰이크 완료 (multipath=%d)", cnx->is_multipath_enabled);
            g_uu_handshake_done = true; break;
        case picoquic_callback_path_available:
            UI_OK("PC5 경로 연결  (nb_paths=%d)", cnx->nb_paths);
            g_pc5_path_added = true; g_pc5_need_probe = false; break;

        // [수정] suspended 는 "일시 중단"이라 되살아날 수 있다.
        //        원본은 deleted 와 같이 묶어 g_pc5_need_probe 를 래치시켜
        //        경로가 살아난 뒤에도 영구히 죽은 것으로 취급했다.
        //        이제 생존 판단은 scheduler_path_status() 가 하므로
        //        여기서는 이벤트만 알리고 상태를 바꾸지 않는다.
        case picoquic_callback_path_suspended:
            UI_WARN("PC5 경로 suspended (일시 중단, 복구 가능)");
            break;

        case picoquic_callback_path_deleted:
            UI_BAD("PC5 경로 삭제 (path ID 소모됨)");
            g_pc5_path_added = false;
            g_pc5_need_probe = true;
            break;
        case picoquic_callback_close:
        case picoquic_callback_application_close:
            // 종료 주체와 사유를 남긴다.
            //   remote_* 가 0 이 아니면 상대(수신측)가 닫은 것
            //   local_error 만 있으면 picoquic 이 스스로 판단한 것
            UI_BAD("QUIC 연결 종료 \u2014 local=0x%llx remote=0x%llx "
                   "app=0x%llx remote_app=0x%llx  state=%d nb_paths=%d",
                   (unsigned long long)cnx->local_error,
                   (unsigned long long)cnx->remote_error,
                   (unsigned long long)cnx->application_error,
                   (unsigned long long)cnx->remote_application_error,
                   (int)cnx->cnx_state, cnx->nb_paths);
            if (cnx->local_error_reason)
                UI_BAD("  local 사유: %s", cnx->local_error_reason);
            if (cnx->remote_error_reason)
                UI_BAD("  remote 사유: %s", cnx->remote_error_reason);
            keepRunning = false; break;
        default: break;
        }
        return 0;
    }, NULL);

    picoquic_start_client_cnx(cnx);
    g_cnx = cnx;
    return quic;
}

// Poll picoquic path state for diagnostics and reprobe decisions.

void pathStatusThread(picoquic_quic_t* quic) {
    (void)quic;
    while (keepRunning && !g_uu_handshake_done)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        scheduler_path_info_t uu = {}, pc5 = {};
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            if (g_cnx) scheduler_path_status(g_cnx, &uu, &pc5);
        }
        {
            std::lock_guard<std::mutex> lk(g_pinfo_mtx);
            g_pinfo_uu = uu;
            g_pinfo_pc5 = pc5;
        }
        g_pinfo_at_us.store(now_us());
    }
}


void gpsThread() {
    gps_data_t gpsdata;
    memset(&gpsdata, 0, sizeof(gpsdata));
    bool warned_open = false;

    while (keepRunning) {
        int gret = gps_open(g_gps_host, g_gps_port, &gpsdata);
        if (gret != 0) {
            if (!warned_open) {
                UI_WARN("GPS gps_open 실패: %s (code=%d) \u2014 1초마다 재시도",
                        gps_errstr(gret), gret);
                warned_open = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (warned_open) UI_OK("GPS gpsd 연결 복구");
        warned_open = false;
        gps_stream(&gpsdata, WATCH_ENABLE | WATCH_JSON, nullptr);

        while (keepRunning) {
            if (!gps_waiting(&gpsdata, 1000000)) {
                continue;
            }

            if (gps_read(&gpsdata, nullptr, 0) == -1) {
                break;
            }

            GpsSnapshot snap;
            snap.fixed = (gpsdata.fix.mode >= MODE_2D);
            if (snap.fixed) {
                snap.lat = gpsdata.fix.latitude;
                snap.lon = gpsdata.fix.longitude;
                if (gpsdata.fix.speed >= 0.0)
                    snap.speed_mps = (float)gpsdata.fix.speed;
                if (gpsdata.fix.track >= 0.0)
                    snap.heading_deg = gpsdata.fix.track;
            }

            {
                std::lock_guard<std::mutex> lk(g_gps_mtx);
                bool was = g_gps_snap.fixed;
                g_gps_snap = snap;
                if (was != snap.fixed) {
                    if (snap.fixed) UI_OK("GPS fix 획득 (mode=%d)", gpsdata.fix.mode);
                    else            UI_WARN("GPS fix 상실");
                }
            }
        }

        gps_stream(&gpsdata, WATCH_DISABLE, nullptr);
        gps_close(&gpsdata);
        memset(&gpsdata, 0, sizeof(gpsdata));

        if (keepRunning)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void rssiUpdateThread() {
    // PC5 RSSI 는 "수신한 신호"의 세기라, 상대(RSU)가 그 순간 PC5 로
    // 아무것도 쏘지 않으면 측정 자체가 안 되어 -95(플로어)가 나온다.
    // 실제로 PC5 링크가 멀쩡한데도 한두 샘플이 -95 로 튀는 일이 있고,
    // Keep the raw RSSI snapshot available for diagnostics and reprobe triggering.
    // → 최근에 유효값을 본 적이 있으면 짧게 직전값을 유지한다.
    //   HOLD 시간이 지나도 계속 -95 면 그때는 진짜 단절로 본다.
    static const int      PC5_RSSI_VALID_ABOVE = -90;
    static const uint64_t PC5_RSSI_HOLD_US     = 3000000ULL;  // 3초
    int      last_valid_pc5 = -140;
    uint64_t last_valid_us  = 0;

    while (keepRunning) {
        Seamless_Pc5Metrics pc5_metric = {0};
        Seamless_UuMetrics  uu_metric  = {0};
        Seamless_GetPc5Metrics(&pc5_metric);
        Seamless_GetUuMetrics(&uu_metric);

        int pc5_raw = std::max(pc5_metric.rssiAnt1Max, pc5_metric.rssiAnt2Max);
        uint64_t now = now_us();
        if (pc5_raw > PC5_RSSI_VALID_ABOVE) {
            last_valid_pc5 = pc5_raw;
            last_valid_us  = now;
        } else if (last_valid_us != 0 && now - last_valid_us < PC5_RSSI_HOLD_US) {
            pc5_raw = last_valid_pc5;      // 일시적 측정 공백 → 직전값 유지
        }

        g_uu_rssi_snapshot.store(uu_metric.rssi);
        g_pc5_rssi_snapshot.store(pc5_raw);

        // [터미널] 화면 표시용 — 여기엔 홀드 없이 원값을 넣어
        // 홀드가 걸렸는지 눈으로 구분할 수 있게 한다.
        g_uu_rsrp.store(uu_metric.rsrp);
        g_uu_rsrq.store(uu_metric.rsrq);
        g_uu_sinr.store(uu_metric.sinr);
        g_pc5_ant1.store(pc5_metric.rssiAnt1Max);
        g_pc5_ant2.store(pc5_metric.rssiAnt2Max);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ===================== 스레드 =====================
void inputThread() {
    char c;
    while (keepRunning) { std::cin >> c; if (c=='q'||c=='Q') keepRunning=false; }
}

void quicSendThread(picoquic_quic_t* quic) {
    uint8_t buf[1536];
    while (keepRunning) {
        size_t sendLen = 0;
        struct sockaddr_storage peer, local;
        int if_idx = 0;
        picoquic_connection_id_t log_cid;
        picoquic_cnx_t* last_cnx = nullptr;
        size_t msg_sz = 0;
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            int ret = picoquic_prepare_next_packet_ex(quic, picoquic_current_time(),
                buf, sizeof(buf), &sendLen, &peer, &local,
                &if_idx, &log_cid, &last_cnx, &msg_sz);
            if (ret != 0) continue;
        }
        if (sendLen > 0)
            dispatchQuicPacket(buf, sendLen, &peer, &local);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void pc5RecvThread(picoquic_quic_t* quic) {
    uint8_t rxBuf[9100] = {0};
    while (keepRunning) {
        int msgLen = Seamless_RecvPc5Raw(rxBuf);
        if (msgLen <= 0 || rxBuf[0]<0x40) {
            // SDK 가 논블로킹이면 여기서 코어 하나를 태운다.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // [제거됨] 0xEE pong 처리 + smoothed_rtt 강제 주입.
        //   §5.4 는 RTT 를 PATH_ACK 수신으로 산출하라고 규정한다.
        //   smoothed_rtt 만 덮어쓰고 rtt_variant / retransmit_timer 를
        //   갱신하지 않으면 §5.7 의 PTO 계산과 demotion_time 이
        //   서로 어긋난다. 이제 RTT 는 picoquic 이 계산한 값을 읽는다.

        // QUIC으로 데이터 넘기기
        std::lock_guard<std::mutex> lk(quic_mutex);
        picoquic_incoming_packet(quic, rxBuf, msgLen,
            (struct sockaddr*)&pc5_remote_addr,
            (struct sockaddr*)&pc5_local_addr,
            0, 0, picoquic_current_time());
    }
}

void uuRecvThread(picoquic_quic_t* quic) {
    uint8_t rxBuf[2048];
    while (keepRunning) {
        struct sockaddr_storage from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_uu_sock, rxBuf, sizeof(rxBuf), 0,
                             (struct sockaddr*)&from, &fl);
        if (n > 0) {
            g_last_rx_uu_us.store(now_us());         // [터미널] Uu liveness
            std::lock_guard<std::mutex> lk(quic_mutex);
            picoquic_incoming_packet(quic, rxBuf, n,
                (struct sockaddr*)&from, (struct sockaddr*)&uu_local_addr,
                0, 0, picoquic_current_time());
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // 에러 무시
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void multipathProbeThread(picoquic_quic_t* quic) {
    while (keepRunning && !g_uu_handshake_done)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!keepRunning) return;

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    for (int i = 0; i < 20 && keepRunning && !g_pc5_path_added; i++) {
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            if (!g_cnx->is_multipath_enabled) break;
            int ret = picoquic_probe_new_path_ex(g_cnx,
                (const struct sockaddr*)&pc5_remote_addr,
                (const struct sockaddr*)&pc5_local_addr,
                0, picoquic_current_time(), 0);
            if (ret == 0) {
                g_probe_ok++;
                UI_PROBE("PC5 초기 probe #%d  \u2192 OK", i + 1);
                break;
            } else {
                g_probe_fail++;
                UI_PROBE("PC5 초기 probe #%d  \u2192 실패 (ret=%d)", i + 1, ret);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // ── PC5 재probe 루프 ────────────────────────────────────
    //
    //  [수정 1] 원본의 pending 검사는 nb_paths 에 PC5 주소를 가진
    //     경로가 하나라도 있으면 probe 를 막았다. path_is_demoted
    //     검사가 없어서, 죽었지만 아직 삭제되지 않은 경로 하나가
    //     재probe 를 영구 차단했다.

    //  [수정 2] 커버리지 밖에서 1초마다 probe 를 던지면 path ID 가
    //     몇 초 만에 고갈된다(§3.4 재사용 금지). 지수 백오프를 쓴다.

    //  [수정 3] 판정은 scheduler_path_status() 하나로 통일한다.
    uint64_t backoff_ms = 1000;
    const uint64_t BACKOFF_MAX_MS    = 30000;
    const uint64_t FALLBACK_PROBE_US = 30000000ULL;   // 30초
    uint64_t last_probe_us = 0;
    bool blocked_reported = false;

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        scheduler_path_info_t uu = {}, pc5 = {};
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            if (!g_cnx || !g_cnx->is_multipath_enabled) continue;
            scheduler_path_status(g_cnx, &uu, &pc5);
        }

        // 경로 객체가 남아 있으면(죽었든 살았든) 새 probe 는 불가.
        // picoquic 이 삭제할 때까지 기다린다.
        if (pc5.present) {
            backoff_ms = 1000;
            blocked_reported = false;
            continue;
        }

                uint64_t now = now_us();
        int  rssi = g_pc5_rssi_snapshot.load();

        // ── 재연결 트리거 ────────────────────────────────────
        //  RSU 가 PC5 로 주기 방송(WSA)을 하므로, PC5 RSSI 가 올라왔다는
        //  것은 RSU 신호가 닿았다 = 커버리지에 진입했다는 뜻이다.
        //  실험 환경에 RSU 1대 + OBU 1대뿐이고 OBU 는 자기 송신을
        //  수신하지 않으므로 다른 해석의 여지가 없다.
        //
        //  [주의] RSSI 를 probe "차단" 조건으로 쓰면 안 된다.
        //  PC5 RSSI 는 수신 기반이라, 방송이 없는 환경에서는 경로가
        //  끊긴 뒤 영영 -95 에 머물러 재연결이 영구 차단된다.
        //  그래서 신호가 없어도 주기적으로는 시도한다.
        bool rssi_trigger = (rssi > PC5_PROBE_RSSI_FLOOR);
        bool time_trigger = (last_probe_us == 0) ||
                            (now - last_probe_us > FALLBACK_PROBE_US);

        if (!rssi_trigger && !time_trigger) continue;

        if (rssi_trigger) {
            backoff_ms = 1000;      // 신호가 있으면 즉시·연속 시도
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            if (!keepRunning) break;
        }

        int ret;
        {
            std::lock_guard<std::mutex> lk(quic_mutex);
            if (!g_cnx) break;
            ret = picoquic_probe_new_path_ex(g_cnx,
                (const struct sockaddr*)&pc5_remote_addr,
                (const struct sockaddr*)&pc5_local_addr,
                0, picoquic_current_time(), 0);
        }

        last_probe_us = now;

        if (ret == 0) {
            g_pc5_need_probe = false;
            backoff_ms = 1000;
            blocked_reported = false;
            UI_PROBE("PC5 재probe \u2192 OK  (%s, rssi %ddBm, 누적 성공 %d)",
                     rssi_trigger ? "RSSI 트리거" : "주기 시도",
                     rssi, g_probe_ok.fetch_add(1) + 1);
        } else {
            int nf = g_probe_fail.fetch_add(1) + 1;
#ifdef PICOQUIC_ERROR_PATH_ID_BLOCKED
            if (ret == PICOQUIC_ERROR_PATH_ID_BLOCKED) {
                if (!blocked_reported) {
                    UI_BAD("PC5 재probe 실패 \u2014 PATH_ID_BLOCKED "
                           "(path ID %d 개 소진, MAX_PATH_ID 없이는 복구 불가)",
                           MAX_PATH_ID);
                    blocked_reported = true;
                }
            } else
#endif
            {
                UI_PROBE("PC5 재probe \u2192 실패 (ret=%d, rssi %ddBm)  "
                         "누적실패 %d  다음 시도 %.1fs 후",
                         ret, rssi, nf, backoff_ms / 1000.0);
            }
            backoff_ms = (backoff_ms * 2 > BACKOFF_MAX_MS)
                       ? BACKOFF_MAX_MS : backoff_ms * 2;
        }
    }
}

// ── 터미널 상태 블록 (1초 주기) ──────────────────────────────
void senderStatusThread() {
    UiRate uu_rate, pc5_rate, enc_rate;
    uint64_t prev_pkt_uu = 0, prev_pkt_pc5 = 0, prev_tick_us = 0;    //구간 pkt/s 계산용
    bool warned_uu_idle = false;
    int  tick = 0;

    while (keepRunning && !g_uu_handshake_done)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    while (keepRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!keepRunning) break;

        if (++tick < 10) continue;          // 1초마다만 블록 출력
        tick = 0;

        uint64_t now    = now_us();
        uint64_t tx_uu  = g_last_tx_uu_us.load();
        uint64_t tx_pc5 = g_last_tx_pc5_us.load();

        // ── 경로 상태: picoquic 의 판정 + 스케줄러의 실제 선택 ──
        scheduler_path_info_t uu, pc5;
        { std::lock_guard<std::mutex> lk(g_pinfo_mtx);
          uu = g_pinfo_uu; pc5 = g_pinfo_pc5; }

        int act = scheduler_get_last_sel_rat();
        const char* cur_name = (act == 1) ? "PC5" : (act == 0) ? "Uu " : "-- ";

        double uu_mbps  = uu_rate.mbps(g_tx_byte_uu.load(),  now);
        double pc5_mbps = pc5_rate.mbps(g_tx_byte_pc5.load(), now);
        double enc_mbps = enc_rate.mbps(g_enc_bytes.load(),  now);

        uint64_t cur_pkt_uu  = g_tx_pkt_uu.load();
        uint64_t cur_pkt_pc5 = g_tx_pkt_pc5.load();
        double   el_s = prev_tick_us ? (now - prev_tick_us) / 1e6 : 1.0;
        if (el_s <= 0.0) el_s = 1.0;
        double pps_uu  = (cur_pkt_uu  >= prev_pkt_uu)
                       ? (cur_pkt_uu  - prev_pkt_uu)  / el_s : 0.0;
        double pps_pc5 = (cur_pkt_pc5 >= prev_pkt_pc5)
                       ? (cur_pkt_pc5 - prev_pkt_pc5) / el_s : 0.0;
        prev_pkt_uu = cur_pkt_uu; prev_pkt_pc5 = cur_pkt_pc5; prev_tick_us = now;

        // ACTIVE reflects actual packet flow during the last status interval.
        // With RR and two healthy paths, both paths should normally be ACTIVE.
        const double ACTIVE_PPS = 10.0;
        auto classify = [&](const scheduler_path_info_t& pi, double pps) -> UiPathState {
            if (!pi.present || !pi.usable) return PS_DEAD;
            return (pps >= ACTIVE_PPS) ? PS_ACTIVE : PS_STANDBY;
        };
        UiPathState st_uu  = classify(uu,  pps_uu);
        UiPathState st_pc5 = classify(pc5, pps_pc5);

        int nb = 0;
        { std::lock_guard<std::mutex> lk(quic_mutex); if (g_cnx) nb = g_cnx->nb_paths; }

        bool fix;
        { std::lock_guard<std::mutex> lk(g_gps_mtx);
          fix = g_gps_snap.fixed; }

        // 1행 — 헤더
        printf("\n%s %s\u25B6 last=%s%s   cam %.1ffps  enc %.2fMbps  drop %llu%s\n",
               ui_stamp(), cBLD(), cur_name, cRST(),
               g_cam_fps.load(), enc_mbps,
               (unsigned long long)g_cam_drop.load(),
               fix ? "" : "   (GPS no fix)");

        // 2행 — Uu
        printf("|  Uu  |  %s  |  %4ddBm  |  %5.2fMbps  |  %4.0fpkt/s  |  "
            "누적 %6llu pkt  |  tx %s전\n",
            ui_state(st_uu),
            g_uu_rssi_snapshot.load(),
            uu_mbps,
            pps_uu,
            (unsigned long long)cur_pkt_uu,
            ui_age(tx_uu, now));

        // 3행 — PC5
        printf("|  PC5 |  %s  |  %4ddBm  |  %5.2fMbps  |  %4.0fpkt/s  |  "
            "누적 %6llu pkt  |  tx %s전\n",
            ui_state(st_pc5),
            g_pc5_rssi_snapshot.load(),
            pc5_mbps,
            pps_pc5,
            (unsigned long long)cur_pkt_pc5,
            ui_age(tx_pc5, now));

        // 4행 — round-robin path selections
        uint64_t rr_uu = 0, rr_pc5 = 0;
        scheduler_get_sel_counts(&rr_uu, &rr_pc5);
        printf("|  RR  |  paths %d  |  selections Uu %llu / PC5 %llu  |  probe %d/%d  |\n",
               nb,
               (unsigned long long)rr_uu,
               (unsigned long long)rr_pc5,
               g_probe_ok.load(), g_probe_fail.load());
        fflush(stdout);


        if (g_verbose) {
            auto fmt_rtt = [](char* buf, size_t n,
                            const scheduler_path_info_t& pi) {
                if (!pi.present || pi.rtt_sample == 0) {
                    snprintf(buf, n, "--");
                } else {
                    snprintf(buf, n, "%.1f ms",
                            pi.rtt_sample / 1000.0);
                }
            };

            char rtt_uu[32];
            char rtt_pc5[32];

            fmt_rtt(rtt_uu, sizeof(rtt_uu), uu);
            fmt_rtt(rtt_pc5, sizeof(rtt_pc5), pc5);

            printf("  %sRTT  Uu[%s]   PC5[%s]%s\n",
                cDIM(),
                rtt_uu,
                rtt_pc5,
                cRST());
        }

        // Uu 가 사라졌거나 사용 불가인데 PC5 로만 가고 있으면 알린다.
        // 경로가 삭제되면(present=0) usable 이 영영 돌아오지 않으므로
        // present 기준으로 해제한다.
        if (!warned_uu_idle && !uu.usable && pc5.usable) {
            UI_WARN("Uu 경로 %s \u2014 PC5 단독 운용 중%s",
                    uu.present ? "사용 불가" : "삭제됨",
                    uu.present ? "" : " (재probe 없음)");
            warned_uu_idle = true;
        }
        if (uu.present && uu.usable) warned_uu_idle = false;
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
        "  --uu-peer IP          Uu receiver/server address\n"
        "  --uu-port PORT        Uu receiver UDP port\n"
        "  --pc5-local IP        local PC5 endpoint address\n"
        "  --pc5-peer IP         remote PC5 endpoint address\n"
        "  --gps-host HOST       gpsd host (default: 192.168.70.102)\n"
        "  --gps-port PORT       gpsd port (default: 4545)\n"
        "  --camera INDEX        V4L2 camera index (default: 0)\n"
        "  --exp NAME            experiment label\n"
        "  --verbose, -v         print raw path RTT samples\n"
        "  --no-color            disable ANSI colors\n",
        prog);
}

// ===================== MAIN =====================
int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    g_color = (isatty(1) != 0);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uu-peer") == 0 && i + 1 < argc) {
            if (!copy_arg(g_uu_receiver_ip, sizeof(g_uu_receiver_ip), argv[++i])) return -1;
            continue;
        }
        if (strcmp(argv[i], "--uu-port") == 0 && i + 1 < argc) {
            g_uu_receiver_port = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--pc5-local") == 0 && i + 1 < argc) {
            if (!copy_arg(g_pc5_sender_ip, sizeof(g_pc5_sender_ip), argv[++i])) return -1;
            continue;
        }
        if (strcmp(argv[i], "--pc5-peer") == 0 && i + 1 < argc) {
            if (!copy_arg(g_pc5_receiver_ip, sizeof(g_pc5_receiver_ip), argv[++i])) return -1;
            continue;
        }
        if (strcmp(argv[i], "--gps-host") == 0 && i + 1 < argc) {
            if (!copy_arg(g_gps_host, sizeof(g_gps_host), argv[++i])) return -1;
            continue;
        }
        if (strcmp(argv[i], "--gps-port") == 0 && i + 1 < argc) {
            if (!copy_arg(g_gps_port, sizeof(g_gps_port), argv[++i])) return -1;
            continue;
        }
        if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            g_camera_index = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--exp") == 0 && i + 1 < argc) {
            g_exp_name = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--no-color") == 0) {
            g_color = false;
            continue;
        }
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        print_usage(argv[0]);
        return -1;
    }

    ui_start();
    ui_rule();
    printf("  %sV2X MP-QUIC Sender%s                        %sexp=%s  role=OBU%s\n",
           cBLD(), cRST(), cDIM(), g_exp_name, cRST());
    printf("  %sscheduler round-robin   cc bbr   max_path_id %d%s\n",
           cDIM(), MAX_PATH_ID, cRST());
    printf("  %scamera %dx%d@%dfps -> H264 1.0Mbps -> RTP%s\n",
           cDIM(), CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS, cRST());
    printf("  %sUu  %s:%d       PC5  %s -> %s%s\n",
           cDIM(), UU_RECEIVER_IP, UU_RECEIVER_PORT,
           PC5_SENDER_IP, PC5_RECEIVER_IP, cRST());
    printf("  %sexit: q + Enter%s\n", cDIM(), cRST());
    ui_rule();

    initAddresses();

    if (!initSirius(PC5_SENDER_IP)) {
        UI_BAD("V2X init ERROR");
        return -1;
    }

    g_uu_sock = initUuSocket();
    if (g_uu_sock < 0) return -1;

    picoquic_quic_t *quic = initQuic();
    if (!quic) return -1;

    std::thread send_thr(quicSendThread, quic);
    std::thread uu_thr(uuRecvThread, quic);
    std::thread pc5_thr(pc5RecvThread, quic);
    std::thread mp_thr(multipathProbeThread, quic);
    std::thread path_thr(pathStatusThread, quic);
    std::thread gps_thr(gpsThread);
    std::thread rssi_thr(rssiUpdateThread);
    std::thread stat_thr(senderStatusThread);

    GstElement* pipeline = initGStreamerPipeline();
    if (!pipeline) return -1;
    initGStreamerApp(pipeline);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    cv::VideoCapture cap;
    cap.open(g_camera_index, cv::CAP_V4L2);
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  CAMERA_WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, CAMERA_HEIGHT);
    cap.set(cv::CAP_PROP_FPS,          CAMERA_FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE,   1);
    if (!cap.isOpened()) { UI_BAD("카메라 열기 실패"); return -1; }

    std::thread input_thr(inputThread);

    cv::Mat frame;
    GstClockTime ts = 0;
    const GstClockTime dur = GST_SECOND / CAMERA_FPS;
    uint64_t frameId = 0;

    auto fps_t0 = std::chrono::steady_clock::now();
    uint64_t fps_count = 0;

    UI_OK("영상 전송 시작");
    while (keepRunning) {
        cap >> frame;
        if (frame.empty()) { g_cam_drop.fetch_add(1); continue; }

        fps_count++;
        auto fps_now = std::chrono::steady_clock::now();
        double el = std::chrono::duration<double>(fps_now - fps_t0).count();
        if (el >= 1.0) {
            g_cam_fps.store(fps_count / el);
            fps_count = 0; fps_t0 = fps_now;
        }

        gsize sz = frame.total() * frame.elemSize();
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, sz, nullptr);
        GstMapInfo map;
        gst_buffer_map(buf, &map, GST_MAP_WRITE);
        memcpy(map.data, frame.data, sz);
        gst_buffer_unmap(buf, &map);
        GST_BUFFER_PTS(buf)      = ts;
        GST_BUFFER_DURATION(buf) = dur;
        ts += dur;

        g_current_frame_id.store(frameId);
        sendFrameMeta(frameId);
        gst_app_src_push_buffer(g_appsrc, buf);
        frameId++;
    }

    gst_app_src_end_of_stream(g_appsrc);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(g_appsrc);
    gst_object_unref(g_appsink);
    gst_object_unref(pipeline);
    cap.release();

    input_thr.join(); send_thr.join();
    uu_thr.join(); pc5_thr.join(); mp_thr.join(); path_thr.join();
    gps_thr.join();
    rssi_thr.join();
    stat_thr.join();

    picoquic_free(quic);
    if (g_uu_sock >= 0) close(g_uu_sock);
    Seamless_FinalizePc5Raw();

    // ── 종료 요약 ──
    ui_rule();
    printf("  %s실험 %s 종료  (%.1f초)%s\n",
           cBLD(), g_exp_name, ui_elapsed_s(), cRST());
    printf("  %s송신  Uu %llu pkt / %s        PC5 %llu pkt / %s%s\n",
           cDIM(),
           (unsigned long long)g_tx_pkt_uu.load(), ui_bytes(g_tx_byte_uu.load()),
           (unsigned long long)g_tx_pkt_pc5.load(), ui_bytes(g_tx_byte_pc5.load()),
           cRST());
    printf("  %sprobe OK%d / FAIL%d        카메라 drop %llu%s\n",
           cDIM(), g_probe_ok.load(), g_probe_fail.load(),
           (unsigned long long)g_cam_drop.load(), cRST());
    {
        uint64_t su = 0, sp = 0;
        scheduler_get_sel_counts(&su, &sp);
        printf("  %sRR selections  Uu %llu / PC5 %llu%s\n",
               cDIM(),
               (unsigned long long)su,
               (unsigned long long)sp,
               cRST());
    }
    ui_rule();
    return 0;
}