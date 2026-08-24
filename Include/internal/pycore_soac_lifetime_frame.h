#ifndef Py_INTERNAL_SOAC_LIFETIME_FRAME_H
#define Py_INTERNAL_SOAC_LIFETIME_FRAME_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif
#include "pycore_interpframe_structs.h"
#ifdef __cplusplus
extern "C" {
#endif

static inline int
_PyFrame_IsSoacLifetime(const _PyInterpreterFrame *frame)
{
    return frame->owner >= FRAME_OWNED_BY_SOAC_ACTIVE &&
           frame->owner <= FRAME_OWNED_BY_SOAC_CLEARED;
}

/* Check before eval-frame setup, recursion handling, or an eval-frame hook.
 * A valid original code object is not permission to evaluate parked locals. */
static inline int
_PyFrame_CheckSoacLifetimeExecution(const _PyInterpreterFrame *frame)
{
    if (_PyFrame_IsSoacLifetime(frame)) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "an optimized source-lifetime frame cannot execute bytecode");
        return -1;
    }
    return 0;
}

PyAPI_FUNC(PyFrameObject *) _PyFrame_GetSoacLifetimeObject(_PyInterpreterFrame *frame);
#ifdef __cplusplus
}
#endif
#endif
