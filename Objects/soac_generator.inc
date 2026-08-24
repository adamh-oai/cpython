/* Managed native generators, coroutines, and async generators. This metadata
 * owns no execution frame: suspended roots and the sole handled-exception item
 * belong to owner. The common explicit pointer never reuses origin/finalizer. */

#include "pycore_soac_managed_code.h"

/* Included in genobject.c through soac_generator.inc. All success operations
 * below are callback-free; native debug bookkeeping may fatal on C OOM. */
typedef struct {
    uintptr_t version;
    uintptr_t phase;
    uintptr_t storage;
    uintptr_t thread;
    uintptr_t generator;
    uintptr_t owner;
    uintptr_t slots;
    uintptr_t count;
} SoacGeneratorCleanupStateV1;

enum {
    SOAC_GENERATOR_CLEANUP_LINKED = 1,
    SOAC_GENERATOR_CLEANUP_TRANSFERRED = 2,
    SOAC_GENERATOR_CLEANUP_ENDED = 3,
};

_Static_assert(sizeof(SoacGeneratorCleanupStateV1) == sizeof(PySoacGeneratorCleanupV1),
               "generator cleanup scope must have the public word count");
_Static_assert(_Alignof(SoacGeneratorCleanupStateV1) == _Alignof(PySoacGeneratorCleanupV1),
               "generator cleanup scope must have public alignment");
_Static_assert(sizeof(PySoacRefV1) == sizeof(_PyStackRef),
               "generator executable transfer must preserve the native token");

static int
soac_generator_cleanup_storage_valid(PySoacGeneratorCleanupV1 *scope, size_t size)
{
    if (scope == NULL || size != sizeof(*scope) ||
        (uintptr_t)scope % _Alignof(PySoacGeneratorCleanupV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid generator cleanup size or alignment");
        return -1;
    }
    return 0;
}

#ifndef Py_GIL_DISABLED
static int
soac_generator_cleanup_overlap(const void *left, size_t left_size,
                               const void *right, size_t right_size)
{
    uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
    return a <= b ? b - a < left_size : a - b < right_size;
}

static int
soac_generator_retirement_empty(const PySoacRefV1 *slots)
{
    for (int i = 0; i < PySoac_GENERATOR_RETIREMENT_V1_SLOTS; i++) {
        if (!PySoacRef_IsEmptyV1(slots[i])) {
            return 0;
        }
    }
    return 1;
}
#endif

int
PyGen_GetSoacGeneratorCleanupLayoutV1(
    uint32_t version, size_t layout_size, PySoacGeneratorCleanupLayoutV1 *out)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)version; (void)layout_size; (void)out;
    PyErr_SetString(PyExc_NotImplementedError,
                    "generator cleanup requires the selected 64-bit GIL build");
    return -1;
#else
    if (version != PySoac_GENERATOR_CLEANUP_ABI_VERSION || out == NULL ||
        layout_size != sizeof(*out) ||
        (uintptr_t)out % _Alignof(PySoacGeneratorCleanupLayoutV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "unsupported generator cleanup layout version or size");
        return -1;
    }
    *out = (PySoacGeneratorCleanupLayoutV1) {
        .version = PySoac_GENERATOR_CLEANUP_ABI_VERSION,
        .word_count = PySoac_GENERATOR_CLEANUP_V1_WORDS,
        .retirement_count = PySoac_GENERATOR_RETIREMENT_V1_SLOTS,
        .reserved = 0,
        .size = sizeof(PySoacGeneratorCleanupV1),
        .alignment = _Alignof(PySoacGeneratorCleanupV1),
    };
    return 0;
#endif
}

int
PyGen_BeginSoacGeneratorCleanupV1(
    PySoacGeneratorCleanupV1 *scope, size_t scope_size,
    PyObject *generator, PyObject *owner,
    PySoacRefV1 *retirement_slots, Py_ssize_t retirement_count)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)scope; (void)scope_size; (void)generator; (void)owner;
    (void)retirement_slots; (void)retirement_count;
    PyErr_SetString(PyExc_NotImplementedError,
                    "generator cleanup requires the selected 64-bit GIL build");
    return -1;
