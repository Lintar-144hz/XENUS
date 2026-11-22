/*
 * XENUS example plugin - the smallest useful plugin: reports its identity,
 * checks the API version, logs, posts one notification, shuts down.
 *
 * Built as a shared object and loaded through dlopen(); it uses the host
 * API only through the xenus_api_t table, never through XENUS globals.
 */
#include <xenus/app.h>
#include <xenus/plugin.h>

static const xenus_api_t *host_api;

static const xenus_plugin_info_t plugin_info = {
    .info_size = sizeof(xenus_plugin_info_t),
    .abi_version = XENUS_PLUGIN_ABI_VERSION,
    .name = "example",
    .version = "1.0.0",
    .description = "Minimal XENUS example plugin",
    .api_major = XENUS_API_VERSION_MAJOR,
    .api_minor = XENUS_API_VERSION_MINOR
};

XENUS_PLUGIN_EXPORT const xenus_plugin_info_t *xenus_plugin_info(void)
{
    return &plugin_info;
}

XENUS_PLUGIN_EXPORT int xenus_plugin_init(const xenus_api_t *api)
{
    if (!api)
        return -1;
    if (api->version_check(XENUS_API_VERSION_MAJOR, XENUS_API_VERSION_MINOR)
        != XENUS_OK)
        return -2;

    host_api = api;
    api->log(XENUS_LOG_INFO, "example: initialized");
    api->notify("Example plugin", "example plugin is running");
    return 0;
}

XENUS_PLUGIN_EXPORT void xenus_plugin_shutdown(void)
{
    if (!host_api)
        return;
    host_api->log(XENUS_LOG_INFO, "example: shutdown");
    host_api = NULL;
}
