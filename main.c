#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <getopt.h>
#include <sched.h>
#include <sys/mman.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "sv_publisher.h"

/* ============================================================
 * Константы конфигурации
 * ============================================================ */
#define SAMPLES_PER_PERIOD    80       /* образцов на период (smpRate)    */
#define NOMINAL_FREQ_HZ       50       /* номинальная частота сети, Гц    */
#define SAMPLES_PER_SEC       (SAMPLES_PER_PERIOD * NOMINAL_FREQ_HZ)  /* 4000 */
#define INTERVAL_NS           250000   /* 1e9 / SAMPLES_PER_SEC = 250 мкс */
#define SIN_AMPLITUDE         10000.0  /* амплитуда синусоиды             */
#define RT_PRIORITY           50       /* приоритет SCHED_FIFO            */
#define MAX_IFACE_LEN         64       /* макс. длина имени интерфейса    */

/* ============================================================
 * Логирование с уровнями
 * ============================================================ */
#define LOG_INFO(fmt, ...)  printf("[INFO]  " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_CRIT(fmt, ...)  fprintf(stderr, "[CRITICAL] " fmt, ##__VA_ARGS__)

/* ============================================================
 * Lookup table для sin() — 80 предрассчитанных значений
 * Заполняется один раз при старте.
 * ============================================================ */
static int32_t sin_lut[SAMPLES_PER_PERIOD];

static void init_sin_lut(void) {
    for (int i = 0; i < SAMPLES_PER_PERIOD; i++) {
        double angle = 2.0 * M_PI * ((double)i / (double)SAMPLES_PER_PERIOD);
        sin_lut[i] = (int32_t)(SIN_AMPLITUDE * sin(angle));
    }
}

static inline int32_t get_sin_value(uint16_t idx) {
    return sin_lut[idx % SAMPLES_PER_PERIOD];
}

/* ============================================================
 * POSIX fallback для времени, не зависит от HAL
 * ============================================================ */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        LOG_ERROR("clock_gettime failed: %s\n", strerror(errno));
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ============================================================
 * Сигналы
 * ============================================================ */
static volatile sig_atomic_t running = 1;

static void sigint_handler(int signalId) {
    (void)signalId;
    running = 0;
}

/* ============================================================
 * Структура буфера для REORDER-искажения
 * ============================================================ */
typedef struct {
    bool    active;   /* буфер заполнен и ждёт вставки    */
    uint16_t cnt;     /* сохранённый SmpCnt                */
    uint64_t t;       /* время захвата (нс)                */
    int32_t  i1, i2, i3; /* значения токов                  */
    uint32_t q1, q2, q3; /* значения качества (Quality тип) */
} ReorderBuf_t;

/* Глобальная переменная для нулевого значения Quality */
static const Quality QUALITY_INVALID = 0;

/* ============================================================
 * Помощник: безопасный парсинг uint16_t
 * Возвращает -1 при ошибке, иначе 0.
 * ============================================================ */
static int parse_uint16_arg(const char* str, const char* arg_name, uint16_t* out) {
    if (!str || !out) return -1;

    char* endptr = NULL;
    errno = 0;
    unsigned long val = strtoul(str, &endptr, 10);

    if (errno == ERANGE || val > UINT16_MAX) {
        LOG_ERROR("Argument --%s: value out of range (0..%u)\n", arg_name, UINT16_MAX);
        return -1;
    }
    if (endptr == str || *endptr != '\0') {
        LOG_ERROR("Argument --%s: invalid number '%s'\n", arg_name, str);
        return -1;
    }
    *out = (uint16_t)val;
    return 0;
}

/* ============================================================
 * Инициализация буфера ReorderBuf_t
 * ============================================================ */
static void init_reorder_buf(ReorderBuf_t* rbuf) {
    if (!rbuf) return;
    rbuf->active = false;
    rbuf->cnt = 0;
    rbuf->t = 0;
    rbuf->i1 = rbuf->i2 = rbuf->i3 = 0;
    rbuf->q1 = rbuf->q2 = rbuf->q3 = 0;
}

/* ============================================================
 * Валидация имени сетевого интерфейса
 * ============================================================ */
static bool validate_iface(const char* iface) {
    if (!iface || iface[0] == '\0') {
        LOG_ERROR("Interface name cannot be empty\n");
        return false;
    }
    /* Проверка на недопустимые символы: только буквы, цифры, '-', '_' */
    for (const char* p = iface; *p; p++) {
        if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') &&
            (*p < '0' || *p > '9') && *p != '-' && *p != '_') {
            LOG_ERROR("Interface name contains invalid character: '%c'\n", *p);
            return false;
        }
    }
    return true;
}

