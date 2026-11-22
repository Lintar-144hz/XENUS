/*
 * Test plugin: valid metadata, but init() fails on purpose so the host can
 * prove that a failing plugin is isolated and never marked running.
 */
#include <xenus/app.h>
#include <xenus/plugin.h>

static const xenus_plugin_info_t plugin_info = {
    .info_size = sizeof(xenus_plugin_info_t),
    .abi_version = XENUS_PLUGIN_ABI_VERSION,
    .name = "failinit",
    .version = "0.1.0",
    .description = "Fails during init() on purpose",
    .api_major = XENUS_API_VERSION_MAJOR,
    .api_minor = XENUS_API_VERSION_MINOR
};

XENUS_PLUGIN_EXPORT const xenus_plugin_info_t *xenus_plugin_info(void)
{
    return &plugin_info;
}

XENUS_PLUGIN_EXPORT int xenus_plugin_init(const xenus_api_t *api)
{
    (void)api;
    return -7;
}

XENUS_PLUGIN_EXPORT void xenus_plugin_shutdown(void)
{
    /* must never run: init() failed */
}
