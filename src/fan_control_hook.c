#define _GNU_SOURCE
#include <features.h>
#undef __GLIBC_USE_ISOC2X
#define __GLIBC_USE_ISOC2X 0
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

/* glibc 2.11 compatibility: redirect stat() to __xstat(3, ...) */
extern int __xstat(int ver, const char *path, struct stat *buf);
#undef stat
#define stat(p, b) __xstat(3, (p), (b))
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

/*
 * ==============================================================================
 * 浪潮 SA5212M4 BMC 核心调速与生存监控劫持库 (Fan Control & Health Engine)
 *
 * 完整逆向 InspurFanControlTask 并融合工业级智能温控模型:
 * 1. ASPEED AST2300 硬件真实 PWM 通道映射 (彻底解决风扇通道错位与 60%+ 失控):
 *    - Fan 0 (Zone B, CPU1 尾部直吹) -> AST2300 PWM 5 (Tach 8)
 *    - Fan 2 (Zone B, CPU1 中置主力) -> AST2300 PWM 1 (Tach 2)
 *    - Fan 4 (Zone A, CPU0 中置直吹) -> AST2300 PWM 0 (Tach 0)
 *    - Fan 6 (Zone A, CPU0 尾部直吹) -> AST2300 PWM 4 (Tach 12)
 * 2. Web 后台与 IPMI 手动/自动切换【零延迟实时同步接管】:
 *    - 劫持 InspurSetFanControlMode (Cmd 0x7a) 与 inspur_set_fan_control_mode
 *    - 切换回 Auto (0) 瞬间，立即清空缓存并同步执行全通道调速，Web 刷新即见最新转速！
 *    - 劫持 InspurSetFanPwmDuty (Cmd 0x78) 与 set_pwm_dutycycle，支持手动自由控速。
 * 3. 对称风道温控模型:
 *    - 当两 CPU 温差 <= 4°C 时，Fan 0, Fan 4, Fan 6 完全对称同频运转。
 *    - Fan 2 最低限制 6% (常温怠速 18%)，其他风扇最低限制 1%。
 * 4. 生存与硬件心跳守护:
 *    - 每秒喂狗 /var/pipe/watchdogQ，翻转 GPIO 65 维系 CPLD 心跳。
 *    - 同步 updateSensorValue 维系原厂全局状态表。
 * ==============================================================================
 */

#define MAX_LEVELS 6
#define CONF_PATH_PRIMARY   "/conf/fan_control.conf"
#define CONF_PATH_FALLBACK  "/etc/fan_control.conf"
#define LOG_FILE            "/var/log/fan_control.log"
#define STATUS_FILE         "/var/run/fan_status"
#define HEARTBEAT_FILE      "/root/fan"
#define WATCHDOG_PIPE       "/var/pipe/watchdogQ"
#define TEMP_OVERRIDE_RUN   "/var/run/cpu_temp"
#define TEMP_OVERRIDE_TMP   "/tmp/cpu_temp"
#define MAX_LOG_SIZE        65536

#define REF_DEFAULT_IDLE_UPPER 68
#define MAX_IDLE_UPPER_TEMP    70
#define MIN_IDLE_UPPER_TEMP    40

struct fan_level_config {
    int lower;
    int upper;
    int speed;
};

struct fan_control_config {
    /* 核心基准温区与动态差值 */
    int idle_upper_temp;          /* 常温标准怠速上限: 默认 68°C */
    int cold_ambient_temp;        /* 低温深冷判定阈值: 默认 48°C (随差值平移) */

    int check_interval;           /* 采样周期: 默认 5 秒 */
    int hysteresis_c;             /* 迟滞量: 默认 2°C */

    int smooth_up_step;           /* 升温步长: +4% */
    int quiet_down_step;          /* 降温步长: -2% */

    int smooth_up_low_temp_c;     /* 爬升起始温区 (随差值平移) */
    int smooth_up_high_temp_c;    /* 爬升上限温区 (随差值平移) */
    int quiet_down_temp_c;        /* 静音缓降温区 (随差值平移) */

    /* 风扇通道与转速限制 */
    int fan_main_channel;         /* 主力风扇: Fan 2 */
    int fan_main_idle_speed;      /* 主力风扇常温怠速: 18% */
    int fan_main_cold_idle_speed; /* 主力风扇最低限制: 6% */

    int fan_cpu0_channel;         /* CPU0 直吹风扇: Fan 4 */
    int fan_cpu1_channel;         /* CPU1 直吹风扇: Fan 0 */
    int fan_aux_offset;           /* 辅助风扇相对 Fan 2 的转速差: 默认 15% */
    int fan_standby_floor;        /* 其他风扇最低底速: 1% */

    int fan_min;                  /* 绝对最低限制: 1% */
    int fan_max;                  /* 最高限制: 100% */
    int max_fan_delta;            /* 相邻风道最大风压差限制: 默认 30% */
    uint32_t disabled_fan_mask;   /* 禁用风扇掩码 */

    int sensor_cpu0;
    int sensor_cpu1;
    int sensor_fail_safe_speed;
    int log_level;
    int curve_offsets[4];         /* 阶梯相对偏移 (L1..L4): 默认 3, 6, 12, 18 */
    struct fan_level_config levels[MAX_LEVELS];
};

/* 全局运行时状态 */
static struct fan_control_config g_cfg;
static char g_active_conf_path[256] = {0};

static pthread_mutex_t g_fan_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_fan0_speed = -1;
static int g_fan2_speed = -1;
static int g_fan4_speed = -1;
static int g_fan6_speed = -1;
static int g_level_sys = -1;
static int g_level_cpu0 = -1;
static int g_level_cpu1 = -1;
static int g_sensor_fail_count = 0;
static int g_last_fan_mode = -1;

/* 硬件原厂库句柄与函数指针 (显式精准加载，杜绝 RTLD_DEFAULT 递归死锁) */
static void *g_pwmtach_lib = NULL;
static void *g_ipmipdk_lib = NULL;
static void *g_ipmimsghndlr_lib = NULL;

static int (*raw_set_pwm_dutycycle)(int dev, int pwm_num, int dutycycle) = NULL;
static int (*raw_PDK_BMCHeartPWMCtrl)(int arg) = NULL;
static int (*raw_PDK_GetPSGood)(int arg) = NULL;
static int (*raw_PDK_GetIntruderStatus)(int arg) = NULL;
static int (*raw_PDK_GlowFanLED)(int status, int p1, int p2, int arg) = NULL;
static int (*raw_PDK_GetFanConfig)(int fan_id) = NULL;
static int (*raw_PDK_GetFanStatus)(int fan_id, uint8_t *status, uint16_t *speed, uint8_t *p4) = NULL;
static int (*raw_PDK_GetFanPWMNo)(int fan_id) = NULL;
static void (*raw_updateSensorValue)(int ps_good) = NULL;
static int (*raw_API_GetSensorReading)(unsigned char sensor_num, unsigned char *resp, int size) = NULL;
static uint8_t *g_inspur_fan_state = NULL;

