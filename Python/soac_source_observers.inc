/* Observer admission and mutation barriers for actual linked SOAC source
 * frames. Ordinary CPython frames retain their normal tracing behavior. */
#include "pycore_soac_observers.h"

int
_PySoacSource_CodeHasObservers(PyInterpreterState *interp, PyCodeObject *code)
{
    /* Configured entitlement, not effective delivery. Tracing suppression,
     * callback=None and a prior DISABLE response do not authorize omission. */
    if (interp->sys_tracing_threads || interp->sys_profiling_threads) return 1;
    for (int e = 0; e < _PY_MONITORING_UNGROUPED_EVENTS; e++) {
        if (interp->monitors.tools[e]) return 1;
    }
    _PyCoMonitoringData *data = code->_co_monitoring;
    if (data != NULL) {
        for (int tool = 0; tool < PY_MONITORING_TOOL_IDS; tool++) {
            if (data->tool_versions[tool] != interp->monitoring_tool_versions[tool]) {
                continue;
            }
            for (int e = 0; e < _PY_MONITORING_LOCAL_EVENTS; e++) {
                if (data->local_monitors.tools[e] & (1 << tool)) return 1;
            }
        }
    }
    return 0;
}

int
_PySoacSource_HasProtectedIntervalThreadListLocked(
    PyInterpreterState *interp, PyCodeObject *code)
{
#ifdef Py_GIL_DISABLED
    (void)interp; (void)code;
    return 0;
#else
    _Py_FOR_EACH_TSTATE_UNLOCKED(interp, tstate) {
        for (_PyInterpreterFrame *frame = tstate->current_frame;
             frame != NULL; frame = frame->previous) {
            if (frame->owner == FRAME_OWNED_BY_SOAC_ACTIVE &&
                (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED) &&
                (code == NULL || _PyFrame_GetCode(frame) == code)) {
                return 1;
            }
        }
    }
    return 0;
#endif
}

int
_PySoacSource_HasProtectedInterval(PyInterpreterState *interp, PyCodeObject *code)
{
#ifdef Py_GIL_DISABLED
    (void)interp; (void)code;
    return 0;
#else
    HEAD_LOCK(interp->runtime);
    int result = _PySoacSource_HasProtectedIntervalThreadListLocked(interp, code);
    HEAD_UNLOCK(interp->runtime);
    return result;
#endif
}

int
_PySoacSource_HasObserverReservation(PyInterpreterState *interp)
{
    int result = 0;
#ifndef Py_GIL_DISABLED
    _Py_FOR_EACH_TSTATE_BEGIN(interp, tstate) {
        if (((_PyThreadStateImpl *)tstate)->soac_observer_reservation_depth) {
            result = 1;
            break;
        }
    }
    _Py_FOR_EACH_TSTATE_END(interp);
#else
    (void)interp;
#endif
    return result;
}

int
_PySoacSource_RestartWouldObserve(PyInterpreterState *interp)
{
    int result = 0;
#ifndef Py_GIL_DISABLED
    _Py_FOR_EACH_TSTATE_BEGIN(interp, tstate) {
        for (_PyInterpreterFrame *frame = tstate->current_frame;
             frame != NULL; frame = frame->previous) {
            if (frame->owner == FRAME_OWNED_BY_SOAC_ACTIVE &&
                (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED) &&
                _PySoacSource_CodeHasObservers(interp, _PyFrame_GetCode(frame))) {
                result = 1;
                break;
            }
        }
        if (result) break;
    }
    _Py_FOR_EACH_TSTATE_END(interp);
#else
    (void)interp;
#endif
    return result;
}

int
_PySoacSource_BeginObserverReservation(PyThreadState *tstate)
{
    if (PyErr_Occurred()) return -1;
    if (tstate != _PyThreadState_GET()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "observer reservation requires the current thread state");
        return -1;
    }
#ifndef Py_GIL_DISABLED
    if (_PySoacSource_HasProtectedInterval(tstate->interp, NULL)) {
        return _PySoacSource_ResolveObserverStatus(_Py_SOAC_OBSERVER_REFUSED);
    }
    _PyThreadStateImpl *impl = (_PyThreadStateImpl *)tstate;
    if (impl->soac_observer_reservation_depth == UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "native observer reservation overflow");
        return -1;
    }
    impl->soac_observer_reservation_depth++;
#endif
    return 0;
}

void
_PySoacSource_EndObserverReservation(PyThreadState *tstate)
{
    if (tstate != _PyThreadState_GET()) {
        Py_FatalError("observer reservation ended on a different thread state");
    }
#ifndef Py_GIL_DISABLED
    _PyThreadStateImpl *impl = (_PyThreadStateImpl *)tstate;
    if (impl->soac_observer_reservation_depth == 0) {
        Py_FatalError("observer reservation ended without a matching begin");
    }
    impl->soac_observer_reservation_depth--;
#endif
}