/* ============================================================
 * Настройка real-time приоритетов
 * ============================================================ */
static void setup_rt_priority(void) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = RT_PRIORITY;

    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        LOG_WARN("Не удалось установить SCHED_FIFO (priority=%d). Запустите с sudo.\n", RT_PRIORITY);
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        LOG_WARN("mlockall failed. Запустите с sudo.\n");
    }
}

/* ============================================================
 * Парсинг аргументов командной строки
 * ============================================================ */
typedef struct {
    char     iface[MAX_IFACE_LEN];
    bool     drop_enabled;
    bool     reorder_enabled;
    uint16_t drop_start;
    uint16_t drop_count;
    uint16_t reorder_delay;
    uint16_t reorder_insert;
} CliConfig_t;

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [OPTIONS] <interface>\n", prog);
    fprintf(stderr, "  -i, --iface IFACE              Network interface (default: eth0)\n");
    fprintf(stderr, "      --drop START COUNT           Drop distortion (START = first sample index to drop, COUNT = number of samples)\n");
    fprintf(stderr, "      --reorder DELAY INSERT       Reorder distortion (DELAY = capture at sample, INSERT = replay at sample)\n");
    fprintf(stderr, "  -h, --help                       Show this help message\n");
    fprintf(stderr, "\nNotes:\n");
    fprintf(stderr, "  For --reorder: INSERT must be > DELAY (captured frame is replayed later)\n");
    fprintf(stderr, "  Sample indices wrap at %u (1 second at 80 samples/period, 50 Hz)\n\n", SAMPLES_PER_SEC);
}

static int parse_args(int argc, char** argv, CliConfig_t* cfg) {
    /* Инициализация по умолчанию */
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->iface, "eth0", MAX_IFACE_LEN - 1);

    struct option long_options[] = {
        {"iface",   required_argument, 0, 'i'},
        {"drop",    required_argument, 0, 0},
        {"reorder", required_argument, 0, 0},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt, option_index = 0;
    while ((opt = getopt_long(argc, argv, "i:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'i': {
                if (strlen(optarg) >= MAX_IFACE_LEN) {
                    LOG_ERROR("Interface name too long (max %d chars)\n", MAX_IFACE_LEN - 1);
                    return -1;
                }
                strncpy(cfg->iface, optarg, MAX_IFACE_LEN - 1);
                cfg->iface[MAX_IFACE_LEN - 1] = '\0';
                break;
            }
            case 'h':
                print_usage(argv[0]);
                return 1; /* специальный код — показать help и выйти */
            case 0: {
                const char* name = long_options[option_index].name;
                if (strcmp(name, "drop") == 0) {
                    if (parse_uint16_arg(optarg, "drop", &cfg->drop_start) != 0)
                        return -1;
                    if (optind < argc && argv[optind][0] != '-') {
                        if (parse_uint16_arg(argv[optind], "drop", &cfg->drop_count) != 0)
                            return -1;
                        optind++;
                        cfg->drop_enabled = true;
                    } else {
                        LOG_ERROR("--drop requires two arguments: START COUNT\n");
                        return -1;
                    }
                } else if (strcmp(name, "reorder") == 0) {
                    if (parse_uint16_arg(optarg, "reorder", &cfg->reorder_delay) != 0)
                        return -1;
                    if (optind < argc && argv[optind][0] != '-') {
                        if (parse_uint16_arg(argv[optind], "reorder", &cfg->reorder_insert) != 0)
                            return -1;
                        optind++;
                        cfg->reorder_enabled = true;
                    } else {
                        LOG_ERROR("--reorder requires two arguments: DELAY INSERT\n");
                        return -1;
                    }
                }
                break;
            }
            default:
                print_usage(argv[0]);
                return -1;
        }
    }

    /* Позиционный аргумент interface (если указан после опций) */
    if (optind < argc) {
        if (strlen(argv[optind]) >= MAX_IFACE_LEN) {
            LOG_ERROR("Interface name too long (max %d chars)\n", MAX_IFACE_LEN - 1);
            return -1;
        }
        strncpy(cfg->iface, argv[optind], MAX_IFACE_LEN - 1);
        cfg->iface[MAX_IFACE_LEN - 1] = '\0';
    }

    /* Валидация интерфейса */
    if (!validate_iface(cfg->iface))
        return -1;

    /* Валидация reorder: INSERT должен быть > DELAY */
    if (cfg->reorder_enabled && cfg->reorder_insert <= cfg->reorder_delay) {
        LOG_ERROR("--reorder: INSERT (%u) must be greater than DELAY (%u)\n",
                  cfg->reorder_insert, cfg->reorder_delay);
        return -1;
    }

    return 0;
}