#else
    if (soac_generator_cleanup_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    for (size_t i = 0; i < PySoac_GENERATOR_CLEANUP_V1_WORDS; i++) {
        if (scope->_opaque[i] != 0) {
            PyErr_SetString(PyExc_RuntimeError,
                            "generator cleanup storage is one-use");
            return -1;
        }
    }
    if (retirement_slots == NULL ||
        retirement_count != PySoac_GENERATOR_RETIREMENT_V1_SLOTS ||
        (uintptr_t)retirement_slots % _Alignof(PySoacRefV1) != 0 ||
        soac_generator_cleanup_overlap(scope, sizeof(*scope), retirement_slots,
            PySoac_GENERATOR_RETIREMENT_V1_SLOTS * sizeof(*retirement_slots))) {
        PyErr_SetString(PyExc_ValueError,
                        "generator cleanup requires three distinct aligned native slots");
        return -1;
    }
    /* Object liveness is an unsafe caller precondition; it must be established
     * from the exact association BEFORE using a remembered Binding pointer. */
    if (generator == NULL || owner == NULL ||
        !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        PyErr_SetString(PyExc_TypeError,
                        "generator cleanup requires an actual managed association");
        return -1;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    size_t sink_size = PySoac_GENERATOR_RETIREMENT_V1_SLOTS * sizeof(*retirement_slots);
    if (soac_generator_cleanup_overlap(retirement_slots, sink_size, gen,
            offsetof(PyGenObject, gi_iframe) + offsetof(_PyInterpreterFrame, localsplus)) ||
        soac_generator_cleanup_overlap(retirement_slots, sink_size, managed, sizeof(*managed)) ||
        soac_generator_cleanup_overlap(scope, sizeof(*scope), gen,
            offsetof(PyGenObject, gi_iframe) + offsetof(_PyInterpreterFrame, localsplus)) ||
        soac_generator_cleanup_overlap(scope, sizeof(*scope), managed, sizeof(*managed))) {
        PyErr_SetString(PyExc_ValueError,
                        "generator cleanup storage aliases a native owner");
        return -1;
    }
    int live = managed->owner == owner &&
        ((managed->phase == SOAC_GEN_LIVE &&
          !FRAME_STATE_FINISHED(gen->gi_frame_state)) ||
         (managed->phase == SOAC_GEN_COMPLETING &&
          gen->gi_frame_state == FRAME_CLEARED));
    int retiring = managed->phase == SOAC_GEN_TERMINAL &&
        managed->owner == NULL && managed->retiring_owner == owner &&
        gen->gi_frame_state == FRAME_CLEARED;
    if (managed->code_mode != SOAC_GEN_TRANSFERRED_CODE || !(live || retiring) ||
        managed->cleanup != NULL || PyStackRef_IsNull(gen->gi_iframe.f_executable) ||
        !PyStackRef_IsHeapSafe(gen->gi_iframe.f_executable) ||
        !soac_generator_retirement_empty(retirement_slots)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "generator cleanup requires an unlinked transferred owner and Empty sink");
        return -1;
    }
    SoacGeneratorCleanupStateV1 state = {
        .version = PySoac_GENERATOR_CLEANUP_ABI_VERSION,
        .phase = SOAC_GENERATOR_CLEANUP_LINKED,
        .storage = (uintptr_t)scope,
        .thread = (uintptr_t)_PyThreadState_GET(),
        .generator = (uintptr_t)gen,
        .owner = (uintptr_t)owner,
        .slots = (uintptr_t)retirement_slots,
        .count = PySoac_GENERATOR_RETIREMENT_V1_SLOTS,
    };
    memcpy(scope, &state, sizeof(state));
    managed->cleanup = scope;
    return 0;
#endif
}

int
PyGen_EndSoacGeneratorCleanupV1(PySoacGeneratorCleanupV1 *scope, size_t scope_size)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)scope; (void)scope_size;
    PyErr_SetString(PyExc_NotImplementedError,
                    "generator cleanup requires the selected 64-bit GIL build");
    return -1;
