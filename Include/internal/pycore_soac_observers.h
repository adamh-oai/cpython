#ifndef Py_INTERNAL_SOAC_OBSERVERS_H
#define Py_INTERNAL_SOAC_OBSERVERS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Scalar-only refusals can propagate while the caller holds stopped-world /
 * HEAD_LOCK. Resolve them only AFTER those locks have been released. */
#define _Py_SOAC_OBSERVER_REFUSED (-2)

/* GIL build: caller holds the GIL, but MUST NOT already hold HEAD_LOCK.
 * These acquire the native thread-list lock, inspect borrowed observer scope
 * fields only, release it, and never allocate, set PyErr or call Python. */
extern int _PySoacSource_HasProtectedInterval(
    PyInterpreterState *interp, PyCodeObject *code);
/* Same scalar scan when the existing all-thread tracing caller already owns
 * HEAD_LOCK. This is a lock-state variant, never an admission bypass. */
extern int _PySoacSource_HasProtectedIntervalThreadListLocked(
    PyInterpreterState *interp, PyCodeObject *code);
extern int _PyMonitoring_SetLocalEventsThreadListLocked(
    PyCodeObject *code, int tool_id, uint32_t events);
extern int _PySoacSource_HasObserverReservation(PyInterpreterState *interp);
extern int _PySoacSource_RestartWouldObserve(PyInterpreterState *interp);

/* No callbacks/allocation: interpret actual configured native monitor masks
 * and tool versions, including legacy masks, not tracing suppression. */
extern int _PySoacSource_CodeHasObservers(
    PyInterpreterState *interp, PyCodeObject *code);

/* Existing contextual calls reject observers for their active SOAC code
 * without constructing a Python frame or native opcode site. */
extern int _PySoacSource_CheckCallObservers(PyThreadState *tstate);

/* These two internal exports are used by the real _testinternalcapi setter.
 * They are native observer-API transaction state, not compiler context.
 * Increment/decrement the exact current native tstate; nesting allowed.
 * No lock survives the call. Free-threaded builds have no interval support
 * and retain ordinary setter behavior through an inert reservation. */
PyAPI_FUNC(int) _PySoacSource_BeginObserverReservation(PyThreadState *tstate);
PyAPI_FUNC(void) _PySoacSource_EndObserverReservation(PyThreadState *tstate);

/* Explicit fallible internal setter. The public void ABI is unchanged and
 * reports failure after saving its incoming exception. */
PyAPI_FUNC(int) _PyInterpreterState_SetEvalFrameFuncChecked(
    PyInterpreterState *interp, _PyFrameEvalFunction eval_frame);

static inline int
_PySoacSource_ResolveObserverStatus(int status)
{
    if (status == _Py_SOAC_OBSERVER_REFUSED) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "cannot enable source observers during a SOAC observer scope");
        return -1;
    }
    return status;
}

/* saved is the actual detached incoming error, not a borrowed exception.
 * Call after all native mutation locks are released. Diagnostics may reenter;
 * the observer scope stays present until its actual interval ends. */
static inline void
_PySoacSource_FinishVoidSetter(int status, PyObject *saved, const char *name)
{
    if (status < 0) {
        (void)_PySoacSource_ResolveObserverStatus(status);
        PyErr_FormatUnraisable("Exception ignored in %s", name);
    }
    PyErr_SetRaisedException(saved);
}

#ifdef __cplusplus
}
#endif
#endif