/* 前向声明 */
static void process_fan_control_cycle(int chassis_intrusion);
int inspur_get_fan_control_mode(void);
int inspur_set_fan_control_mode(int mode);

/* -------------------------------------------------------------------------- */
/* 硬件真实拓扑映射函数                                                       */
/* -------------------------------------------------------------------------- */
/*
 * SA5212M4 ASPEED AST2300 硬件 PWM 通道映射 (源自 libipmipdk.so.1.44.0 PDK_GetFanPWMNo):
 * Fan 0 (Zone B, CPU1 尾部)  -> AST2300 PWM 5 (Tach 8)
 * Fan 1 (未安装 / 空插槽)    -> AST2300 PWM 2 (Tach 6)
 * Fan 2 (Zone B, CPU1 中置)  -> AST2300 PWM 1 (Tach 2)
 * Fan 3 (未安装 / 空插槽)    -> AST2300 PWM 3 (Tach 4)
 * Fan 4 (Zone A, CPU0 中置)  -> AST2300 PWM 0 (Tach 0)
 * Fan 5 (未安装 / 空插槽)    -> AST2300 PWM 6 (Tach 10)
 * Fan 6 (Zone A, CPU0 尾部)  -> AST2300 PWM 4 (Tach 12)
 * Fan 7 (未安装 / 空插槽)    -> AST2300 PWM 7 (Tach 14)
 */
static int get_pwm_for_fan(int fan_id) {
    if (raw_PDK_GetFanPWMNo) {
        int pwm = raw_PDK_GetFanPWMNo(fan_id);
        if (pwm >= 0 && pwm < 8) return pwm;
    }
    switch (fan_id) {
        case 0: return 5;
        case 1: return 2;
        case 2: return 1;
        case 3: return 3;
        case 4: return 0;
        case 5: return 6;
        case 6: return 4;
        case 7: return 7;
        default: return -1;
    }
}

static int get_fan_for_pwm(int pwm_num) {
    switch (pwm_num) {
        case 5: return 0;
        case 2: return 1;
        case 1: return 2;
        case 3: return 3;
        case 0: return 4;
        case 6: return 5;
        case 4: return 6;
        case 7: return 7;
        default: return -1;
    }
}

/* -------------------------------------------------------------------------- */
/* 辅助函数: 安全风速截断 (绝对防下溢与越界)                                  */
/* -------------------------------------------------------------------------- */
static int clamp_fan_speed(int val, int min_val, int max_val) {
    if (min_val < 1) min_val = 1;
    if (max_val > 100) max_val = 100;
    if (min_val > max_val) min_val = max_val;

    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static char *trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* -------------------------------------------------------------------------- */
/* 辅助函数: 日志与时间输出                                                   */
/* -------------------------------------------------------------------------- */
static void log_write(int level, const char *fmt, ...) {
    if (level > g_cfg.log_level) return;

    char time_buf[64];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > MAX_LOG_SIZE) {
        int tr = truncate(LOG_FILE, 0); (void)tr;
    }

    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        fprintf(fp, "[%s] %s\n", time_buf, msg);
        fclose(fp);
    }

    fprintf(stderr, "[FANHOOK %s] %s\n", time_buf, msg);
}

