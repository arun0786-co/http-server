/* Request target to a safe path under the document root. */
#ifndef HP_PATH_H
#define HP_PATH_H

#include <stddef.h>

/* Decodes %XX escapes, drops the query and fragment, resolves "." and ".." segments and
 * collapses repeated slashes. Writes a path that starts with '/' and never leaves the root.
 * Returns 0, or the HTTP status to answer: 400 (malformed, or any control byte such as NUL,
 * raw or encoded), 403 (climbs above the root) or 414 (too long for `out`). */
int hp_resolve(const char *target, size_t len, char *out, size_t out_size);

#endif