/* ============================================================
 * Создание и настройка SVPublisher
 * ============================================================ */
static SVPublisher create_publisher(const char* iface, SVPublisher_ASDU* out_asdu,
                                     int* out_idx_i1, int* out_idx_q1,
                                     int* out_idx_i2, int* out_idx_q2,
                                     int* out_idx_i3, int* out_idx_q3) {
    CommParameters svParams = {
        .vlanPriority = 4,
        .vlanId       = 1,
        .appId        = 0x4000,
        .dstAddress   = {0x01, 0x0C, 0xCD, 0x04, 0x00, 0x01}
    };

    LOG_INFO("Creating SVPublisher on '%s'...\n", iface);
    fflush(stdout);

    SVPublisher pub = SVPublisher_createEx(&svParams, iface, true);
    if (!pub) {
        LOG_CRIT("SVPublisher_createEx failed!\n");
        return NULL;
    }

    LOG_INFO("Configuring ASDU...\n");
    fflush(stdout);

    /* Параметры из CID-файла */
    SVPublisher_ASDU asdu = SVPublisher_addASDU(pub,
        "SV_FAULT_INJECTORLD1/LLN0.S1",
        "SVDataSet",
        1001);
    if (!asdu) {
        LOG_ERROR("addASDU failed\n");
        SVPublisher_destroy(pub);
        return NULL;
    }

    /* Параметры ДО setupComplete (влияют на размер кадра) */
    SVPublisher_ASDU_setSmpRate(asdu, SAMPLES_PER_PERIOD);
    SVPublisher_ASDU_setSmpMod(asdu, IEC61850_SV_SMPMOD_PER_NOMINAL_PERIOD);
    /* RefrTm отключён (пустой SmvOpts в CID) — не вызываем enableRefrTm() */

    /* Резервирование памяти под данные (порядок строго по FCDA из CID) */
    *out_idx_i1 = SVPublisher_ASDU_addINT32(asdu);
    *out_idx_q1 = SVPublisher_ASDU_addQuality(asdu);
    *out_idx_i2 = SVPublisher_ASDU_addINT32(asdu);
    *out_idx_q2 = SVPublisher_ASDU_addQuality(asdu);
    *out_idx_i3 = SVPublisher_ASDU_addINT32(asdu);
    *out_idx_q3 = SVPublisher_ASDU_addQuality(asdu);

    /* Финализация структуры */
    SVPublisher_setupComplete(pub);

    /* Параметры ПОСЛЕ setupComplete (не меняют размер буфера) */
    SVPublisher_ASDU_setSmpSynch(asdu, IEC61850_SV_SMPSYNC_SYNCED_GLOBAL_CLOCK);
    SVPublisher_ASDU_setSmpCntWrap(asdu, SAMPLES_PER_SEC);

    *out_asdu = asdu;

    LOG_INFO("ASDU configured: smvID=SV_FAULT_INJECTORLD1/LLN0.S1, confRev=1001, smpRate=%u\n",
             SAMPLES_PER_PERIOD);
    LOG_INFO("SmpCnt wrap=%u (%u sec) | Interval=~%d mks | Ctrl+C to stop\n",
             SAMPLES_PER_SEC, SAMPLES_PER_SEC / SAMPLES_PER_PERIOD, INTERVAL_NS / 1000);
    fflush(stdout);

    return pub;
}

/* ============================================================
 * Основная функция
 * ============================================================ */
