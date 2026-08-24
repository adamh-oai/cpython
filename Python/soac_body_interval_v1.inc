/* Native synchronous source-body interval. Included once after ceval_macros.h
 * so recursion and initial periodic duties use the native implementation.
 * This unit neither evaluates bytecode nor authenticates a source artifact.
 */
#include "pycore_soac_body_interval.h"

typedef struct {
    SoacLifetimeScopeStateV1 scope;
    uintptr_t version;
    uintptr_t state;
    uintptr_t charged;
    uintptr_t code;
    intptr_t entry_offset;
    intptr_t entry_lineno;
} SoacSyncBodyStateV1;

enum {
    SOAC_SYNC_BODY_BEFORE_RESUME = 1,
    SOAC_SYNC_BODY_RESUMED = 2,
    SOAC_SYNC_BODY_ENDED = 3,
};

_Static_assert(sizeof(SoacSyncBodyStateV1) == sizeof(PySoacSyncBodyIntervalV1),
               "native synchronous interval has exactly twelve words");
_Static_assert(_Alignof(SoacSyncBodyStateV1) == _Alignof(PySoacSyncBodyIntervalV1),
               "native synchronous interval alignment must match");
_Static_assert(offsetof(SoacSyncBodyStateV1, scope) == 0,
               "one native scope is the interval's physical prefix");

int
_PySoacBody_CodeHasObservers(PyInterpreterState *interp, PyCodeObject *code)
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
_PySoacBody_HasProtectedIntervalThreadListLocked(
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
                (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SYNC_BODY) &&
                (code == NULL || _PyFrame_GetCode(frame) == code)) {
                return 1;
            }
        }
    }
    return 0;
#endif
}

int
_PySoacBody_HasProtectedInterval(PyInterpreterState *interp, PyCodeObject *code)
{
#ifdef Py_GIL_DISABLED
    (void)interp; (void)code;
    return 0;
#else
    HEAD_LOCK(interp->runtime);
    int result = _PySoacBody_HasProtectedIntervalThreadListLocked(interp, code);
    HEAD_UNLOCK(interp->runtime);
    return result;
#endif
}