#else
    if (soac_generator_cleanup_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    SoacGeneratorCleanupStateV1 state;
    memcpy(&state, scope, sizeof(state));
    if (state.version != PySoac_GENERATOR_CLEANUP_ABI_VERSION ||
        state.storage != (uintptr_t)scope ||
        state.count != PySoac_GENERATOR_RETIREMENT_V1_SLOTS || state.slots == 0 ||
        (state.phase != SOAC_GENERATOR_CLEANUP_LINKED &&
         state.phase != SOAC_GENERATOR_CLEANUP_TRANSFERRED)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "generator cleanup scope is inactive, copied, or already ended");
        return -1;
    }
    if (state.thread != (uintptr_t)_PyThreadState_GET()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "generator cleanup belongs to another thread state");
        return -1;
    }
    int transferred = state.phase == SOAC_GENERATOR_CLEANUP_TRANSFERRED;
    if (transferred) {
        if (state.generator != 0 || state.owner != 0) {
            PyErr_SetString(PyExc_RuntimeError,
                            "transferred generator cleanup retains an invalid association");
            return -1;
        }
        /* Do not dereference a dead generator or infer transfer from a pointer
         * remembered by the capsule. The scope is its own explicit witness. */
    }
    else {
        PyGenObject *gen = (PyGenObject *)state.generator;
        if (gen == NULL || !_PyGen_IsSoacManaged(gen) ||
            gen->gi_soac_managed->code_mode != SOAC_GEN_TRANSFERRED_CODE ||
            gen->gi_soac_managed->cleanup != scope ||
            !soac_generator_retirement_empty((PySoacRefV1 *)state.slots)) {
            PyErr_SetString(PyExc_RuntimeError,
                            "generator cleanup lost its live link or Empty sink");
            return -1;
        }
        gen->gi_soac_managed->cleanup = NULL;
    }
    state.phase = SOAC_GENERATOR_CLEANUP_ENDED;
    state.thread = state.generator = state.owner = state.slots = 0;
    memcpy(scope, &state, sizeof(state));
    return transferred;
#endif
}

/* Only gen_dealloc calls this, AFTER its unsuccessful resurrection check and
 * source/origin retirement, immediately before code/name/qualname cleanup. */
static void
soac_gen_transfer_destructor_tail(PyGenObject *gen)
{
#ifndef Py_GIL_DISABLED
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->cleanup == NULL) {
        return;
    }
    PySoacGeneratorCleanupV1 *scope = managed->cleanup;
    SoacGeneratorCleanupStateV1 state;
    memcpy(&state, scope, sizeof(state));
    if (managed->code_mode != SOAC_GEN_TRANSFERRED_CODE ||
        state.version != PySoac_GENERATOR_CLEANUP_ABI_VERSION ||
        state.phase != SOAC_GENERATOR_CLEANUP_LINKED ||
        state.storage != (uintptr_t)scope ||
        state.thread != (uintptr_t)_PyThreadState_GET() ||
        state.generator != (uintptr_t)gen || state.slots == 0 ||
        state.count != PySoac_GENERATOR_RETIREMENT_V1_SLOTS ||
        !soac_generator_retirement_empty((PySoacRefV1 *)state.slots) ||
        PyStackRef_IsNull(gen->gi_iframe.f_executable) ||
        gen->gi_name == NULL || gen->gi_qualname == NULL) {
        /* Corrupt committed cleanup cannot allocate an exception or unwind
         * through these still-borrowed source owners. */
        Py_FatalError("invalid committed managed generator retirement sink");
    }
    PySoacRefV1 *slots = (PySoacRefV1 *)state.slots;
    _PyStackRef executable = gen->gi_iframe.f_executable;
    PyObject *name = gen->gi_name;
    PyObject *qualname = gen->gi_qualname;
    _PyFrame_SetExecutableView(&gen->gi_iframe, NULL);
    gen->gi_iframe.f_executable = PyStackRef_NULL;
    gen->gi_name = gen->gi_qualname = NULL;
    memcpy(&slots[PySoac_GENERATOR_RETIRE_CODE], &executable, sizeof(executable));
    slots[PySoac_GENERATOR_RETIRE_NAME] = PySoacRef_FromOwnedV1(name);
    slots[PySoac_GENERATOR_RETIRE_QUALNAME] = PySoacRef_FromOwnedV1(qualname);
    managed->cleanup = NULL;
    state.phase = SOAC_GENERATOR_CLEANUP_TRANSFERRED;
    state.generator = state.owner = 0;
    memcpy(scope, &state, sizeof(state));
