#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// HTTPS GET of url into a malloc'd, NUL-terminated body of at most capacity
// bytes (the caller frees it). api_key, when non-empty, is sent as the
// x-apikey header. Returns NULL on any failure, non-200 status or a body
// that does not fit.
char *zectrix_http_get(const char *url, const char *api_key, size_t capacity);

#ifdef __cplusplus
}
#endif
