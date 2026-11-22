/*
 * Test plugin: reports an impossible required API version (99.0) so the
 * host must reject it during validation, before init() is ever called.
 */
#include <xenus/app.h>
#include <xenus/plugin.h>

static const xenus_plugin_info_t plugin_info = {
    .info_size = sizeof(xenus_plugin_info_t),
    .abi_version = XENUS_PLUGIN_ABI_VERSION,
    .name = "badver",
    .version = "9.9.9",
    .description = "Requires a future API version",
    .api_major = 99,
    .api_minor = 0
};

XENUS_PLUGIN_EXPORT const xenus_plugin_info_t *xenus_plugin_info(void)
{
    return &plugin_info;
}

XENUS_PLUGIN_EXPORT int xenus_plugin_init(const xenus_api_t *api)
{
    (void)api;
    return 0;                      /* never reached: version check fails */
}

XENUS_PLUGIN_EXPORT void xenus_plugin_shutdown(void)
{
}