#else
    (void)gen;
#endif
}

static void
soac_gen_assert_no_native_activation(PyGenObject *gen)
{
    assert(_PyGen_IsSoacManaged(gen));
    assert(gen->gi_exc_state.exc_value == NULL);
    assert(gen->gi_exc_state.previous_item == NULL);
    assert(gen->gi_iframe.previous == NULL);
    assert(PyStackRef_IsNull(gen->gi_iframe.f_funcobj));
    assert(gen->gi_iframe.frame_obj == NULL);
}

static void
soac_gen_decref_preserving_error(PyObject *object)
{
    PyObject *error = PyErr_GetRaisedException();
    Py_DECREF(object);
    PyErr_SetRaisedException(error);
}

static void
soac_gen_retire(PyGenObject *gen)
{
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    soac_gen_assert_no_native_activation(gen);
    /* Terminalize before any callback or decref can expose a retained gen. */
    gen->gi_frame_state = FRAME_CLEARED;
    managed->phase = SOAC_GEN_TERMINAL;
    PyObject *owner = managed->owner;
    managed->owner = NULL;
    if (owner != NULL) {
        PyObject *error = PyErr_GetRaisedException();
        assert(managed->retiring_owner == NULL);
        managed->retiring_owner = owner;
        managed->spec.clear(owner, (PyObject *)gen);
        Py_DECREF(owner);
        managed->retiring_owner = NULL;
        PyErr_SetRaisedException(error);
    }
    soac_gen_assert_no_native_activation(gen);
}

int
PyGen_MatchesSoacOwner(PyObject *generator, PyObject *owner)
{
    if (generator == NULL || !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        return 0;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->phase == SOAC_GEN_TERMINAL || managed->owner == NULL ||
        FRAME_STATE_FINISHED(gen->gi_frame_state)) {
        PyErr_SetString(PyExc_RuntimeError, "managed generator association is terminal");
        return -1;
    }
    return managed->owner == owner;
}

int
PyGen_MarkSoacManagedTerminal(PyObject *generator, PyObject *owner)
{
    if (generator == NULL || owner == NULL ||
        !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification requires a managed generator");
        return -1;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->owner != owner) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification does not own this generator");
        return -1;
    }
    if (managed->phase == SOAC_GEN_COMPLETING &&
        gen->gi_frame_state == FRAME_CLEARED) {
        return 0;
    }
    if (managed->phase != SOAC_GEN_LIVE ||
        gen->gi_frame_state != FRAME_EXECUTING) {
        PyErr_SetString(PyExc_RuntimeError,
                        "terminal notification requires an active native step");
        return -1;
    }
    /* The compiler must do this before detaching its handled item or releasing
     * any terminal local/cell: either can run a Python finalizer. No callback,
     * allocation, or decref occurs here. The active step keeps its owner until
     * the final CLOSED result validates and performs one-way retirement. */
    managed->phase = SOAC_GEN_COMPLETING;
    gen->gi_frame_state = FRAME_CLEARED;
    return 0;
}