int
_PySoacBody_HasObserverReservation(PyInterpreterState *interp)
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
_PySoacBody_RestartWouldObserve(PyInterpreterState *interp)
{
    int result = 0;
#ifndef Py_GIL_DISABLED
    _Py_FOR_EACH_TSTATE_BEGIN(interp, tstate) {
        for (_PyInterpreterFrame *frame = tstate->current_frame;
             frame != NULL; frame = frame->previous) {
            if (frame->owner == FRAME_OWNED_BY_SOAC_ACTIVE &&
                (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SYNC_BODY) &&
                _PySoacBody_CodeHasObservers(interp, _PyFrame_GetCode(frame))) {
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
_PySoacBody_BeginObserverReservation(PyThreadState *tstate)
{
    if (PyErr_Occurred()) return -1;
    if (tstate != _PyThreadState_GET()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "observer reservation requires the current thread state");
        return -1;
    }
#ifndef Py_GIL_DISABLED
    if (_PySoacBody_HasProtectedInterval(tstate->interp, NULL)) {
        return _PySoacBody_ResolveObserverStatus(_Py_SOAC_OBSERVER_REFUSED);
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
_PySoacBody_EndObserverReservation(PyThreadState *tstate)
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
soac_body_range(const void *pointer, size_t size, size_t alignment)
{
    uintptr_t start = (uintptr_t)pointer;
    return pointer != NULL && start % alignment == 0 && size <= UINTPTR_MAX - start;
}

static int
soac_body_overlap(const void *left, size_t left_size,
                  const void *right, size_t right_size)
{
    uintptr_t l = (uintptr_t)left, r = (uintptr_t)right;
    return l < r + right_size && r < l + left_size;
}

static int
soac_body_storage_valid(PySoacSyncBodyIntervalV1 *interval, size_t size)
{
    if (size != sizeof(*interval) ||
        !soac_body_range(interval, size, _Alignof(PySoacSyncBodyIntervalV1))) {
        PyErr_SetString(PyExc_ValueError, "invalid native body interval storage");
        return -1;
    }
    return 0;
}

int
PyFrame_GetSoacSyncBodyLayoutV1(
    uint32_t version, size_t out_size, PySoacSyncBodyLayoutV1 *out)
{
    if (PyErr_Occurred()) return -1;
    if (version != Py_SOAC_SYNC_BODY_ABI_V1 || out_size != sizeof(*out) ||
        !soac_body_range(out, out_size, _Alignof(PySoacSyncBodyLayoutV1))) {
        PyErr_SetString(PyExc_ValueError, "invalid native body layout query");
        return -1;
    }
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    PyErr_SetString(PyExc_NotImplementedError,
                    "native body intervals require the selected 64-bit GIL build");
    return -1;
#else
    uint32_t capabilities = Py_SOAC_SYNC_BODY_GIL_64 |
        Py_SOAC_SYNC_BODY_NATIVE_RECURSION | Py_SOAC_SYNC_BODY_INITIAL_RESUME |
        Py_SOAC_SYNC_BODY_NO_SOURCE_OBSERVERS | Py_SOAC_SYNC_BODY_EMPTY_PREFIX_ONLY;
#ifdef Py_STACKREF_DEBUG
    capabilities |= Py_SOAC_SYNC_BODY_DEBUG_HANDLES;
#endif
    *out = (PySoacSyncBodyLayoutV1) {
        .abi_version = Py_SOAC_SYNC_BODY_ABI_V1,
        .word_count = Py_SOAC_SYNC_BODY_V1_WORDS,
        .interval_size = sizeof(PySoacSyncBodyIntervalV1),
        .interval_alignment = _Alignof(PySoacSyncBodyIntervalV1),
        .capabilities = capabilities,
        .reserved = 0,
    };
    return 0;
#endif
}

#if !defined(Py_GIL_DISABLED) && SIZEOF_VOID_P == 8
static int
soac_body_validate_live(PySoacSyncBodyIntervalV1 *interval, size_t size,
                        SoacSyncBodyStateV1 *state)
{
    if (soac_body_storage_valid(interval, size) < 0) return -1;
    memcpy(state, interval, sizeof(*state));
    if (state->version != Py_SOAC_SYNC_BODY_ABI_V1 || state->charged != 1 ||
        (state->state != SOAC_SYNC_BODY_BEFORE_RESUME &&
         state->state != SOAC_SYNC_BODY_RESUMED)) {
        PyErr_SetString(PyExc_RuntimeError, "native body interval is not live");
        return -1;
    }
    uint16_t flags = SOAC_LIFETIME_SYNC_BODY;
    if (state->state == SOAC_SYNC_BODY_RESUMED) flags |= SOAC_LIFETIME_SYNC_RESUMED;
    if (_PyFrame_ValidateSoacScopeV1(&state->scope, (uintptr_t)interval, flags) < 0) {
        return -1;
    }
    /* The scope check establishes current-thread/LIFO before saved addresses
     * are dereferenced. The captured code is never reacquired from func_code. */
    _PyInterpreterFrame *frame = (_PyInterpreterFrame *)state->scope.frame;
    if ((uintptr_t)_PyFrame_GetCode(frame) != state->code) {
        PyErr_SetString(PyExc_RuntimeError, "native body code primary changed");
        return -1;
    }
    return 0;
}

static void
soac_body_end_validated(PySoacSyncBodyIntervalV1 *interval, SoacSyncBodyStateV1 *state)
{
    PyThreadState *tstate = (PyThreadState *)state->scope.thread;
    /* A callback may change the recursion limit; refund the CURRENT counter. */
    _Py_LeaveRecursiveCallPy(tstate);
    _Py_LeaveRecursiveCallTstate(tstate);
    state->charged = 0;
    state->state = SOAC_SYNC_BODY_ENDED;
    _PyFrame_UnlinkSoacScopeV1(&state->scope, interval, 1);
    memcpy(interval, state, sizeof(*state));
}

static int
soac_body_lltrace_requested(PyObject *globals)
{
#ifdef Py_DEBUG
    /* Same native entry lookup/error behavior, without asking the lifetime
     * frame to pretend it can execute the LLTRACE instruction protocol. */
    int contains = PyDict_Contains(globals, &_Py_ID(__lltrace__));
    if (contains < 0) {
        PyErr_Clear();
        return 0;
    }
    int level = contains * 5;
    if (level == 0) {
        char *value = Py_GETENV("PYTHON_LLTRACE");
        if (value != NULL && *value >= '0') level = *value - '0';
    }
    return level != 0;
#else
    (void)globals;
    return 0;
#endif
}
#endif

int
PyFrame_BeginSoacSyncBodyV1(
    PySoacSyncBodyIntervalV1 *interval, size_t interval_size,
    PyFrameObject *object, const PySoacSourceCallInfoV1 *captured, size_t captured_size,
    PySoacSyncBodyEntrySiteV1 *entry_site, size_t entry_site_size)
{
    if (PyErr_Occurred()) return -1;
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)interval; (void)interval_size; (void)object;
    (void)captured; (void)captured_size; (void)entry_site; (void)entry_site_size;
    PyErr_SetString(PyExc_NotImplementedError,
                    "native body intervals require the selected 64-bit GIL build");
    return -1;
#else
    if (soac_body_storage_valid(interval, interval_size) < 0) return -1;
    if (captured_size != sizeof(*captured) || entry_site_size != sizeof(*entry_site) ||
        !soac_body_range(captured, captured_size, _Alignof(PySoacSourceCallInfoV1)) ||
        !soac_body_range(entry_site, entry_site_size, _Alignof(PySoacSyncBodyEntrySiteV1)) ||
        soac_body_overlap(interval, interval_size, captured, captured_size) ||
        soac_body_overlap(interval, interval_size, entry_site, entry_site_size) ||
        soac_body_overlap(captured, captured_size, entry_site, entry_site_size)) {
        PyErr_SetString(PyExc_ValueError, "invalid or aliased native body inputs");
        return -1;
    }
    PySoacSourceCallInfoV1 info;
    memcpy(&info, captured, sizeof(info));
    if (object == NULL || !PyFrame_Check(object) || info.reserved != 0 ||
        info.kind > Py_SOAC_CALL_VM_EXPANDED_V1 || info.function == NULL ||
        !PyFunction_Check(info.function) || info.code == NULL || !PyCode_Check(info.code) ||
        info.globals == NULL || !PyDict_Check(info.globals) || info.builtins == NULL) {
        PyErr_SetString(PyExc_ValueError, "invalid captured native source call");
        return -1;
    }
    PyCodeObject *code = info.code;
    _PyInterpreterFrame *frame = object->f_frame;
    if (frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE || _PyFrame_GetCode(frame) != code ||
        (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_BORROWED_CODE) ||
        info.parameter_count != _PyCode_NumFrameParameters(code) ||
        info.supplied_count != (Py_ssize_t)code->co_argcount + code->co_kwonlyargcount) {
        PyErr_SetString(PyExc_ValueError, "native body must use its captured binding and code primary");
        return -1;
    }
    size_t frame_bytes = offsetof(PyFrameObject, _f_frame_data) +
        offsetof(_PyInterpreterFrame, localsplus) +
        ((size_t)code->co_nlocalsplus + (size_t)code->co_stacksize) * sizeof(_PyStackRef);
    size_t code_bytes = (size_t)Py_TYPE(code)->tp_basicsize +
        (size_t)Py_SIZE(code) * (size_t)Py_TYPE(code)->tp_itemsize;
    if (soac_body_overlap(interval, interval_size, object, frame_bytes) ||
        soac_body_overlap(entry_site, entry_site_size, object, frame_bytes) ||
        soac_body_overlap(interval, interval_size, code, code_bytes) ||
        soac_body_overlap(entry_site, entry_site_size, code, code_bytes) ||
        soac_body_overlap(interval, interval_size, info.function, sizeof(PyFunctionObject)) ||
        soac_body_overlap(entry_site, entry_site_size, info.function, sizeof(PyFunctionObject))) {
        PyErr_SetString(PyExc_ValueError, "native body outputs alias Python source owners");
        return -1;
    }
    for (size_t i = 0; i < Py_SOAC_SYNC_BODY_V1_WORDS; i++) {
        if (interval->_opaque[i] != 0) {
            PyErr_SetString(PyExc_RuntimeError, "native body interval storage is one-use");
            return -1;
        }
    }
    if (!(code->co_flags & CO_OPTIMIZED) ||
        (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) ||
        code->co_ncellvars || code->co_nfreevars || code->_co_firsttraceable != 0 ||
        Py_SIZE(code) == 0) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "native body V1 requires synchronous optimized code with an empty entry prefix");
        return -1;
    }
    _Py_CODEUNIT entry = _PyCode_CODE(code)[code->_co_firsttraceable];
    if ((entry.op.code != RESUME && entry.op.code != RESUME_CHECK &&
         entry.op.code != INSTRUMENTED_RESUME) ||
        (entry.op.arg & RESUME_OPARG_LOCATION_MASK) != RESUME_AT_FUNC_START) {
        PyErr_SetString(PyExc_ValueError, "captured native code has no initial RESUME");
        return -1;
    }
    int offset = code->_co_firsttraceable * (int)sizeof(_Py_CODEUNIT);
    int line = PyCode_Addr2Line(code, offset);
    if (line <= 0) {
        PyErr_SetString(PyExc_ValueError, "captured native RESUME has no source location");
        return -1;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->interp->eval_frame != NULL ||
        _PySoacBody_CodeHasObservers(tstate->interp, code) ||
        _PySoacBody_HasObserverReservation(tstate->interp)) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "native source-body observer or eval-frame support is unavailable");
        return -1;
    }
    SoacSyncBodyStateV1 state = {
        .version = Py_SOAC_SYNC_BODY_ABI_V1,
        .state = SOAC_SYNC_BODY_BEFORE_RESUME,
        .charged = 1,
        .code = (uintptr_t)code,
        .entry_offset = offset,
        .entry_lineno = line,
    };
    if (_PyFrame_PrepareSoacScopeV1(&state.scope, (uintptr_t)interval, object,
            info.function, info.globals, info.builtins) < 0) return -1;
    if (_Py_EnterRecursiveCallTstate(tstate, "")) return -1;

    memcpy(interval, &state, sizeof(state));
    _PyFrame_LinkSoacScopeV1(&state.scope, interval, info.function,
                           info.globals, info.builtins, SOAC_LIFETIME_SYNC_BODY);
    if (_Py_EnterRecursivePy(tstate)) {
        /* Native start_frame failure has no callee traceback. No source
         * primary is cleared here; the installed owner still needs Finish. */
        soac_body_end_validated(interval, &state);
        return -1;
    }
    if (soac_body_lltrace_requested(info.globals)) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "native source-body LLTRACE execution is unavailable");
        soac_body_end_validated(interval, &state);
        return -1;
    }
    *entry_site = (PySoacSyncBodyEntrySiteV1) {
        .abi_version = Py_SOAC_SYNC_BODY_ABI_V1,
        .reserved = 0,
        .code = code,
        .instruction_offset_bytes = offset,
        .source_lineno = line,
    };
    return 0;