int main(int argc, char** argv) {
    /* --- Парсинг аргументов --- */
    CliConfig_t cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc == 1) return EXIT_SUCCESS;  /* help показан */
    if (rc != 0) return EXIT_FAILURE;

    /* --- Real-time приоритеты --- */
    setup_rt_priority();

    /* --- Lookup table для sin() --- */
    init_sin_lut();

    LOG_INFO("Initializing network parameters...\n");
    fflush(stdout);

    /* --- Создание издателя --- */
    SVPublisher_ASDU asdu = NULL;
    int idx_i1 = -1, idx_q1 = -1;
    int idx_i2 = -1, idx_q2 = -1;
    int idx_i3 = -1, idx_q3 = -1;

    SVPublisher pub = create_publisher(cfg.iface, &asdu,
                                        &idx_i1, &idx_q1, &idx_i2, &idx_q2,
                                        &idx_i3, &idx_q3);
    if (!pub || !asdu) {
        return EXIT_FAILURE;
    }

    /* --- Обработка SIGINT --- */
    signal(SIGINT, sigint_handler);

    /* --- Буфер для REORDER --- */
    ReorderBuf_t rbuf;
    init_reorder_buf(&rbuf);

    /* --- Инициализация абсолютного времени --- */
    struct timespec next_time;
    if (clock_gettime(CLOCK_MONOTONIC, &next_time) != 0) {
        LOG_ERROR("clock_gettime failed: %s\n", strerror(errno));
        SVPublisher_destroy(pub);
        return EXIT_FAILURE;
    }

    uint16_t sample_idx = 0;

    /* ========================================================
     * Цикл публикации (REAL-TIME)
     * ======================================================== */
    LOG_INFO("Entering publish loop (REAL-TIME)...\n");
    fflush(stdout);

    while (running) {
        uint16_t curr = sample_idx;

        /* Значения из lookup table (вместо sin() в цикле) */
        int32_t v1 = get_sin_value(curr);
        int32_t v2 = -v1;
        int32_t v3 = v1 / 2;

        uint64_t now = get_time_ns();
        Quality q = QUALITY_INVALID;  /* Quality тип из iec61850_common.h */
        bool publish_now = true;

        /* 🔽 DROP — пропуск публикации */
        if (cfg.drop_enabled && curr >= cfg.drop_start && curr < cfg.drop_start + cfg.drop_count) {
            publish_now = false;
        }
        /* 🔽 REORDER — захват кадра */
        else if (cfg.reorder_enabled && !rbuf.active && curr == cfg.reorder_delay) {
            rbuf.active       = true;
            rbuf.cnt          = curr;
            rbuf.t            = now;
            rbuf.i1 = v1; rbuf.i2 = v2; rbuf.i3 = v3;
            rbuf.q1 = q; rbuf.q2 = q; rbuf.q3 = q;
            publish_now = false;
        }
        /* 🔽 REORDER — вставка сохранённого кадра */
        else if (cfg.reorder_enabled && rbuf.active && curr == cfg.reorder_insert) {
            SVPublisher_ASDU_setSmpCnt(asdu, rbuf.cnt);
            SVPublisher_ASDU_setINT32(asdu, idx_i1, rbuf.i1);
            SVPublisher_ASDU_setQuality(asdu, idx_q1, rbuf.q1);
            SVPublisher_ASDU_setINT32(asdu, idx_i2, rbuf.i2);
            SVPublisher_ASDU_setQuality(asdu, idx_q2, rbuf.q2);
            SVPublisher_ASDU_setINT32(asdu, idx_i3, rbuf.i3);
            SVPublisher_ASDU_setQuality(asdu, idx_q3, rbuf.q3);
            SVPublisher_publish(pub);
            rbuf.active = false;
        }

        /* 📤 Обычная публикация */
        if (publish_now) {
            SVPublisher_ASDU_setSmpCnt(asdu, curr);
            SVPublisher_ASDU_setINT32(asdu, idx_i1, v1);
            SVPublisher_ASDU_setQuality(asdu, idx_q1, q);
            SVPublisher_ASDU_setINT32(asdu, idx_i2, v2);
            SVPublisher_ASDU_setQuality(asdu, idx_q2, q);
            SVPublisher_ASDU_setINT32(asdu, idx_i3, v3);
            SVPublisher_ASDU_setQuality(asdu, idx_q3, q);
            SVPublisher_publish(pub);
        }

        /* ⏱️ Обновление абсолютного времени — предотвращает дрейф */
        sample_idx = (sample_idx + 1) % SAMPLES_PER_SEC;

        next_time.tv_nsec += INTERVAL_NS;
        while (next_time.tv_nsec >= 1000000000L) {
            next_time.tv_sec++;
            next_time.tv_nsec -= 1000000000L;
        }

        /* Обработка EINTR — повторный вызов при прерывании сигналом */
        int ret;
        do {
            ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);
        } while (ret == EINTR);
    }

    /* ========================================================
     * Очистка
     * ======================================================== */
    LOG_INFO("\nCleaning up...\n");
    fflush(stdout);

    SVPublisher_destroy(pub);
    return EXIT_SUCCESS;
}