/* -------------------------------------------------------------------------- */
/* 硬件句柄显式初始化 (严格指向目标 .so，不经过全局搜索)                       */
/* -------------------------------------------------------------------------- */
static void init_hardware_handles(void) {
    static int initialized = 0;
    if (initialized) return;

    /* 1. 驱动库 libpwmtach.so: 获取底层真实 set_pwm_dutycycle */
    if (!g_pwmtach_lib) {
        g_pwmtach_lib = dlopen("/usr/local/lib/libpwmtach.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!g_pwmtach_lib) g_pwmtach_lib = dlopen("/usr/local/lib/libpwmtach.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_pwmtach_lib) g_pwmtach_lib = dlopen("libpwmtach.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (g_pwmtach_lib && !raw_set_pwm_dutycycle) {
        raw_set_pwm_dutycycle = (int (*)(int, int, int))dlsym(g_pwmtach_lib, "set_pwm_dutycycle");
    }

    /* 2. 原厂 PDK 库 libipmipdk.so: 获取 CPLD 心跳、PSGood、开箱检测、指示灯、PWM映射 */
    if (!g_ipmipdk_lib) {
        g_ipmipdk_lib = dlopen("/usr/local/lib/libipmipdk.so.1.44.0", RTLD_NOW | RTLD_GLOBAL);
        if (!g_ipmipdk_lib) g_ipmipdk_lib = dlopen("/usr/local/lib/libipmipdk.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!g_ipmipdk_lib) g_ipmipdk_lib = dlopen("libipmipdk.so.1", RTLD_NOW | RTLD_GLOBAL);
    }
    if (g_ipmipdk_lib) {
        if (!raw_PDK_BMCHeartPWMCtrl)
            raw_PDK_BMCHeartPWMCtrl = (int (*)(int))dlsym(g_ipmipdk_lib, "PDK_BMCHeartPWMCtrl");
        if (!raw_PDK_GetPSGood)
            raw_PDK_GetPSGood = (int (*)(int))dlsym(g_ipmipdk_lib, "PDK_GetPSGood");
        if (!raw_PDK_GetIntruderStatus)
            raw_PDK_GetIntruderStatus = (int (*)(int))dlsym(g_ipmipdk_lib, "PDK_GetIntruderStatus");
        if (!raw_PDK_GlowFanLED)
            raw_PDK_GlowFanLED = (int (*)(int, int, int, int))dlsym(g_ipmipdk_lib, "PDK_GlowFanLED");
        if (!raw_PDK_GetFanConfig)
            raw_PDK_GetFanConfig = (int (*)(int))dlsym(g_ipmipdk_lib, "PDK_GetFanConfig");
        if (!raw_PDK_GetFanStatus)
            raw_PDK_GetFanStatus = (int (*)(int, uint8_t *, uint16_t *, uint8_t *))dlsym(g_ipmipdk_lib, "PDK_GetFanStatus");
        if (!raw_PDK_GetFanPWMNo)
            raw_PDK_GetFanPWMNo = (int (*)(int))dlsym(g_ipmipdk_lib, "PDK_GetFanPWMNo");
    }

    /* 3. 消息处理库 libipmimsghndlr.so: 获取 updateSensorValue 与全局状态表 */
    if (!g_ipmimsghndlr_lib) {
        g_ipmimsghndlr_lib = dlopen("/usr/local/lib/libipmimsghndlr.so.2.368.0", RTLD_NOW | RTLD_GLOBAL);
        if (!g_ipmimsghndlr_lib) g_ipmimsghndlr_lib = dlopen("/usr/local/lib/libipmimsghndlr.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!g_ipmimsghndlr_lib) g_ipmimsghndlr_lib = dlopen("libipmimsghndlr.so.2", RTLD_NOW | RTLD_GLOBAL);
    }
    if (g_ipmimsghndlr_lib) {
        if (!raw_updateSensorValue)
            raw_updateSensorValue = (void (*)(int))dlsym(g_ipmimsghndlr_lib, "updateSensorValue");

        /* 通过 updateSensorValue 精准定位 libipmimsghndlr.so 的加载基地址 */
        Dl_info info;
        if (raw_updateSensorValue && dladdr((void *)raw_updateSensorValue, &info) && info.dli_fbase) {
            /* 原厂全局状态表在 libipmimsghndlr.so.2.368.0 中的固定基准偏移为 0x92a28 */
            g_inspur_fan_state = (uint8_t *)info.dli_fbase + 0x92a28;
        }
    }

    /* 4. PDK API 备用测温 */
    void *api_h = dlopen("libipmipdkapi.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!api_h) api_h = dlopen("/usr/local/lib/libipmipdkapi.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (api_h) {
        raw_API_GetSensorReading = dlsym(api_h, "API_GetSensorReading");
    }

    initialized = 1;
}

/* -------------------------------------------------------------------------- */
/* 配置解析与动态差值重算                                                     */
/* -------------------------------------------------------------------------- */
static void set_default_config(struct fan_control_config *cfg) {
    cfg->idle_upper_temp = REF_DEFAULT_IDLE_UPPER;  /* 默认 68°C */
    cfg->cold_ambient_temp = 48;                     /* 默认 48°C */

    cfg->check_interval = 30;
    cfg->hysteresis_c = 2;

    cfg->smooth_up_step = 2;                         /* 升温 +2% (匹配 setfanlevel.sh) */
    cfg->quiet_down_step = 1;                        /* 降温 -1% (匹配 setfanlevel.sh) */

    cfg->fan_main_channel = 2;                       /* Fan 2: 中置主力 */
    cfg->fan_main_idle_speed = 16;                   /* Fan 2 常温 16% */
    cfg->fan_main_cold_idle_speed = 6;               /* Fan 2 最低 6% */

    cfg->fan_cpu0_channel = 4;                       /* Fan 4: 直吹 CPU0 */
    cfg->fan_cpu1_channel = 0;                       /* Fan 0: 直吹 CPU1 */
    cfg->fan_standby_floor = 1;                      /* 定向/辅助风扇最低 1% */
    cfg->fan_aux_offset = 15;                        /* 辅助风扇偏移 -15% (匹配 setfanlevel.sh) */

    cfg->fan_min = 1;                                /* 绝对安全最低限制: 1% */
    cfg->fan_max = 100;
    cfg->max_fan_delta = 30;
    cfg->disabled_fan_mask = 0;

    cfg->curve_offsets[0] = 3;
    cfg->curve_offsets[1] = 6;
    cfg->curve_offsets[2] = 12;
    cfg->curve_offsets[3] = 18;

    cfg->levels[0].speed = 16;
    cfg->levels[1].speed = 20;
    cfg->levels[2].speed = 22;
    cfg->levels[3].speed = 28;
    cfg->levels[4].speed = 40;
    cfg->levels[5].speed = 70;

    cfg->sensor_cpu0 = 0x19;
    cfg->sensor_cpu1 = 0x1a;
    cfg->sensor_fail_safe_speed = 50;
    cfg->log_level = 1;
}

static void recalculate_dynamic_curve(struct fan_control_config *cfg) {
    if (cfg->idle_upper_temp > MAX_IDLE_UPPER_TEMP) {
        cfg->idle_upper_temp = MAX_IDLE_UPPER_TEMP;
    } else if (cfg->idle_upper_temp < MIN_IDLE_UPPER_TEMP) {
        cfg->idle_upper_temp = MIN_IDLE_UPPER_TEMP;
    }

    int base = cfg->idle_upper_temp;

    cfg->smooth_up_low_temp_c = base;
    cfg->smooth_up_high_temp_c = base + 4;
    cfg->quiet_down_temp_c = base - 2;

    if (cfg->cold_ambient_temp <= 0 || cfg->cold_ambient_temp >= base) {
        cfg->cold_ambient_temp = base - 20;
    }

    cfg->levels[0].lower = 0;
    cfg->levels[0].upper = base;

    cfg->levels[1].lower = base;
    cfg->levels[1].upper = base + cfg->curve_offsets[0];

    cfg->levels[2].lower = base + cfg->curve_offsets[0];
    cfg->levels[2].upper = base + cfg->curve_offsets[1];

    cfg->levels[3].lower = base + cfg->curve_offsets[1];
    cfg->levels[3].upper = base + cfg->curve_offsets[2];

    cfg->levels[4].lower = base + cfg->curve_offsets[2];
    cfg->levels[4].upper = base + cfg->curve_offsets[3];

    cfg->levels[5].lower = base + cfg->curve_offsets[3];
    cfg->levels[5].upper = 125;
}

static int parse_int_array(const char *str, int *out, int max_count) {
    int count = 0;
    const char *p = str;
    while (*p && count < max_count) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char *endp;
        long val = strtol(p, &endp, 0);
        if (endp == p) break;
        out[count++] = (int)val;
        p = endp;
    }
    return count;
}

static void parse_thermal_curve_matrix(const char *val, struct fan_control_config *cfg) {
    char buf[256];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = buf;
    int idx = 0;
    while (*p && idx < MAX_LEVELS) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *elem_end = p;
        while (*elem_end && *elem_end != ',' && *elem_end != ' ' && *elem_end != '\t') elem_end++;
        char save = *elem_end;
        *elem_end = '\0';

        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            char *off_str = trim_whitespace(p);
            char *spd_str = trim_whitespace(colon + 1);
            if (idx > 0 && idx < 5) {
                if (*off_str != '+' && *off_str != '\0') {
                    cfg->curve_offsets[idx - 1] = clamp_fan_speed((int)strtol(off_str, NULL, 0), 1, 50);
                }
            }
            cfg->levels[idx].speed = clamp_fan_speed((int)strtol(spd_str, NULL, 0), 1, 100);
        } else {
            cfg->levels[idx].speed = clamp_fan_speed((int)strtol(p, NULL, 0), 1, 100);
        }
        idx++;
        if (save == '\0') break;
        p = elem_end + 1;
    }
}

static int parse_config_file(const char *path, struct fan_control_config *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_whitespace(line);
        if (*p == '#' || *p == ';' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(p);
        char *val = trim_whitespace(eq + 1);

        if (strcmp(key, "idle_upper_temp") == 0) {
            cfg->idle_upper_temp = clamp_fan_speed((int)strtol(val, NULL, 0), MIN_IDLE_UPPER_TEMP, MAX_IDLE_UPPER_TEMP);
        } else if (strcmp(key, "cold_ambient_temp") == 0) {
            cfg->cold_ambient_temp = clamp_fan_speed((int)strtol(val, NULL, 0), 10, 60);
        } else if (strcmp(key, "check_interval") == 0) {
            cfg->check_interval = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 60);
        } else if (strcmp(key, "hysteresis_c") == 0) {
            cfg->hysteresis_c = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 10);
        } else if (strcmp(key, "ramp_steps") == 0) {
            int steps[2];
            if (parse_int_array(val, steps, 2) >= 2) {
                cfg->smooth_up_step = clamp_fan_speed(steps[0], 1, 20);
                cfg->quiet_down_step = clamp_fan_speed(steps[1], 1, 10);
            }
        } else if (strcmp(key, "smooth_up_step") == 0) {
            cfg->smooth_up_step = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 20);
        } else if (strcmp(key, "quiet_down_step") == 0) {
            cfg->quiet_down_step = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 10);
        } else if (strcmp(key, "fan_channels") == 0) {
            int chs[3];
            if (parse_int_array(val, chs, 3) >= 3) {
                cfg->fan_main_channel = chs[0];
                cfg->fan_cpu0_channel = chs[1];
                cfg->fan_cpu1_channel = chs[2];
            }
        } else if (strcmp(key, "fan_main_channel") == 0) {
            cfg->fan_main_channel = (int)strtol(val, NULL, 0);
        } else if (strcmp(key, "fan_main_speeds") == 0) {
            int spds[2];
            if (parse_int_array(val, spds, 2) >= 2) {
                cfg->fan_main_idle_speed = clamp_fan_speed(spds[0], 1, 100);
                cfg->fan_main_cold_idle_speed = clamp_fan_speed(spds[1], 1, 100);
            }
        } else if (strcmp(key, "fan_main_idle_speed") == 0) {
            cfg->fan_main_idle_speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "fan_main_cold_idle_speed") == 0) {
            cfg->fan_main_cold_idle_speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "fan_cpu0_channel") == 0) {
            cfg->fan_cpu0_channel = (int)strtol(val, NULL, 0);
        } else if (strcmp(key, "fan_cpu1_channel") == 0) {
            cfg->fan_cpu1_channel = (int)strtol(val, NULL, 0);
        } else if (strcmp(key, "fan_aux_offset") == 0) {
            cfg->fan_aux_offset = clamp_fan_speed((int)strtol(val, NULL, 0), 0, 50);
        } else if (strcmp(key, "fan_standby_floor") == 0) {
            cfg->fan_standby_floor = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "fan_limits") == 0) {
            int lims[2];
            if (parse_int_array(val, lims, 2) >= 2) {
                cfg->fan_min = clamp_fan_speed(lims[0], 1, 100);
                cfg->fan_max = clamp_fan_speed(lims[1], 1, 100);
            }
        } else if (strcmp(key, "fan_min") == 0) {
            cfg->fan_min = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "fan_max") == 0) {
            cfg->fan_max = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "max_fan_delta") == 0) {
            cfg->max_fan_delta = clamp_fan_speed((int)strtol(val, NULL, 0), 5, 50);
        } else if (strcmp(key, "disabled_fan_mask") == 0) {
            cfg->disabled_fan_mask = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "cpu_sensors") == 0) {
            int s[2];
            if (parse_int_array(val, s, 2) >= 2) {
                cfg->sensor_cpu0 = s[0];
                cfg->sensor_cpu1 = s[1];
            }
        } else if (strcmp(key, "sensor_cpu0") == 0) {
            cfg->sensor_cpu0 = (int)strtol(val, NULL, 0);
        } else if (strcmp(key, "sensor_cpu1") == 0) {
            cfg->sensor_cpu1 = (int)strtol(val, NULL, 0);
        } else if (strcmp(key, "sensor_fail_safe_speed") == 0) {
            cfg->sensor_fail_safe_speed = clamp_fan_speed((int)strtol(val, NULL, 0), 20, 100);
        } else if (strcmp(key, "log_level") == 0) {
            cfg->log_level = clamp_fan_speed((int)strtol(val, NULL, 0), 0, 2);
        } else if (strcmp(key, "thermal_curve") == 0 || strcmp(key, "curve_matrix") == 0) {
            parse_thermal_curve_matrix(val, cfg);
        } else if (strcmp(key, "curve_speeds") == 0) {
            int spds[6];
            int n = parse_int_array(val, spds, 6);
            for (int i = 0; i < n; i++) {
                cfg->levels[i].speed = clamp_fan_speed(spds[i], 1, 100);
            }
        } else if (strcmp(key, "curve_offsets") == 0) {
            int offs[4];
            int n = parse_int_array(val, offs, 4);
            for (int i = 0; i < n; i++) {
                cfg->curve_offsets[i] = clamp_fan_speed(offs[i], 1, 50);
            }
        } else if (strcmp(key, "level0_speed") == 0) {
            cfg->levels[0].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level1_speed") == 0) {
            cfg->levels[1].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level2_speed") == 0) {
            cfg->levels[2].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level3_speed") == 0) {
            cfg->levels[3].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level4_speed") == 0) {
            cfg->levels[4].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level5_speed") == 0) {
            cfg->levels[5].speed = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 100);
        } else if (strcmp(key, "level1_offset") == 0) {
            cfg->curve_offsets[0] = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 50);
        } else if (strcmp(key, "level2_offset") == 0) {
            cfg->curve_offsets[1] = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 50);
        } else if (strcmp(key, "level3_offset") == 0) {
            cfg->curve_offsets[2] = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 50);
        } else if (strcmp(key, "level4_offset") == 0) {
            cfg->curve_offsets[3] = clamp_fan_speed((int)strtol(val, NULL, 0), 1, 50);
        }
    }

    fclose(fp);
    recalculate_dynamic_curve(cfg);
    return 0;
}