static PySendResult
soac_gen_step(PyGenObject *gen, int8_t previous_state,
              const PySoacGeneratorInput *input, PyObject **value)
{
    soac_gen_assert_no_native_activation(gen);
    assert(gen->gi_frame_state == FRAME_EXECUTING);
    assert(previous_state == FRAME_CREATED || FRAME_STATE_SUSPENDED(previous_state));
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    *value = NULL;
    if (managed->phase != SOAC_GEN_LIVE || managed->owner == NULL) {
        gen->gi_frame_state = previous_state;
        PyErr_SetString(PyExc_RuntimeError, "managed generator owner is not bound");
        return PYGEN_ERROR;
    }
    PyObject *owner = Py_NewRef(managed->owner);
    PyThreadState *tstate = _PyThreadState_GET();
    _PyErr_StackItem *caller_item = tstate->exc_info;
    PySoacGeneratorResult result = {
        .outcome = PYGEN_ERROR,
        .state = PySoac_GENERATOR_UNCHANGED,
        .suspension = PySoac_SUSPEND_NONE,
        .value = NULL,
    };
    managed->spec.step(owner, (PyObject *)gen, input, &result);
    /* The callback owns any materialized execution/exception activation and
     * must detach it before returning. Native never installs gi_exc_state. */
    assert(tstate->exc_info == caller_item);
    (void)caller_item;
    soac_gen_assert_no_native_activation(gen);
    int executing = managed->phase == SOAC_GEN_LIVE &&
                    gen->gi_frame_state == FRAME_EXECUTING;
    int completing = managed->phase == SOAC_GEN_COMPLETING &&
                     gen->gi_frame_state == FRAME_CLEARED;
    int valid = managed->owner == owner && (executing || completing);
    if (result.outcome == PYGEN_NEXT) {
        valid = valid && executing && result.state == PySoac_GENERATOR_SUSPENDED &&
                result.value != NULL && !PyErr_Occurred();
        if (result.suspension == PySoac_SUSPEND_DIRECT) {
            valid = valid && PyGen_CheckExact((PyObject *)gen);
        }
        else if (result.suspension == PySoac_SUSPEND_DELEGATING) {
            /* Every native family may suspend through its actual delegate. */
        }
        else if (result.suspension == PySoac_SUSPEND_ASYNC_YIELD) {
            valid = valid && PyAsyncGen_CheckExact((PyObject *)gen) &&
                    result.value != NULL &&
                    Py_IS_TYPE(result.value, &_PyAsyncGenWrappedValue_Type);
        }
        else {
            valid = 0;
        }
    }
    else if (result.outcome == PYGEN_RETURN) {
        valid = valid && result.state == PySoac_GENERATOR_CLOSED &&
                result.suspension == PySoac_SUSPEND_NONE &&
                result.value != NULL && !PyErr_Occurred() &&
                (!PyAsyncGen_CheckExact((PyObject *)gen) || result.value == Py_None);
    }
    else if (result.outcome == PYGEN_ERROR) {
        valid = valid && ((executing && result.state == PySoac_GENERATOR_UNCHANGED) ||
                           result.state == PySoac_GENERATOR_CLOSED) &&
                result.suspension == PySoac_SUSPEND_NONE &&
                result.value == NULL && PyErr_Occurred();
    }
    else {
        valid = 0;
    }
    if (!valid) {
        PyObject *error = PyErr_GetRaisedException();
        soac_gen_retire(gen);
        Py_XDECREF(result.value);
        Py_DECREF(owner);
        Py_XDECREF(error);
        PyErr_SetString(PyExc_SystemError, "invalid managed generator callback result");
        return PYGEN_ERROR;
    }
    if (result.state == PySoac_GENERATOR_CLOSED) {
        soac_gen_retire(gen);
    }
    else if (result.state == PySoac_GENERATOR_SUSPENDED) {
        gen->gi_frame_state = result.suspension == PySoac_SUSPEND_DELEGATING
                            ? FRAME_SUSPENDED_YIELD_FROM : FRAME_SUSPENDED;
    }
    else {
        gen->gi_frame_state = previous_state;
    }
    soac_gen_decref_preserving_error(owner);
    *value = result.value;
    return result.outcome;
}

static PyObject *
soac_gen_close(PyGenObject *gen, int8_t previous_state)
{
    const PySoacGeneratorInput input = {
        .operation = PySoac_GENERATOR_CLOSE,
        .close_on_genexit = 1,
    };
    PyObject *value;
    PySendResult result = soac_gen_step(gen, previous_state, &input, &value);
    if (result == PYGEN_RETURN) {
        return value;
    }
    if (result == PYGEN_NEXT) {
        Py_DECREF(value);
        PyErr_SetString(PyExc_RuntimeError, "generator ignored GeneratorExit");
        return NULL;
    }
    if (PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    return NULL;
}

static PyObject *
soac_gen_yield_from(PyGenObject *gen)
{
    soac_gen_assert_no_native_activation(gen);
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->phase != SOAC_GEN_LIVE || managed->owner == NULL ||
        gen->gi_frame_state != FRAME_SUSPENDED_YIELD_FROM) {
        Py_RETURN_NONE;
    }
    /* The fixed callback only reads its already-owned delegate; it must not
     * dispatch Python or introduce a second execution/exception activation. */
    PyObject *result = managed->spec.yield_from(managed->owner);
    soac_gen_assert_no_native_activation(gen);
    if (result == NULL && !PyErr_Occurred()) {
        PyErr_SetString(PyExc_SystemError, "managed generator delegate callback returned NULL");
    }
    return result;
}

