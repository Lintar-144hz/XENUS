#include "cache.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>

#define CACHE_SLOTS 8
#define CACHE_NAME_MAX 24
#define CACHE_BUDGET_BYTES (2u * 1024u * 1024u)
#define CACHE_CHECK_MS 10000
#define CACHE_LOW_KB (64u * 1024u)

typedef struct {
    int used;
    unsigned long generation;
    char name[CACHE_NAME_MAX];
    size_t bytes;
    int releasable;
    void (*release)(void);
} CacheSlot;

static CacheSlot slots[CACHE_SLOTS];
static unsigned long long last_check_ms;
static unsigned long next_generation;

static unsigned long long cache_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}

static CacheSlot *find_slot(const char *name)
{
    int i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (slots[i].used && strcmp(slots[i].name, name) == 0)
            return &slots[i];
    }
    return NULL;
}

int xenus_cache_register(const char *name, void (*release)(void))
{
    CacheSlot *slot;
    int i;

    if (name == NULL || name[0] == '\0' || strlen(name) >= CACHE_NAME_MAX)
        return -1;
    slot = find_slot(name);
    if (slot != NULL) {
        slot->release = release;
        return 0;
    }
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (!slots[i].used) {
            slots[i].used = 1;
            slots[i].generation = ++next_generation;
            if (slots[i].generation == 0)
                slots[i].generation = ++next_generation;
            snprintf(slots[i].name, sizeof(slots[i].name), "%s", name);
            slots[i].bytes = 0;
            slots[i].releasable = 0;
            slots[i].release = release;
            return 0;
        }
    }
    return -1;
}

void xenus_cache_set(const char *name, size_t bytes, int releasable)
{
    CacheSlot *slot = find_slot(name);

    if (slot == NULL)
        return;
    slot->bytes = bytes;
    slot->releasable = releasable != 0;
}

void xenus_cache_unregister(const char *name)
{
    CacheSlot *slot = find_slot(name);

    if (slot != NULL)
        memset(slot, 0, sizeof(*slot));
}

size_t xenus_cache_bytes(void)
{
    size_t total = 0;
    int i;

    for (i = 0; i < CACHE_SLOTS; i++) {
        if (slots[i].used) {
            if (slots[i].bytes > (size_t)-1 - total)
                return (size_t)-1;
            total += slots[i].bytes;
        }
    }
    return total;
}

static int mem_available_kb(unsigned long *out)
{
    FILE *f;
    char line[128];
    unsigned long kb = 0;

    if (out == NULL)
        return 0;
    f = fopen("/proc/meminfo", "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1)
                break;
        }
        fclose(f);
        if (kb > 0) {
            *out = kb;
            return 1;
        }
    }
    {
        struct sysinfo si;

        if (sysinfo(&si) == 0 && si.mem_unit > 0) {
            *out = (unsigned long)si.freeram *
                   (unsigned long)si.mem_unit / 1024ul;
            return 1;
        }
    }
    return 0;
}

int xenus_cache_trim(void)
{
    int pending_slots[CACHE_SLOTS];
    unsigned long generations[CACHE_SLOTS];
    void (*callbacks[CACHE_SLOTS])(void);
    int count = 0;
    int i;
    int released = 0;

    /* Snapshot eligible entries before invoking callbacks. */
    for (i = 0; i < CACHE_SLOTS; i++) {
        if (!slots[i].used || !slots[i].releasable ||
            slots[i].bytes == 0 || slots[i].release == NULL)
            continue;

        pending_slots[count] = i;
        generations[count] = slots[i].generation;
        callbacks[count] = slots[i].release;
        count++;

        slots[i].bytes = 0;
        slots[i].releasable = 0;
    }

    for (i = 0; i < count; i++) {
        int slot = pending_slots[i];

        /* Skip entries removed or replaced by an earlier callback. */
        if (!slots[slot].used ||
            slots[slot].generation != generations[i] ||
            slots[slot].release != callbacks[i])
            continue;

        callbacks[i]();
        released++;
    }

    return released;
}

int xenus_cache_enforce(void)
{
    unsigned long available = 0;
    size_t total = xenus_cache_bytes();

    if (total > CACHE_BUDGET_BYTES)
        return xenus_cache_trim();
    if (mem_available_kb(&available) && available < CACHE_LOW_KB)
        return xenus_cache_trim();
    return 0;
}

void xenus_cache_tick(void)
{
    unsigned long long now = cache_now_ms();

    if (last_check_ms != 0 && now >= last_check_ms &&
        now - last_check_ms < CACHE_CHECK_MS)
        return;
    last_check_ms = now;
    (void)xenus_cache_enforce();
}
