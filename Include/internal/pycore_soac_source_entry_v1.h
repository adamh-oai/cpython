/* Function-owned source-entry registration; none is installed automatically. */
#ifndef Py_INTERNAL_SOAC_SOURCE_ENTRY_V1_H
#define Py_INTERNAL_SOAC_SOURCE_ENTRY_V1_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_soac_call_v1.h"

/* GIL-held, no Python references/callbacks/error changes. Invalidation publishes
 * absence before freeing the C-only record. It never changes permanent strict
 * identity, owner state, required-boundary policy, metadata or vectorcall.
 */
PyAPI_FUNC(void) _PySoacSourceEntry_InvalidateV1(PyFunctionObject *function);

/* Copy an exact currently matching record into producer-owned C storage.
 * Returns 1 on match, 0 on absent/stale (stale is invalidated). No exception is
 * set/cleared. The caller must Pin before any callback or use of metadata.
 * A copied spec is not a reusable capability or an immutable Python pin.
 */
PyAPI_FUNC(int) _PySoacSourceEntry_CopyV1(
    PyFunctionObject *function, PySoacSourceEntrySpecV1 *out);

#endif
