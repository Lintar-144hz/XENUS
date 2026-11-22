/*
 * XENUS BIOS backend - read-only environment facts and safe bridge detection.
 *
 * Nothing in this file loads kernel modules, changes Android firmware or
 * bypasses permissions.  It only inspects procfs/sysfs, the PATH and the
 * Termux/Android filesystem layout that is already reachable by the user.
 */
#ifndef XENUS_BIOS_BACKEND_H
#define XENUS_BIOS_BACKEND_H

#include <stddef.h>

typedef struct {
    char os[64];
    char kernel[64];
    char arch[32];
    char hostname[64];
    char cpu_model[96];
    int cpus;
    int android;
    int termux;
    int proot;
    int x11;
    int termux_x11;
    unsigned long mem_total_mb;
    unsigned long mem_available_mb;
    unsigned long uptime_sec;
    double load1;
    double load5;
    double load15;
    char display[64];
    char termux_prefix[160];

    int audio_available;
    int network_available;
    int android_cmd_available;
    int termux_api_available;
    int termux_x11_available;
    int notifications_available;
    int clipboard_available;
    int storage_available;
    int xenus_binary_available;

    char audio_backend[64];
    char network_iface[32];
    char android_detail[160];
    char termux_detail[160];
} xenus_bios_env_t;

int xenus_bios_env_probe(xenus_bios_env_t *env);
int xenus_bios_command_exists(const char *name);

#endif /* XENUS_BIOS_BACKEND_H */
