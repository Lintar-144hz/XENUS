#ifndef XENUS_CACHE_H
#define XENUS_CACHE_H

#include <stddef.h>

int xenus_cache_register(const char *name, void (*release)(void));
void xenus_cache_set(const char *name, size_t bytes, int releasable);
void xenus_cache_unregister(const char *name);
void xenus_cache_tick(void);
int xenus_cache_enforce(void);
int xenus_cache_trim(void);
size_t xenus_cache_bytes(void);

#endif