#endif
}

int
PyFrame_ResumeSoacSyncBodyV1(PySoacSyncBodyIntervalV1 *interval, size_t interval_size)
{
    if (PyErr_Occurred()) return -1;
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)interval; (void)interval_size;
    PyErr_SetString(PyExc_NotImplementedError,
                    "native body intervals require the selected 64-bit GIL build");
    return -1;
#else
    SoacSyncBodyStateV1 state;
    if (soac_body_validate_live(interval, interval_size, &state) < 0) return -1;
    if (state.state != SOAC_SYNC_BODY_BEFORE_RESUME) {
        PyErr_SetString(PyExc_RuntimeError, "native initial RESUME already completed");
        return -1;
    }
    state.state = SOAC_SYNC_BODY_RESUMED;
    _PyInterpreterFrame *frame = (_PyInterpreterFrame *)state.scope.frame;
    frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_SYNC_RESUMED;
    memcpy(interval, &state, sizeof(state));

    PyThreadState *tstate = _PyThreadState_GET();
    PyCodeObject *code = (PyCodeObject *)state.code;
    if (tstate->tracing == 0) {
        uintptr_t version = _Py_atomic_load_uintptr_relaxed(&tstate->eval_breaker) &
            ~_PY_EVAL_EVENTS_MASK;
        if (FT_ATOMIC_LOAD_UINTPTR_ACQUIRE(code->_co_instrumentation_version) != version &&
            _Py_Instrument(code, tstate->interp) < 0) return -1;
    }
    /* No quickening, source bytecode execution, fake PC or event omission. */
    return check_periodics(tstate);
#endif
}

int
PyFrame_EndSoacSyncBodyV1(PySoacSyncBodyIntervalV1 *interval, size_t interval_size)
{
    PyObject *saved = PyErr_GetRaisedException();
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)interval; (void)interval_size;
    PyErr_SetString(PyExc_NotImplementedError,
                    "native body intervals require the selected 64-bit GIL build");
    int status = -1;
#else
    SoacSyncBodyStateV1 state;
    int status = soac_body_validate_live(interval, interval_size, &state);
    if (status == 0) {
        /* Restore before native terminal parent retention; that helper saves
         * and restores the same pending exception around possible OOM. */
        PyErr_SetRaisedException(saved);
        soac_body_end_validated(interval, &state);
        return 0;
    }
#endif
    if (saved != NULL) {
        /* Invalid API rejection must not replace a caller's pending error. */
        PyErr_SetRaisedException(saved);
    }
    return status;
}
