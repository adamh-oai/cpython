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

enum {
    SOAC_LIFETIME_OWNS_GLOBALS = 1,
    SOAC_LIFETIME_OWNS_BUILTINS = 2,
    /* Scope state is mutually exclusive with FINISHED environment ownership. */
    SOAC_LIFETIME_SCOPE_LINKED = 4,
    SOAC_LIFETIME_SCOPE_TERMINAL = 8,
    /* Nonowning managed executable view; not a generic token flag. */
    SOAC_LIFETIME_BORROWED_CODE = 16,
    SOAC_LIFETIME_SYNC_BODY = 32,
    SOAC_LIFETIME_SYNC_RESUMED = 64,
};

typedef struct {
    uintptr_t version;
    uintptr_t state;
    uintptr_t storage;
    uintptr_t thread;
    uintptr_t frame;
    uintptr_t previous;
} SoacLifetimeScopeStateV1;

enum {
    SOAC_SCOPE_V1_LINKED = 1,
    SOAC_SCOPE_V1_SUSPENDED = 2,
    SOAC_SCOPE_V1_TERMINAL = 3,
};


/* Internal transport remains memcpy-based. The storage identity may name a
 * six-word scope or the native prefix of a twelve-word body interval. */
extern int _PyFrame_PrepareSoacScopeV1(
    SoacLifetimeScopeStateV1 *state, uintptr_t storage, PyFrameObject *object,
    PyObject *actual_function, PyObject *globals, PyObject *builtins);
extern void _PyFrame_LinkSoacScopeV1(
    const SoacLifetimeScopeStateV1 *state, void *storage,
    PyObject *actual_function, PyObject *globals, PyObject *builtins,
    uint16_t extra_flags);
extern int _PyFrame_ValidateSoacScopeV1(
    const SoacLifetimeScopeStateV1 *state, uintptr_t storage, uint16_t extra_flags);
extern void _PyFrame_UnlinkSoacScopeV1(
    SoacLifetimeScopeStateV1 *state, void *storage, int terminal);

/* Detach first: clearing a captured mapping can run finalizers that recursively
 * clear this same frame or its traceback. Ordinary borrowed maps are untouched.
 * Call before releasing the frame's actual function reference. */
static inline void
_PyFrame_ClearSoacLifetimeEnvironment(_PyInterpreterFrame *frame)
{
    assert(_PyFrame_IsSoacLifetime(frame));
    uint16_t owned = frame->soac_lifetime_owned_environment;
    assert(!(owned & SOAC_LIFETIME_SCOPE_LINKED));
    frame->soac_lifetime_owned_environment = owned & SOAC_LIFETIME_BORROWED_CODE;
    PyObject *globals = NULL;
    PyObject *builtins = NULL;
    if (owned & SOAC_LIFETIME_OWNS_GLOBALS) {
        globals = frame->f_globals;
        frame->f_globals = NULL;
    }
    if (owned & SOAC_LIFETIME_OWNS_BUILTINS) {
        builtins = frame->f_builtins;
        frame->f_builtins = NULL;
    }
    if (owned != 0) {
        PyObject *error = PyErr_GetRaisedException();
        Py_XDECREF(globals);
        Py_XDECREF(builtins);
        PyErr_SetRaisedException(error);
    }
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
