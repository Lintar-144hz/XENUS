#include "../src/cache.h"

#include <stdio.h>

static int checks;
static int failures;
static int releases;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));           \
        }                                                                    \
    } while (0)

static void release_unit(void)
{
    releases++;
    xenus_cache_set("unit", 0, 1);
}


static int new_releases;

static void release_new(void)
{
    new_releases++;
}

static void release_replace(void)
{
    releases++;
    xenus_cache_unregister("victim");
    xenus_cache_register("replacement", release_new);
    xenus_cache_set("replacement", 512u, 1);
}

int main(void)
{
    const size_t over_budget = 3u * 1024u * 1024u;

    CHECK(xenus_cache_register(NULL, release_unit) != 0,
          "null cache name rejected");
    CHECK(xenus_cache_register("", release_unit) != 0,
          "empty cache name rejected");
    CHECK(xenus_cache_register("unit", release_unit) == 0,
          "register cache");
    CHECK(xenus_cache_register("unit", release_unit) == 0,
          "duplicate register is idempotent");

    xenus_cache_set("unit", 4096u, 1);
    CHECK(xenus_cache_bytes() == 4096u, "cache bytes tracked");
    CHECK(xenus_cache_trim() == 1, "trim releases cache");
    CHECK(releases == 1, "release callback called");
    CHECK(xenus_cache_bytes() == 0, "released cache has zero bytes");
    CHECK(xenus_cache_trim() == 0, "trim is idempotent");

    releases = 0;
    xenus_cache_set("unit", over_budget, 0);
    CHECK(xenus_cache_enforce() == 0, "non-releasable cache kept");
    CHECK(xenus_cache_bytes() == over_budget, "non-releasable bytes kept");

    xenus_cache_set("unit", over_budget, 1);
    CHECK(xenus_cache_enforce() == 1, "budget overrun releases cache");
    CHECK(releases == 1, "budget release called once");
    CHECK(xenus_cache_bytes() == 0, "budget release clears bytes");

    xenus_cache_set("unit", 1024u, 1);
    xenus_cache_unregister("unit");
    CHECK(xenus_cache_bytes() == 0, "unregister drops accounting");
    xenus_cache_set("unit", 2048u, 1);
    CHECK(xenus_cache_bytes() == 0, "set after unregister ignored");


    /* Callback mutation: a newly registered cache must wait for next trim. */
    xenus_cache_unregister("unit");
    xenus_cache_register("trigger", release_replace);
    xenus_cache_register("victim", release_new);
    xenus_cache_set("trigger", 1024u, 1);
    xenus_cache_set("victim", 1024u, 1);
    new_releases = 0;

    CHECK(xenus_cache_trim() == 1,
          "new cache is not trimmed during current pass");
    CHECK(new_releases == 0,
          "replacement callback is not called immediately");
    CHECK(xenus_cache_bytes() == 512u,
          "replacement cache remains registered");

    xenus_cache_unregister("trigger");
    xenus_cache_unregister("replacement");

    /* Integer overflow must saturate instead of wrapping. */
    xenus_cache_register("overflow-a", NULL);
    xenus_cache_register("overflow-b", NULL);
    xenus_cache_set("overflow-a", (size_t)-1, 0);
    xenus_cache_set("overflow-b", 1u, 0);
    CHECK(xenus_cache_bytes() == (size_t)-1,
          "cache byte total saturates on overflow");
    xenus_cache_unregister("overflow-a");
    xenus_cache_unregister("overflow-b");

    printf("cache_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