static void reload_config_from_disk(void) {
    const char *target_path = NULL;
    struct stat st;

    if (stat(CONF_PATH_PRIMARY, &st) == 0) {
        target_path = CONF_PATH_PRIMARY;
    } else if (stat(CONF_PATH_FALLBACK, &st) == 0) {
        target_path = CONF_PATH_FALLBACK;
    }

    if (target_path) {
        struct fan_control_config new_cfg;
        set_default_config(&new_cfg);
        if (parse_config_file(target_path, &new_cfg) == 0) {
            memcpy(&g_cfg, &new_cfg, sizeof(struct fan_control_config));
            strncpy(g_active_conf_path, target_path, sizeof(g_active_conf_path) - 1);
            log_write(1, "Loaded config %s: idle_upper=%dC, ramp=+%d%%/-%d%%, main=[%d%%,%d%%], aux_offset=%d%%, levels=[%d,%d,%d,%d,%d,%d]",
                      target_path, g_cfg.idle_upper_temp, g_cfg.smooth_up_step, g_cfg.quiet_down_step,
                      g_cfg.fan_main_idle_speed, g_cfg.fan_main_cold_idle_speed, g_cfg.fan_aux_offset,
                      g_cfg.levels[0].speed, g_cfg.levels[1].speed, g_cfg.levels[2].speed,
                      g_cfg.levels[3].speed, g_cfg.levels[4].speed, g_cfg.levels[5].speed);
        }
    } else {
        set_default_config(&g_cfg);
        recalculate_dynamic_curve(&g_cfg);
        log_write(1, "No config file found. Loaded internal safe defaults.");
    }
}