static PyObject *
soac_gen_new_managed(PyTypeObject *type, int code_kind,
                     PyObject *function, PyCodeObject *source_code,
                     PyObject *name, PyObject *qualname, PyObject *owner,
                     const PySoacGeneratorSpec *spec,
                     PySoacRefV1 *code_owner)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "managed generators require the selected GIL build");
    return NULL;
#else
    if (function == NULL || !PyFunction_Check(function) || source_code == NULL ||
        !PyCode_Check(source_code) || owner == NULL || spec == NULL ||
        spec->abi_version != PySoac_GENERATOR_ABI_VERSION || spec->reserved != 0 ||
        spec->bind == NULL || spec->step == NULL ||
        spec->yield_from == NULL || spec->clear == NULL ||
        (name != NULL && !PyUnicode_Check(name)) ||
        (qualname != NULL && !PyUnicode_Check(qualname))) {
        PyErr_SetString(PyExc_TypeError, "invalid managed generator construction specification");
        return NULL;
    }
    if ((source_code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
        != code_kind) {
        PyErr_SetString(PyExc_TypeError,
                        "managed generator code does not match the native family");
        return NULL;
    }
    if (type == &PyCoro_Type &&
        _PyThreadState_GET()->coroutine_origin_tracking_depth != 0) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "optimized coroutine origin tracking is not supported");
        return NULL;
    }
    PyFunctionObject *func = (PyFunctionObject *)function;
    PyObject *public_name = name == NULL ? func->func_name : name;
    PyObject *public_qualname = qualname == NULL ? func->func_qualname : qualname;
    if (public_name == NULL || public_qualname == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "managed generator function metadata is terminal");
        return NULL;
    }
    /* Copy the caller's ABI before allocation, never retaining its address. */
    PySoacGeneratorSpec copied_spec = *spec;
    struct _PySoacManagedGenerator *managed = PyMem_Calloc(1, sizeof(*managed));
    if (managed == NULL) {
        return PyErr_NoMemory();
    }
    /* No locals/operand slots: the fixed frame header holds only the code edge
     * required by gi_code. It is never an evaluation or introspection frame. */
    PyGenObject *gen = PyObject_GC_NewVar(PyGenObject, type, 0);
    if (gen == NULL) {
        PyMem_Free(managed);
        return NULL;
    }
    gen->gi_soac_managed = managed;
    gen->gi_origin_or_finalizer = NULL;
    gen->gi_weakreflist = NULL;
    gen->gi_name = Py_NewRef(public_name);
    gen->gi_qualname = Py_NewRef(public_qualname);
    gen->gi_exc_state.exc_value = NULL;
    gen->gi_exc_state.previous_item = NULL;
    gen->gi_hooks_inited = 0;
    gen->gi_closed = 0;
    gen->gi_running_async = 0;
    gen->gi_frame_state = FRAME_CREATED;
    memset(&gen->gi_iframe, 0, offsetof(_PyInterpreterFrame, localsplus));
    if (code_owner == NULL) {
        gen->gi_iframe.f_executable = PyStackRef_FromPyObjectNew((PyObject *)source_code);
        managed->code_mode = SOAC_GEN_LEGACY_CODE;
    }
    else {
        /* All recoverable allocation precedes the commit; bind may
         * observe the fully initialized object only after this move. */
        PySoacRefV1 moved = PySoacRef_TakeV1(code_owner);
        memcpy(&gen->gi_iframe.f_executable, &moved, sizeof(moved));
        managed->code_mode = SOAC_GEN_TRANSFERRED_CODE;
    }
    _PyFrame_SetExecutableView(&gen->gi_iframe, (PyObject *)source_code);
    gen->gi_iframe.f_funcobj = PyStackRef_NULL;
    gen->gi_iframe.owner = FRAME_OWNED_BY_GENERATOR;
    managed->spec = copied_spec;
    managed->owner = Py_NewRef(owner);
    managed->phase = SOAC_GEN_BINDING;
    PyObject *binding_owner = Py_NewRef(owner);
    int result = copied_spec.bind(binding_owner, (PyObject *)gen, function, source_code);
    soac_gen_assert_no_native_activation(gen);
    if (result != 0 || PyErr_Occurred() || managed->phase != SOAC_GEN_BINDING ||
        managed->owner != binding_owner || gen->gi_frame_state != FRAME_CREATED) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "managed generator binding failed without an error");
        }
        soac_gen_retire(gen);
        soac_gen_decref_preserving_error(binding_owner);
        /* A bind observer may have created a weak or strong reference. Use the
         * normal, fully initialized destructor, including weakref clearing. */
        _PyObject_GC_TRACK(gen);
        soac_gen_decref_preserving_error((PyObject *)gen);
        return NULL;
    }
    managed->phase = SOAC_GEN_LIVE;
    Py_DECREF(binding_owner);
    _PyObject_GC_TRACK(gen);
    return (PyObject *)gen;
