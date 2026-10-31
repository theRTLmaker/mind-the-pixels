#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// HTTPS GET of url into a malloc'd, NUL-terminated body of at most capacity
// bytes (the caller frees it). api_key, when non-empty, is sent as the
// x-apikey header. Returns NULL on any failure, non-200 status or a body
// that does not fit.
char *zectrix_http_get(const char *url, const char *api_key, size_t capacity);

// Receives the body in chunks as it arrives.
typedef void (*zectrix_http_chunk_cb)(void *context, const char *data,
                                      size_t length);

// HTTPS GET of url, handing the body to on_chunk piece by piece instead of
// storing it. Returns true on HTTP 200.
bool zectrix_http_get_stream(const char *url, const char *api_key,
                             zectrix_http_chunk_cb on_chunk, void *context);

#ifdef __cplusplus
}
#endif
