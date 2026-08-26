/* Observer refusal for active SOAC calls, independent of Python frames.
 * Ordinary CPython frames retain their normal tracing behavior. */
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
        for (PySoacObserverScopeV1 *scope =
                 ((_PyThreadStateImpl *)tstate)->soac_observer_scope;
             scope != NULL; scope = scope->_previous) {
            if (code == NULL || scope->_code == code) {
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
        for (PySoacObserverScopeV1 *scope =
                 ((_PyThreadStateImpl *)tstate)->soac_observer_scope;
             scope != NULL; scope = scope->_previous) {
            if (_PySoacSource_CodeHasObservers(interp, scope->_code)) {
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

static int
soac_observer_scope_error(PyObject *exception, const char *message)
{
    if (!PyErr_Occurred()) {
        PyErr_SetString(exception, message);
    }
    return -1;
}

#ifndef Py_GIL_DISABLED
static int
soac_observer_scope_storage_valid(PySoacObserverScopeV1 *scope, size_t size)
{
    if (scope == NULL || size != sizeof(*scope) ||
        (uintptr_t)scope % _Alignof(PySoacObserverScopeV1) != 0) {
        return soac_observer_scope_error(
            PyExc_ValueError, "invalid SOAC observer scope size or alignment");
    }
    return 0;
}
#endif

int
PySoac_EnterObserverScopeV1(
    PySoacObserverScopeV1 *scope, size_t scope_size, PyCodeObject *actual_code)
{
    if (PyErr_Occurred()) return -1;
#ifdef Py_GIL_DISABLED
    return soac_observer_scope_error(
        PyExc_NotImplementedError, "SOAC observer scopes require the GIL build");
#else
    if (soac_observer_scope_storage_valid(scope, scope_size) < 0) return -1;
    if (scope->_previous != NULL || scope->_thread != NULL ||
        scope->_code != NULL) {
        return soac_observer_scope_error(
            PyExc_RuntimeError, "SOAC observer scope storage is already active");
    }
    if (actual_code == NULL || !PyCode_Check(actual_code)) {
        return soac_observer_scope_error(
            PyExc_TypeError, "SOAC observer scope requires actual code");
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->interp->eval_frame != NULL ||
        _PySoacSource_CodeHasObservers(tstate->interp, actual_code) ||
        _PySoacSource_HasObserverReservation(tstate->interp)) {
        return soac_observer_scope_error(
            PyExc_NotImplementedError,
            "SOAC observer or eval-frame support is unavailable");
    }
    _PyThreadStateImpl *impl = (_PyThreadStateImpl *)tstate;
    scope->_previous = impl->soac_observer_scope;
    scope->_thread = tstate;
    scope->_code = actual_code;
    impl->soac_observer_scope = scope;
    return 0;
#endif
}

int
PySoac_LeaveObserverScopeV1(PySoacObserverScopeV1 *scope, size_t scope_size)
{
#ifdef Py_GIL_DISABLED
    return soac_observer_scope_error(
        PyExc_NotImplementedError, "SOAC observer scopes require the GIL build");
#else
    if (soac_observer_scope_storage_valid(scope, scope_size) < 0) return -1;
    PyThreadState *tstate = _PyThreadState_GET();
    _PyThreadStateImpl *impl = (_PyThreadStateImpl *)tstate;
    if (scope->_thread != tstate || scope->_code == NULL ||
        impl->soac_observer_scope != scope) {
        return soac_observer_scope_error(
            PyExc_RuntimeError,
            "SOAC observer scopes must leave on their thread in stack order");
    }
    impl->soac_observer_scope = scope->_previous;
    scope->_previous = NULL;
    scope->_thread = NULL;
    scope->_code = NULL;
    return 0;
#endif
}

int
_PySoacSource_CheckCallObservers(PyThreadState *tstate)
{
    assert(tstate == _PyThreadState_GET());
    PySoacObserverScopeV1 *scope =
        ((_PyThreadStateImpl *)tstate)->soac_observer_scope;
    if (scope != NULL &&
        _PySoacSource_CodeHasObservers(tstate->interp, scope->_code)) {
        return soac_observer_scope_error(
            PyExc_NotImplementedError, "SOAC source call observer support is unavailable");
    }
    return 0;
}