#endif
}

PyObject *
PyGen_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                    PyObject *name, PyObject *qualname, PyObject *owner,
                    const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyGen_Type, CO_GENERATOR, function, source_code,
                                name, qualname, owner, spec, NULL);
}

PyObject *
PyCoro_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                     PyObject *name, PyObject *qualname, PyObject *owner,
                     const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyCoro_Type, CO_COROUTINE, function, source_code,
                                name, qualname, owner, spec, NULL);
}

PyObject *
PyAsyncGen_NewSoacManaged(PyObject *function, PyCodeObject *source_code,
                         PyObject *name, PyObject *qualname, PyObject *owner,
                         const PySoacGeneratorSpec *spec)
{
    return soac_gen_new_managed(&PyAsyncGen_Type, CO_ASYNC_GENERATOR, function,
                                source_code, name, qualname, owner, spec, NULL);
}

PyObject *
PySoac_NewManagedWithReferenceV1(
    PyObject *function, PySoacRefV1 *code_owner,
    PyObject *name, PyObject *qualname, PyObject *owner,
    const PySoacGeneratorSpec *spec)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)function; (void)code_owner; (void)name; (void)qualname;
    (void)owner; (void)spec;
    PyErr_SetString(PyExc_NotImplementedError,
                    "managed references require the selected 64-bit GIL build");
    return NULL;
#else
    if (code_owner == NULL ||
        (uintptr_t)code_owner % _Alignof(PySoacRefV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "managed code requires an aligned native owner slot");
        return NULL;
    }
    PyObject *object = PySoacRef_ObjectViewV1(*code_owner);
    if (object == NULL || !PyCode_Check(object)) {
        PyErr_SetString(PyExc_TypeError,
                        "managed code requires an actual code object");
        return NULL;
    }
    if (!PySoacRef_IsHeapSafeV1(*code_owner)) {
        PyErr_SetString(PyExc_ValueError,
                        "managed code must already be heap safe");
        return NULL;
    }
    PyCodeObject *code = (PyCodeObject *)object;
    int kind = code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR);
    PyTypeObject *type;
    switch (kind) {
        case CO_GENERATOR: type = &PyGen_Type; break;
        case CO_COROUTINE: type = &PyCoro_Type; break;
        case CO_ASYNC_GENERATOR: type = &PyAsyncGen_Type; break;
        default:
            PyErr_SetString(PyExc_TypeError,
                            "managed code must select one native suspended family");
            return NULL;
    }
    return soac_gen_new_managed(type, kind, function, code, name, qualname,
                                owner, spec, code_owner);
#endif
}

PyObject *
PyAsyncGen_WrapSoacYield(PyObject *value)
{
    if (value == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return _PyAsyncGenValueWrapperNew(_PyThreadState_GET(), value);
}
