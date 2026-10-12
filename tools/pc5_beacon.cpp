/*
 * Optional RSU-side PC5 raw beacon for coverage/re-entry experiments.
 *
 * The beacon is not required for steady-state round-robin MP-QUIC. It is
 * useful when the deployed RSU does not otherwise emit periodic PC5 traffic
 * from which the OBU can refresh its RSSI estimate.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

extern "C" {
    // 메시지 타입 헤더 — receiver 와 동일한 순서
    #include "etf-types.h"
    #include "etf_ksr1600_j2735_enum.h"
    #include "etf_sdk_wsmp_psid_kr.h"
    #include "etf_j2540.h"
    #include "etf_ksr1600_j2735_data_element.h"
    #include "etf_ksr1600_kor_data_element.h"
    #include "etf_ksr1600_j2735_data_frame.h"
    #include "etf_ksr1600_kor_data_frame.h"
    #include "etf_ksr1600_j2735_message.h"
    #include "etf_j3224.h"

    #include "seamless_app_v2x_client.h"

    #include <unistd.h>
    #include <signal.h>
    #include <sys/time.h>
}

// ── 출력 유틸 ────────────────────────────────────────────────
static bool g_color = true;
static inline const char* CC(const char* s) { return g_color ? s : ""; }
static inline const char* cRST() { return CC("\033[0m");  }
static inline const char* cBLD() { return CC("\033[1m");  }
static inline const char* cDIM() { return CC("\033[2m");  }
static inline const char* cRED() { return CC("\033[31m"); }
static inline const char* cGRN() { return CC("\033[32m"); }
static inline const char* cYEL() { return CC("\033[33m"); }

static uint64_t g_t0 = 0;

static inline uint64_t now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}
static inline double elapsed_s() {
    return g_t0 ? (now_us() - g_t0) / 1e6 : 0.0;
}
static void ev(const char* col, const char* icon, const char* fmt, ...) {
    char m[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    printf("  %s%s%s %s[+%06.1fs]%s %s\n",
           col, icon, cRST(), cDIM(), elapsed_s(), cRST(), m);
    fflush(stdout);
}
#define EV_OK(...)   ev(cGRN(), "\u2713", __VA_ARGS__)
#define EV_BAD(...)  ev(cRED(), "\u2717", __VA_ARGS__)
#define EV_WARN(...) ev(cYEL(), "\u26A0", __VA_ARGS__)

static void rule() {
    char l[96]; memset(l, '=', 79); l[79] = 0;
    printf("%s%s%s\n", cDIM(), l, cRST());
}

// ── 비콘 페이로드 ────────────────────────────────────────────
//   seq 를 실어 보내므로, 필요하면 수신측에서 순번 간격으로
//   비콘 손실률(= PC5 링크 품질의 독립 지표)을 얻을 수 있다.
#pragma pack(push, 1)
struct Pc5Beacon {
    uint8_t  magic[4];      // 'R' 'S' 'U' 'B'
    uint8_t  version;       // 1
    uint8_t  reserved;
    uint16_t interval_ms;   // 송신 주기
    uint64_t seq;           // 송신 순번
    uint64_t t_tx_us;       // 송신 시각
};
#pragma pack(pop)

static std::atomic<bool> keepRunning(true);
static char g_pc5_local_ip[64] = "10.254.52.18";
static void onSignal(int) { keepRunning = false; }

static void inputThread() {
    char c;
    while (keepRunning) {
        std::cin >> c;
        if (c == 'q' || c == 'Q') keepRunning = false;
    }
}

// ── MAIN ─────────────────────────────────────────────────────
int main(int argc, char** argv) {
    g_color = (isatty(1) != 0);
    signal(SIGINT,  onSignal);
    signal(SIGTERM, onSignal);

    double hz = 1.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--hz") == 0 && i + 1 < argc) {
            hz = atof(argv[++i]);
            if (hz <= 0.0 || hz > 50.0) {
                printf("--hz must be in (0, 50].\n"); return -1;
            }
        } else if (strcmp(argv[i], "--pc5-local") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (strlen(v) >= sizeof(g_pc5_local_ip)) return -1;
            snprintf(g_pc5_local_ip, sizeof(g_pc5_local_ip), "%s", v);
        } else if (strcmp(argv[i], "--no-color") == 0) {
            g_color = false;
        } else {
            printf("Usage: %s [--hz N] [--pc5-local IP] [--no-color]\n", argv[0]);
            return -1;
        }
    }

    const uint32_t interval_ms = (uint32_t)(1000.0 / hz + 0.5);

    g_t0 = now_us();
    rule();
    printf("  %sPC5 Beacon%s   %sRSU 주기 방송 (ETRI WSA 환경 재현)%s\n",
           cBLD(), cRST(), cDIM(), cRST());
    printf("  %s채널 PC5-V2X   주기 %.1fHz (%ums)   페이로드 %zuB%s\n",
           cDIM(), hz, interval_ms, sizeof(Pc5Beacon), cRST());
    printf("  %s종료: q + Enter 또는 Ctrl+C%s\n", cDIM(), cRST());
    rule();

    int ret = Seamless_InitializePc5Raw(g_pc5_local_ip);
    if (ret <= 0) {
        EV_BAD("Seamless_InitializePc5Raw 실패 (ret=%d)", ret);
        return -1;
    }
    EV_OK("PC5-RAW channel initialized (%s)", g_pc5_local_ip);

    std::thread in_thr(inputThread);

    Pc5Beacon b;
    memset(&b, 0, sizeof(b));
    b.magic[0] = 0x1B; b.magic[1] = 0x5C; b.magic[2] = 'R'; b.magic[3] = 'S';
    b.version     = 1;
    b.interval_ms = (uint16_t)interval_ms;

    uint64_t sent = 0, failed = 0;
    uint64_t last_report_us = now_us();
    int      last_ret = 0;

    EV_OK("방송 시작");

    while (keepRunning) {
        b.seq     = sent;
        b.t_tx_us = now_us();

        int r = Seamless_SendPc5Raw((uint8_t*)&b, (uint16_t)sizeof(b));
        if (r > 0) {
            sent++;
        } else {
            failed++;
            // 첫 실패와 오류코드가 바뀐 경우만 보고 (로그 폭주 방지)
            if (failed == 1 || r != last_ret) {
                EV_WARN("송신 실패 ret=%d (누적 실패 %llu)",
                        r, (unsigned long long)failed);
                last_ret = r;
            }
        }

        // 5초마다 상태 한 줄
        uint64_t n = now_us();
        if (n - last_report_us >= 5000000ULL) {
            uint64_t tot = sent + failed;
            printf("  %s[+%06.1fs]%s  %s%.1fHz%s   송신 %llu   실패 %llu (%.1f%%)\n",
                   cDIM(), elapsed_s(), cRST(), cBLD(), hz, cRST(),
                   (unsigned long long)sent,
                   (unsigned long long)failed,
                   tot ? 100.0 * failed / tot : 0.0);
            fflush(stdout);
            last_report_us = n;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    EV_OK("방송 종료");
    Seamless_FinalizePc5Raw();

    // std::cin 에서 블록될 수 있으므로 detach
    in_thr.detach();

    rule();
    printf("  %s총 송신 %llu   실패 %llu   가동 %.1f초%s\n",
           cDIM(), (unsigned long long)sent, (unsigned long long)failed,
           elapsed_s(), cRST());
    rule();
    return 0;
}