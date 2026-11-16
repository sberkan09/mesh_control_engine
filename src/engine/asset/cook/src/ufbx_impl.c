/* ufbx implementation for the cook (compiled without the engine warning policy: third-party; ADR-0047).
 * Single-threaded (no thread pool is ever passed), memory sources only in the cook; the OBJ and geometry-cache
 * readers are compiled out (UFBX_NO_FORMAT_OBJ, UFBX_NO_GEOMETRY_CACHE in cmake/Dependencies.cmake). */
#include "ufbx.c" /* NOLINT(bugprone-suspicious-include): ufbx ships its implementation as one .c */