/* -------------------------------------------------------------------------- */
/* 看门狗喂狗机制 (/var/pipe/watchdogQ)                                        */
/* -------------------------------------------------------------------------- */
static void feed_watchdog(void) {
    int fd = open(WATCHDOG_PIPE, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        uint32_t msg[2] = {1, 1};
        ssize_t wr = write(fd, msg, sizeof(msg)); (void)wr;
        close(fd);
    }
}

/* -------------------------------------------------------------------------- */
/* 硬件真实通道风扇转速应用与原厂状态表同步                                   */
/* -------------------------------------------------------------------------- */
static int apply_fan_duty(int fan_id, int duty) {
    init_hardware_handles();

    int min_floor = (fan_id == 2) ? g_cfg.fan_main_cold_idle_speed : g_cfg.fan_standby_floor;
    duty = clamp_fan_speed(duty, min_floor, g_cfg.fan_max);

    int pwm_num = get_pwm_for_fan(fan_id);
    if (pwm_num < 0 || pwm_num >= 8) return -1;

    if (g_cfg.disabled_fan_mask & (1 << fan_id)) {
        duty = 0;
    }

    /*
     * 双向索引同步至原厂全局状态表供 WebUI 与 IPMI 查询:
     * - offset 56 + 2 * fan_id: WebUI/IPMI 通过 fan_id (0, 2, 4, 6) 读取
     * - offset 56 + 2 * pwm_num: 驱动或低级任务通过硬件 pwm_num 读取
     * - offset 72 + fan_id: 记录 fan_id 对应的 pwm_num
     */
    if (g_inspur_fan_state) {
        *(uint16_t *)(g_inspur_fan_state + 56 + 2 * fan_id) = (uint16_t)duty;
        *(uint16_t *)(g_inspur_fan_state + 56 + 2 * pwm_num) = (uint16_t)duty;
        g_inspur_fan_state[72 + fan_id] = (uint8_t)pwm_num;
    }

    /* 直接写入 ASPEED AST2300 真实硬件 PWM 控制器 */
    if (raw_set_pwm_dutycycle) {
        return raw_set_pwm_dutycycle(0, pwm_num, duty);
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* 温度传感器读取 (优先读取由 updateSensorValue 维护的原厂全局状态表)          */
/* -------------------------------------------------------------------------- */
static int read_temp_from_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[64];
    int t = -1;
    if (fgets(line, sizeof(line), fp)) {
        t = (int)strtol(line, NULL, 10);
    }
    fclose(fp);
    if (t >= 0 && t <= 125) return t;
    return -1;
}

static int read_single_sensor(int sensor_num) {
    if (raw_API_GetSensorReading) {
        unsigned char resp[16] = {0};
        int ret = raw_API_GetSensorReading((unsigned char)sensor_num, resp, 1);
        if (ret >= 0 && resp[0] == 0) {
            return (int)resp[1];
        }
    }
    return -1;
}

static int get_all_temperatures(int *cpu0_out, int *cpu1_out, int *sys_max_out) {
    int override_val = read_temp_from_file(TEMP_OVERRIDE_RUN);
    if (override_val < 0) override_val = read_temp_from_file(TEMP_OVERRIDE_TMP);
    if (override_val >= 0) {
        if (cpu0_out) *cpu0_out = override_val;
        if (cpu1_out) *cpu1_out = override_val;
        if (sys_max_out) *sys_max_out = override_val;
        return override_val;
    }

    int t0 = -1, t1 = -1;

    /* 1. 优先读取原厂 updateSensorValue() 更新到全局表中的准确温度 */
    if (g_inspur_fan_state) {
        uint16_t s0 = *(uint16_t *)(g_inspur_fan_state + 100);
        uint16_t s1 = *(uint16_t *)(g_inspur_fan_state + 102);
        if (s0 > 0 && s0 <= 125) t0 = (int)s0;
        if (s1 > 0 && s1 <= 125) t1 = (int)s1;
    }

    /* 2. 备用读取: 调用 API_GetSensorReading */
    if (t0 <= 0) t0 = read_single_sensor(g_cfg.sensor_cpu0);
    if (t1 <= 0) t1 = read_single_sensor(g_cfg.sensor_cpu1);

    if (cpu0_out) *cpu0_out = t0;
    if (cpu1_out) *cpu1_out = t1;

    int max_t = -1;
    if (t0 > 0 && t1 > 0) {
        max_t = (t0 > t1) ? t0 : t1;
    } else if (t0 > 0) {
        max_t = t0;
    } else if (t1 > 0) {
        max_t = t1;
    }

    if (sys_max_out) *sys_max_out = max_t;
    return max_t;
}

/* -------------------------------------------------------------------------- */
/* 调速曲线与状态机计算 (迟滞防震荡)                                         */
/* -------------------------------------------------------------------------- */
static int get_initial_level(int temp) {
    for (int i = 0; i < MAX_LEVELS - 1; i++) {
        if (temp < g_cfg.levels[i].upper) {
            return i;
        }
    }
    return MAX_LEVELS - 1;
}

static int update_level_hysteresis(int cur_level, int temp) {
    if (cur_level < 0) {
        return get_initial_level(temp);
    }

    while (cur_level < MAX_LEVELS - 1) {
        if (temp >= g_cfg.levels[cur_level].upper) {
            cur_level++;
        } else {
            break;
        }
    }

    while (cur_level > 0) {
        int threshold = g_cfg.levels[cur_level].lower - g_cfg.hysteresis_c;
        if (temp < threshold) {
            cur_level--;
        } else {
            break;
        }
    }

    return cur_level;
}

static int calculate_fan2_speed(int temp_sys, int curve_speed) {
    int target = -1;

    /* 首次初始化或模式切换瞬间 (g_fan2_speed <= 0): 立即定位至目标转速，杜绝步长迟滞 */
    if (g_fan2_speed <= 0) {
        if (temp_sys < g_cfg.cold_ambient_temp) {
            target = g_cfg.fan_main_cold_idle_speed;
        } else if (temp_sys < g_cfg.quiet_down_temp_c) {
            target = g_cfg.fan_main_idle_speed;
        } else {
            target = curve_speed;
        }
        return clamp_fan_speed(target, g_cfg.fan_main_cold_idle_speed, g_cfg.fan_max);
    }

    if (temp_sys < g_cfg.cold_ambient_temp) {
        target = g_fan2_speed - g_cfg.quiet_down_step;
        if (target < g_cfg.fan_main_cold_idle_speed) {
            target = g_cfg.fan_main_cold_idle_speed;
        }
    } else if (temp_sys < g_cfg.quiet_down_temp_c) {
        if (g_fan2_speed < g_cfg.fan_main_idle_speed) {
            target = g_fan2_speed + g_cfg.smooth_up_step;
            if (target > g_cfg.fan_main_idle_speed) target = g_cfg.fan_main_idle_speed;
        } else {
            target = g_fan2_speed - g_cfg.quiet_down_step;
            if (target < g_cfg.fan_main_idle_speed) target = g_cfg.fan_main_idle_speed;
        }
    } else if (temp_sys >= g_cfg.smooth_up_low_temp_c && temp_sys <= g_cfg.smooth_up_high_temp_c) {
        if (g_fan2_speed < curve_speed) {
            target = g_fan2_speed + g_cfg.smooth_up_step;
            if (target > curve_speed) target = curve_speed;
        } else if (g_fan2_speed > curve_speed) {
            target = g_fan2_speed - g_cfg.quiet_down_step;
            if (target < curve_speed) target = curve_speed;
        } else {
            target = curve_speed;
        }
    } else if (temp_sys > g_cfg.smooth_up_high_temp_c) {
        target = curve_speed;
    } else {
        target = g_fan2_speed;
    }

    return clamp_fan_speed(target, g_cfg.fan_main_cold_idle_speed, g_cfg.fan_max);
}

/* -------------------------------------------------------------------------- */
/* 全风道协同调速与执行 (支持 Fan 0, Fan 2, Fan 4, Fan 6 全拓扑接管)          */
/* -------------------------------------------------------------------------- */
static void process_fan_control_cycle(int chassis_intrusion) {
    pthread_mutex_lock(&g_fan_mutex);

    /* 若机箱开盖入侵，强制 100% 满速排热与告警 */
    if (chassis_intrusion) {
        log_write(0, "Chassis intrusion detected! Ramping all fans to 100%%.");
        apply_fan_duty(0, 100);
        apply_fan_duty(2, 100);
        apply_fan_duty(4, 100);
        apply_fan_duty(6, 100);
        g_fan0_speed = 100;
        g_fan2_speed = 100;
        g_fan4_speed = 100;
        g_fan6_speed = 100;
        pthread_mutex_unlock(&g_fan_mutex);
        return;
    }

    int cpu0 = -1, cpu1 = -1, sys_temp = -1;
    get_all_temperatures(&cpu0, &cpu1, &sys_temp);

    if (sys_temp <= 0) {
        g_sensor_fail_count++;
        log_write(0, "Failed to read CPU temperatures (attempt %d)", g_sensor_fail_count);

        if (g_sensor_fail_count >= 3) {
            log_write(0, "Sensor reading failed repeatedly! Applying failsafe speed %d%%",
                      g_cfg.sensor_fail_safe_speed);
            apply_fan_duty(0, g_cfg.sensor_fail_safe_speed);
            apply_fan_duty(2, g_cfg.sensor_fail_safe_speed);
            apply_fan_duty(4, g_cfg.sensor_fail_safe_speed);
            apply_fan_duty(6, g_cfg.sensor_fail_safe_speed);
            g_fan0_speed = g_cfg.sensor_fail_safe_speed;
            g_fan2_speed = g_cfg.sensor_fail_safe_speed;
            g_fan4_speed = g_cfg.sensor_fail_safe_speed;
            g_fan6_speed = g_cfg.sensor_fail_safe_speed;
        }
        pthread_mutex_unlock(&g_fan_mutex);
        return;
    }

    g_sensor_fail_count = 0;

    g_level_sys = update_level_hysteresis(g_level_sys, sys_temp);
    if (cpu0 > 0) g_level_cpu0 = update_level_hysteresis(g_level_cpu0, cpu0);
    if (cpu1 > 0) g_level_cpu1 = update_level_hysteresis(g_level_cpu1, cpu1);

    int curve_sys = g_cfg.levels[g_level_sys].speed;
    int curve_cpu0 = (cpu0 > 0) ? g_cfg.levels[g_level_cpu0].speed : g_cfg.fan_standby_floor;
    int curve_cpu1 = (cpu1 > 0) ? g_cfg.levels[g_level_cpu1].speed : g_cfg.fan_standby_floor;

    /*
     * 核心风扇控速逻辑 (严格对齐 setfanlevel.sh 原厂实现):
     * 1. Fan 2 (中置主力): 负责双路 CPU 整体温控，根据 sys_temp 与迟滞状态机 calculate_fan2_speed 计算。
     * 2. Fan 6 (辅助出风): 严格恒定 1% 静音底速 (遵从 setfanlevel.sh，不随温度提速)。
     * 3. Fan 0 (CPU1) 与 Fan 4 (CPU0) (辅助直吹):
     *    两 CPU 温度相近时 (|CPU0 - CPU1| <= 4°C)，两风扇完全对称同步，严格低于 Fan 2 整整 fan_aux_offset (15%)！
     *    当单侧偏载温差大时 (|CPU0 - CPU1| > 4°C)，较热侧风扇维持 base_aux，较冷侧可按自身温度进一步静音。
     */
    int target_fan2 = calculate_fan2_speed(sys_temp, curve_sys);
    target_fan2 = clamp_fan_speed(target_fan2, g_cfg.fan_main_cold_idle_speed, g_cfg.fan_max);

    /* Fan 6 始终锁定 1% (硬件静音底速，不参与主散热加速) */
    int target_fan6 = 1;

    /* 辅助风扇基准转速: 严格低于 Fan 2 达 fan_aux_offset (默认 15%)，最低受限于 fan_standby_floor (1%) */
    int base_aux = target_fan2 - g_cfg.fan_aux_offset;
    if (base_aux < g_cfg.fan_standby_floor) {
        base_aux = g_cfg.fan_standby_floor;
    }
    int target_fan0 = base_aux;
    int target_fan4 = base_aux;

    if (cpu0 > 0 && cpu1 > 0 && abs(cpu0 - cpu1) > 4) {
        /* 当两 CPU 温差显著时，较热侧维持 base_aux，较冷侧风扇根据自身温度进一步静音降噪 */
        if (cpu0 > cpu1) {
            int aux_cpu1 = calculate_fan2_speed(cpu1, curve_cpu1) - g_cfg.fan_aux_offset;
            target_fan0 = clamp_fan_speed(aux_cpu1, g_cfg.fan_standby_floor, base_aux);
            target_fan4 = base_aux;
        } else {
            int aux_cpu0 = calculate_fan2_speed(cpu0, curve_cpu0) - g_cfg.fan_aux_offset;
            target_fan4 = clamp_fan_speed(aux_cpu0, g_cfg.fan_standby_floor, base_aux);
            target_fan0 = base_aux;
        }
    }

    target_fan2 = clamp_fan_speed(target_fan2, g_cfg.fan_main_cold_idle_speed, g_cfg.fan_max);
    target_fan0 = clamp_fan_speed(target_fan0, g_cfg.fan_standby_floor, g_cfg.fan_max);
    target_fan4 = clamp_fan_speed(target_fan4, g_cfg.fan_standby_floor, g_cfg.fan_max);
    target_fan6 = 1;

    if (target_fan0 != g_fan0_speed || target_fan2 != g_fan2_speed ||
        target_fan4 != g_fan4_speed || target_fan6 != g_fan6_speed) {

        log_write(1, "Fan speed adjust: Fan0(CPU1)=%d%%, Fan2(Main)=%d%%, Fan4(CPU0)=%d%%, Fan6(Lock)=%d%% (CPU0=%dC, CPU1=%dC, aux_offset=%d%%)",
                  target_fan0, target_fan2, target_fan4, target_fan6,
                  cpu0, cpu1, g_cfg.fan_aux_offset);

        apply_fan_duty(0, target_fan0);
        apply_fan_duty(2, target_fan2);
        apply_fan_duty(4, target_fan4);
        apply_fan_duty(6, target_fan6);

        g_fan0_speed = target_fan0;
        g_fan2_speed = target_fan2;
        g_fan4_speed = target_fan4;
        g_fan6_speed = target_fan6;
    }

    for (int ch = 0; ch < 8; ch++) {
        if (g_cfg.disabled_fan_mask & (1 << ch)) {
            apply_fan_duty(ch, 0);
        }
    }

    FILE *sfp = fopen(STATUS_FILE, "w");
    if (sfp) {
        fprintf(sfp, "TEMP_CPU0=%d\nTEMP_CPU1=%d\nTEMP_SYS=%d\nIDLE_UPPER=%d\nLEVEL_SYS=%d\n"
                     "FAN0_CPU1=%d\nFAN2_MAIN=%d\nFAN4_CPU0=%d\nFAN6_AUX=%d\nAUX_OFFSET=%d\n",
                cpu0, cpu1, sys_temp, g_cfg.idle_upper_temp, g_level_sys,
                target_fan0, target_fan2, target_fan4, target_fan6, g_cfg.fan_aux_offset);
        fclose(sfp);
    }

    int hfd = open(HEARTBEAT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (hfd >= 0) close(hfd);

    pthread_mutex_unlock(&g_fan_mutex);
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 1: InspurFanControlTask (全面接管并维系所有原厂硬件监控闭环)       */
/* -------------------------------------------------------------------------- */
void *InspurFanControlTask(void *arg) {
    int inst = 0;
    if (arg) {
        inst = *(int *)arg;
    }

    log_write(0, "=======================================================");
    log_write(0, "InspurFanControlTask hooked! Enterprise Fan Engine Active");
    log_write(0, "Physical Map: Fan0->PWM5, Fan2->PWM1, Fan4->PWM0, Fan6->PWM4");
    log_write(0, "Limits: Fan2 Min=6%%, Other Fans Min=1%%");
    log_write(0, "CPLD GPIO 65 Heartbeat + Watchdog Feed + Sensor State Sync");
    log_write(0, "=======================================================");

    init_hardware_handles();
    reload_config_from_disk();

    int warmup_counter = 0;
    int tick_count = 0;

    while (1) {
        /* 1. 每秒喂用户态看门狗 (/var/pipe/watchdogQ) */
        feed_watchdog();

        /* 2. 每秒翻转 CPLD BMC 硬件心跳引脚 GPIO 65 (杜绝 CPLD 硬件超时强行重启 BMC) */
        if (raw_PDK_BMCHeartPWMCtrl) {
            raw_PDK_BMCHeartPWMCtrl(inst);
        }

        /* 3. 检查电源状态并调用原厂传感器同步函数 (更新 g_inspur_fan_state 全局表) */
        int ps_good = 1;
        if (raw_PDK_GetPSGood) {
            ps_good = raw_PDK_GetPSGood(inst) & 0xFF;
        }

        if (raw_updateSensorValue) {
            raw_updateSensorValue(ps_good);
        }

        /* 4. 更新前面板风扇指示灯状态 */
        if (raw_PDK_GlowFanLED) {
            for (int i = 0; i < 8; i++) {
                if (g_cfg.disabled_fan_mask & (1 << i)) {
                    raw_PDK_GlowFanLED(-1, 0, 0, inst);
                } else {
                    raw_PDK_GlowFanLED(0, 0, 0, inst);
                }
            }
        }

        /* 5. 原厂预热机制 (前 5 秒不执行 PWM 调节，确保传感器与硬件稳定) */
        if (warmup_counter < 5) {
            warmup_counter++;
            sleep(1);
            continue;
        }

        /* 待机状态 (S5 关机) 重置预热计数器并休眠 */
        if (ps_good == 0) {
            warmup_counter = 0;
            sleep(1);
            continue;
        }

        /* 6. 机箱开盖检测 */
        int intrusion = 0;
        if (raw_PDK_GetIntruderStatus) {
            intrusion = raw_PDK_GetIntruderStatus(inst);
        }

        /* 7. 风扇控制模式检测 (0: 自动模式, 1: 手动模式) */
        int fan_mode = inspur_get_fan_control_mode();
        if (fan_mode != 0) {
            /* 处于手动控制模式: 记录上一状态，挂起自动调速，允许用户完全自由手动控速 */
            if (g_last_fan_mode == 0) {
                log_write(1, "Fan control switched to MANUAL mode (%d). Pausing auto algorithm.", fan_mode);
                g_last_fan_mode = 1;
            }
            sleep(1);
            continue;
        }

        /* 若刚从手动模式切回自动模式，重新读取配置文件并强制瞬间接管硬件 */
        if (g_last_fan_mode != 0) {
            log_write(1, "Fan control switched back to AUTO mode! Reloading config and forcing immediate update.");
            reload_config_from_disk();
            g_fan0_speed = -1;
            g_fan2_speed = -1;
            g_fan4_speed = -1;
            g_fan6_speed = -1;
            g_level_sys = -1;
            g_level_cpu0 = -1;
            g_level_cpu1 = -1;
            g_last_fan_mode = 0;
            tick_count = g_cfg.check_interval; /* 触发立即执行调速 */
        }

        /* 8. 执行高级全风道温控采样与调速周期 (无需反复轮询 stat 检查配置文件) */
        tick_count++;
        if (tick_count >= g_cfg.check_interval) {
            tick_count = 0;
            process_fan_control_cycle(intrusion);
        }

        sleep(1);
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 2: set_pwm_dutycycle (直接调用底层驱动句柄，彻底消除递归死锁)       */
/* -------------------------------------------------------------------------- */
int set_pwm_dutycycle(int device_num, int pwm_num, int dutycycle) {
    init_hardware_handles();

    if (g_cfg.disabled_fan_mask & (1 << pwm_num)) {
        if (raw_set_pwm_dutycycle) {
            return raw_set_pwm_dutycycle(device_num, pwm_num, 0);
        }
        return 0;
    }

    dutycycle = clamp_fan_speed(dutycycle, g_cfg.fan_min, g_cfg.fan_max);

    int fan_id = get_fan_for_pwm(pwm_num);
    if (g_inspur_fan_state) {
        if (fan_id >= 0 && fan_id < 8) {
            *(uint16_t *)(g_inspur_fan_state + 56 + 2 * fan_id) = (uint16_t)dutycycle;
            g_inspur_fan_state[72 + fan_id] = (uint8_t)pwm_num;
        }
        if (pwm_num >= 0 && pwm_num < 8) {
            *(uint16_t *)(g_inspur_fan_state + 56 + 2 * pwm_num) = (uint16_t)dutycycle;
        }
    }

    /* 当外部手动设置转速时，失效 Hook 自动缓存，确保切回自动时能立即接管 */
    if (inspur_get_fan_control_mode() != 0) {
        g_fan0_speed = -1;
        g_fan2_speed = -1;
        g_fan4_speed = -1;
        g_fan6_speed = -1;
    }

    if (raw_set_pwm_dutycycle) {
        return raw_set_pwm_dutycycle(device_num, pwm_num, dutycycle);
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 3: PDK_GetFanConfig (针对禁用风扇返回 0，未配置插槽返回 0)         */
/* -------------------------------------------------------------------------- */
int PDK_GetFanConfig(int fan_id) {
    init_hardware_handles();

    if (fan_id >= 0 && fan_id < 8 && (g_cfg.disabled_fan_mask & (1 << fan_id))) {
        return 0; /* 0 = 未配置 / 空插槽 */
    }

    if (raw_PDK_GetFanConfig) {
        return raw_PDK_GetFanConfig(fan_id);
    }

    /* SA5212M4 在位风扇为 Fan 0, Fan 2, Fan 4, Fan 6 */
    return (fan_id == 0 || fan_id == 2 || fan_id == 4 || fan_id == 6) ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 4: PDK_GetFanStatus (针对禁用风扇返回 2，消除 0 RPM 告警与红灯)     */
/* -------------------------------------------------------------------------- */
int PDK_GetFanStatus(int fan_id, uint8_t *status, uint16_t *speed, uint8_t *param4) {
    init_hardware_handles();

    if (fan_id >= 0 && fan_id < 8 && (g_cfg.disabled_fan_mask & (1 << fan_id))) {
        return 2; /* 2 = DEVICE_NOT_PRESENT */
    }

    if (raw_PDK_GetFanStatus) {
        return raw_PDK_GetFanStatus(fan_id, status, speed, param4);
    }

    return 2;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 5: PDK_GetFanPWMNo (提供绝对准确的 AST2300 硬件 PWM 路由)          */
/* -------------------------------------------------------------------------- */
int PDK_GetFanPWMNo(int fan_id) {
    init_hardware_handles();
    return get_pwm_for_fan(fan_id);
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 6 & 7: 风扇控制模式获取与设置 (实时响应 WebUI 与 IPMI 模式切换)    */
/* -------------------------------------------------------------------------- */
int inspur_get_fan_control_mode(void) {
    init_hardware_handles();
    if (g_inspur_fan_state) {
        return (int)g_inspur_fan_state[96]; /* offset 0x60 */
    }
    return 0;
}

int inspur_set_fan_control_mode(int mode) {
    init_hardware_handles();
    if (mode > 1) return -1;

    log_write(1, "inspur_set_fan_control_mode called: mode=%d", mode);

    if (g_inspur_fan_state) {
        g_inspur_fan_state[96] = (uint8_t)mode;
    }

    if (mode == 0) {
        /*
         * 【核心关键】用户从 Web 后台或 IPMI 切回 AUTO 自动模式:
         * 重新从磁盘读取最新配置 (使得 /conf/fan_control.conf 修改即刻生效，无需后台高频轮询 stat)，
         * 立即重置全部转速缓存与迟滞状态，并在当前线程内【同步、即刻】执行一次
         * 完整的温度采样与硬件 PWM 写入，使 Web 页面刷新时 0 延迟呈现最新自动转速！
         */
        reload_config_from_disk();
        g_fan0_speed = -1;
        g_fan2_speed = -1;
        g_fan4_speed = -1;
        g_fan6_speed = -1;
        g_level_sys = -1;
        g_level_cpu0 = -1;
        g_level_cpu1 = -1;
        g_last_fan_mode = 0;

        process_fan_control_cycle(0);
        log_write(1, "Instant real-time auto takeover with reloaded config complete upon mode switch to AUTO.");
    } else {
        g_last_fan_mode = 1;
        g_fan0_speed = -1;
        g_fan2_speed = -1;
        g_fan4_speed = -1;
        g_fan6_speed = -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 8: InspurSetFanControlMode (IPMI OEM Cmd 0x7a, WebUI 核心切换接口) */
/* -------------------------------------------------------------------------- */
int InspurSetFanControlMode(unsigned char *req, unsigned char req_len, unsigned char *res, int flag) {
    (void)flag;
    init_hardware_handles();

    if (!req || !res) return 0;
    if (req_len != 1) {
        *res = 0xC7; /* CC_REQ_INV_LEN */
        return 1;
    }

    int mode = (int)req[0];
    log_write(1, "InspurSetFanControlMode (Web/IPMI 0x7a) called with mode=%d", mode);

    int ret = inspur_set_fan_control_mode(mode);
    if (ret == 0) {
        *res = 0;
    } else {
        *res = 0xCC; /* CC_PARAM_OUT_OF_RANGE */
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 9: InspurGetFanControlMode (IPMI OEM Cmd 0x7b, WebUI 模式查询)     */
/* -------------------------------------------------------------------------- */
int InspurGetFanControlMode(unsigned char *req, unsigned char req_len, unsigned char *res, int flag) {
    (void)flag;
    init_hardware_handles();

    if (!req || !res) return 0;
    if (req_len != 0) {
        *res = 0xC7;
        return 1;
    }

    res[0] = 0;
    res[1] = (unsigned char)inspur_get_fan_control_mode();
    return 2;
}

/* -------------------------------------------------------------------------- */
/* 劫持接口 10: InspurSetFanPwmDuty (IPMI OEM Cmd 0x78, WebUI 手动控速设置)    */
/* -------------------------------------------------------------------------- */
int InspurSetFanPwmDuty(unsigned char *req, unsigned char req_len, unsigned char *res, int flag) {
    (void)flag;
    init_hardware_handles();

    if (!req || !res) return 0;
    if (req_len != 2) {
        *res = 0xC7;
        return 1;
    }

    if (inspur_get_fan_control_mode() == 0) {
        *res = 0xD5; /* 0x2A (~0x2A = 0xD5) 自动模式下禁止手动设置转速 */
        return 1;
    }

    int fan_id = (int)req[0];
    int duty = (int)req[1];
    log_write(1, "InspurSetFanPwmDuty (Web/IPMI 0x78): fan_id=%d, duty=%d%%", fan_id, duty);

    if (fan_id < 0 || fan_id >= 8) {
        *res = 0xCC;
        return 1;
    }

    int pwm_num = get_pwm_for_fan(fan_id);
    if (pwm_num < 0 || pwm_num >= 8) {
        *res = 0xCC;
        return 1;
    }

    duty = clamp_fan_speed(duty, 1, 100);

    if (g_inspur_fan_state) {
        *(uint16_t *)(g_inspur_fan_state + 56 + 2 * fan_id) = (uint16_t)duty;
        *(uint16_t *)(g_inspur_fan_state + 56 + 2 * pwm_num) = (uint16_t)duty;
        g_inspur_fan_state[72 + fan_id] = (uint8_t)pwm_num;
    }

    /* 手动修改后失效自动转速缓存 */
    g_fan0_speed = -1;
    g_fan2_speed = -1;
    g_fan4_speed = -1;
    g_fan6_speed = -1;

    if (raw_set_pwm_dutycycle) {
        raw_set_pwm_dutycycle(0, pwm_num, duty);
    }

    *res = 0;
    return 1;
}
