/* A lazy, exact PyFrameObject that owns source lifetimes, not an execution
 * activation. ACTIVE slots are deliberately empty and are never exposed as
 * Python locals. The source activation owns its sole primary frame reference.
 * At terminal retirement, retained tracebacks take the then-current owners.
 */
#include "pycore_gc.h"  // _PyGC_PREV_MASK_COLLECTING
#include "pycore_traceback.h"  // _PyTraceBack_HereSoacLifetime
#include "native_lifetime_reference_v1.h"  // ignored native22 ABI layer
#include "pycore_soac_outgoing_v1.h"  // read-only current-scope validation
#include "pycore_soac_managed_code.h"  // shared private managed association

static int
soac_frame_unavailable(const char *operation)
{
    PyErr_Format(PyExc_NotImplementedError,
                 "%s is unavailable for an optimized source-lifetime frame",
                 operation);
    return -1;
}

static int
soac_frame_check_inspection(PyFrameObject *frame, const char *operation)
{
    if (_PyFrame_IsSoacLifetime(frame->f_frame)) {
        return soac_frame_unavailable(operation);
    }
    return 0;
}

static int
soac_frame_setattro(PyObject *object, PyObject *name, PyObject *value)
{
    if (soac_frame_check_inspection((PyFrameObject *)object,
                                   "frame mutation/tracing") < 0) {
        return -1;
    }
    return PyObject_GenericSetAttr(object, name, value);
}

PyFrameObject *
_PyFrame_GetSoacLifetimeObject(_PyInterpreterFrame *frame)
{
    assert(_PyFrame_IsSoacLifetime(frame));
    PyFrameObject *object = (PyFrameObject *)
        ((char *)frame - offsetof(PyFrameObject, _f_frame_data));
    assert(object->f_frame == frame);
    return object;
}

/* Shared allocation for legacy and native-reference lifetime constructors.
 * The native-reference path moves the actual code-primary token.
 * Its public declarations are in native_lifetime_reference_v1.h.
 */
_Static_assert(sizeof(PySoacRefV1) == sizeof(_PyStackRef),
               "lifetime code handoff must move the exact native token");
_Static_assert(_Alignof(PySoacRefV1) == _Alignof(_PyStackRef),
               "lifetime code handoff requires native token alignment");

#ifndef Py_GIL_DISABLED
static PyFrameObject *
soac_lifetime_new_code_primary(PyCodeObject *source_code,
                               PySoacRefV1 *code_owner,
                               const _PyStackRef *borrowed_code)
{
    /* _PyFrame_New_NoTrack borrows code only and does not acquire a code edge.
     * As in the legacy path, allocation cannot call Python or collect now. */
    PyObject *error = PyErr_GetRaisedException();
    PyFrameObject *object = _PyFrame_New_NoTrack(source_code);
    if (object == NULL) {
        PyObject *allocation_error = PyErr_GetRaisedException();
        Py_XDECREF(error);
        PyErr_SetRaisedException(allocation_error);
        return NULL;
    }
    _PyInterpreterFrame *frame = (_PyInterpreterFrame *)object->_f_frame_data;
    memset(frame, 0, offsetof(_PyInterpreterFrame, localsplus));
    frame->f_executable = PyStackRef_NULL;
    frame->f_funcobj = PyStackRef_NULL;
    frame->owner = FRAME_OWNED_BY_SOAC_ACTIVE;
    frame->stackpointer = frame->localsplus;
    int slots = source_code->co_nlocalsplus + source_code->co_stacksize;
    for (int i = 0; i < slots; i++) {
        frame->localsplus[i] = PyStackRef_NULL;
    }
    object->f_frame = frame;

    /* All validation and recoverable allocation is complete. No Python can
     * observe this still-untracked frame between Empty publication and move. */
    assert(code_owner == NULL || borrowed_code == NULL);
    if (borrowed_code != NULL) {
        frame->f_executable = PyStackRef_Borrow(*borrowed_code);
        frame->soac_lifetime_owned_environment = SOAC_LIFETIME_BORROWED_CODE;
    }
    else if (code_owner == NULL) {
        frame->f_executable = PyStackRef_FromPyObjectNew((PyObject *)source_code);
    }
    else {
        PySoacRefV1 moved = PySoacRef_TakeV1(code_owner);
        /* Transport only: moved is not another semantic owner and is never
         * independently closed. No decoding or tag-dependent policy here. */
        memcpy(&frame->f_executable, &moved, sizeof(frame->f_executable));
    }
    _PyObject_GC_TRACK(object);
    PyErr_SetRaisedException(error);
    return object;
}
#endif

PyFrameObject *
PyFrame_NewSoacLifetime(PyCodeObject *source_code)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime frames require the selected GIL build");
    return NULL;
#else
    if (source_code == NULL || !PyCode_Check(source_code)) {
        PyErr_SetString(PyExc_TypeError,
                        "source-lifetime frame requires an actual code object");
        return NULL;
    }
    return soac_lifetime_new_code_primary(source_code, NULL, NULL);
#endif
}

PyFrameObject *
PyFrame_NewSoacLifetimeWithReferenceV1(PySoacRefV1 *code_owner)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)code_owner;
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime references require the selected 64-bit GIL build");
    return NULL;
#else
    if (code_owner == NULL ||
        (uintptr_t)code_owner % _Alignof(PySoacRefV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime code owner requires an aligned native slot");
        return NULL;
    }
    PyObject *code = PySoacRef_ObjectViewV1(*code_owner);
    if (code == NULL || !PyCode_Check(code)) {
        PyErr_SetString(PyExc_TypeError,
                        "source-lifetime code owner requires an actual code object");
        return NULL;
    }
    if (!PySoacRef_IsHeapSafeV1(*code_owner)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime code owner must already be heap safe");
        return NULL;
    }
    return soac_lifetime_new_code_primary((PyCodeObject *)code, code_owner, NULL);
#endif
}


PyFrameObject *
PyFrame_NewSoacManagedLifetimeV1(PyObject *generator, PyObject *owner)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)generator; (void)owner;
    PyErr_SetString(PyExc_NotImplementedError,
                    "managed lifetime references require the selected 64-bit GIL build");
    return NULL;
#else
    if (generator == NULL || owner == NULL ||
        !_PyGen_IsSoacManaged((PyGenObject *)generator)) {
        PyErr_SetString(PyExc_TypeError,
                        "managed lifetime requires an actual managed generator");
        return NULL;
    }
    PyGenObject *gen = (PyGenObject *)generator;
    struct _PySoacManagedGenerator *managed = gen->gi_soac_managed;
    if (managed->code_mode != SOAC_GEN_TRANSFERRED_CODE ||
        managed->owner != owner || managed->phase != SOAC_GEN_LIVE ||
        gen->gi_frame_state != FRAME_EXECUTING || managed->cleanup != NULL ||
        !PyStackRef_IsHeapSafe(gen->gi_iframe.f_executable)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "managed lifetime requires the actual executing transferred owner");
        return NULL;
    }
    /* Exact native support handle, not an owning pointer conversion. The
     * allocator completes all recoverable work before creating the Borrow. */
    return soac_lifetime_new_code_primary(
        _PyFrame_GetCode(&gen->gi_iframe), NULL, &gen->gi_iframe.f_executable);
#endif
}

int
PyFrame_AddSoacTraceback(PyFrameObject *object,
                         int instruction_offset, int source_lineno)
{
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE) {
        PyErr_SetString(PyExc_RuntimeError,
                        "traceback attachment requires an active source-lifetime frame");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    if ((source_lineno <= 0 &&
         (source_lineno != -1 || instruction_offset != -1)) ||
        instruction_offset < -1 ||
        (instruction_offset >= 0 &&
         (instruction_offset % sizeof(_Py_CODEUNIT) != 0 ||
          instruction_offset >= _PyCode_NBYTES(code) ||
          PyBytes_GET_SIZE(code->co_linetable) == 0 ||
          PyCode_Addr2Line(code, instruction_offset) != source_lineno))) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid source-lifetime traceback position");
        return -1;
    }
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime traceback requires a raised exception");
        return -1;
    }
    /* The site belongs to this traceback, never a mutable pseudo-PC on
     * the lifetime frame. Generic FromFrame requests remain unknown. */
    return _PyTraceBack_HereSoacLifetime(object, instruction_offset, source_lineno);
}

/* One finish implementation, two explicitly typed input row forms. The enum
 * is selected by the API wrapper, never inferred from bits, names or objects.
 */
typedef enum {
    SOAC_LIFETIME_OBJECT_ROWS,
    SOAC_LIFETIME_REFERENCE_ROWS,
} SoacLifetimeOwnerRowKind;

typedef struct {
    SoacLifetimeOwnerRowKind kind;
    PyObject *const *objects;
    const PySoacRefV1 *const *references;
} SoacLifetimeOwnerRows;

#ifndef Py_GIL_DISABLED
static PyObject *
soac_lifetime_owner_view(const SoacLifetimeOwnerRows *rows, Py_ssize_t index)
{
    if (rows->kind == SOAC_LIFETIME_OBJECT_ROWS) {
        return rows->objects[index];
    }
    const PySoacRefV1 *slot = rows->references[index];
    return slot == NULL ? NULL : PySoacRef_ObjectViewV1(*slot);
}
#endif

static int
soac_lifetime_finish_owner_rows(
    PyFrameObject *object, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins,
    PyObject *source_namespace, const SoacLifetimeOwnerRows *rows, Py_ssize_t count)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime frames require the selected GIL build");
    return -1;
#else
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        source_function == NULL || !PyFunction_Check(source_function)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime finish requires an active frame and function");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    PyCodeObject *code = _PyFrame_GetCode(frame);
    PyFunctionObject *function = (PyFunctionObject *)source_function;
    if (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-lifetime finish requires leaving its parent scope");
        return -1;
    }
    if (frame->soac_lifetime_owned_environment &
        ~(SOAC_LIFETIME_BORROWED_CODE | SOAC_LIFETIME_SCOPE_TERMINAL)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "invalid active source-lifetime ownership flags");
        return -1;
    }
    if ((code->co_flags & CO_OPTIMIZED) ? source_namespace != NULL
        : source_namespace == NULL || !PyMapping_Check(source_namespace)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime namespace does not match the original code scope");
        return -1;
    }
    const void *owner_array = rows->kind == SOAC_LIFETIME_OBJECT_ROWS
        ? (const void *)rows->objects : (const void *)rows->references;
    if (count != code->co_nlocalsplus || (count != 0 && owner_array == NULL)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime owners do not match the original code layout");
        return -1;
    }
    if (rows->kind == SOAC_LIFETIME_OBJECT_ROWS &&
        (captured_globals == NULL || captured_builtins == NULL)) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime finish requires the captured function environment");
        return -1;
    }
    if (rows->kind == SOAC_LIFETIME_REFERENCE_ROWS && count != 0 &&
        (uintptr_t)rows->references % _Alignof(const PySoacRefV1 *) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "source-lifetime reference rows require an aligned pointer array");
        return -1;
    }
    for (Py_ssize_t i = 0; i < count; i++) {
        if (rows->kind == SOAC_LIFETIME_REFERENCE_ROWS &&
            rows->references[i] != NULL &&
            (uintptr_t)rows->references[i] % _Alignof(PySoacRefV1) != 0) {
            PyErr_SetString(PyExc_ValueError,
                            "source-lifetime reference row requires an aligned native slot");
            return -1;
        }
        PyObject *value = soac_lifetime_owner_view(rows, i);
        _PyLocals_Kind kind = _PyLocals_GetKind(code->co_localspluskinds, (int)i);
        if ((kind & (CO_FAST_CELL | CO_FAST_FREE)) &&
            value != NULL && !PyCell_Check(value)) {
            PyErr_SetString(PyExc_TypeError,
                            "source-lifetime cell slots require original cell objects");
            return -1;
        }
    }
    /* Nothing below allocates a Python object, dispatches Python, decrefs,
     * or recoverably fails. Native STACKREF_DEBUG bookkeeping may use its
     * ordinary fatal-on-OOM C allocation. Decide before adding any parked
     * references, including a self-local. Primary native tokens are unchanged. */
    int retained = !_PyObject_IsUniquelyReferenced((PyObject *)object);
    if (rows->kind == SOAC_LIFETIME_REFERENCE_ROWS) {
        /* The native interval's actual function token, not a cached map
         * pointer, supported these borrowed views. GC may already have run
         * func_clear, even while that function remains allocated. Unpublish
         * before every commit path, including unique and never-entered frames;
         * never read or acquire a captured map after its support may retire.
         * Lifetime map introspection (Python and C) already fails explicitly. */
        frame->f_globals = NULL;
        frame->f_builtins = NULL;
    }
    uint16_t code_role = frame->soac_lifetime_owned_environment &
                         SOAC_LIFETIME_BORROWED_CODE;
    if (retained && code_role) {
        /* Only the real escaped-frame phase acquires its code edge.
         * This consumes/recreates the frame child, not gen support. */
        frame->f_executable = PyStackRef_MakeHeapSafe(frame->f_executable);
        code_role = 0;
    }
    frame->soac_lifetime_owned_environment = code_role;
    frame->owner = retained ? FRAME_OWNED_BY_SOAC_FINISHED
                            : FRAME_OWNED_BY_SOAC_CLEARED;
    if (!retained) {
        return 0;
    }
    frame->f_funcobj = PyStackRef_FromPyObjectNew(source_function);
    if (rows->kind == SOAC_LIFETIME_OBJECT_ROWS) {
        frame->f_globals = captured_globals;
        frame->f_builtins = captured_builtins;
        /* Cyclic GC may already have cleared this still-allocated function, or it
         * may still be queued for tp_clear after this owner. delete_garbage does not
         * recheck reachability after Finish adds the function reference. Retain the
         * explicit environment in either case. This is the function's own pending
         * collection state, not an ambient "GC is running" test: reachable ordinary
         * functions still add no map edges or refcount changes. */
        assert(frame->soac_lifetime_owned_environment == 0);
        int function_collecting =
            (_Py_AS_GC(source_function)->_gc_prev & _PyGC_PREV_MASK_COLLECTING) != 0;
        if (function_collecting || function->func_globals != captured_globals) {
            Py_INCREF(captured_globals);
            frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_OWNS_GLOBALS;
        }
        if (function_collecting || function->func_builtins != captured_builtins) {
            Py_INCREF(captured_builtins);
            frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_OWNS_BUILTINS;
        }
    }
    frame->f_locals = Py_XNewRef(source_namespace);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *value = soac_lifetime_owner_view(rows, i);
        frame->localsplus[i] = value == NULL ? PyStackRef_NULL
            : PyStackRef_FromPyObjectNew(value);
    }
    frame->stackpointer = frame->localsplus + count;
    return 1;
#endif
}

int
PyFrame_FinishSoacLifetimeWithEnvironment(
    PyFrameObject *object, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins,
    PyObject *source_namespace, PyObject *const *source_owners, Py_ssize_t count)
{
    const SoacLifetimeOwnerRows rows = {
        .kind = SOAC_LIFETIME_OBJECT_ROWS, .objects = source_owners,
    };
    return soac_lifetime_finish_owner_rows(
        object, source_function, captured_globals, captured_builtins,
        source_namespace, &rows, count);
}

int
PyFrame_FinishSoacLifetimeWithReferencesV1(
    PyFrameObject *object, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins,
    PyObject *source_namespace,
    const PySoacRefV1 *const *source_owner_slots, Py_ssize_t count)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)object;
    (void)source_function;
    (void)captured_globals;
    (void)captured_builtins;
    (void)source_namespace;
    (void)source_owner_slots;
    (void)count;
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-lifetime references require the selected 64-bit GIL build");
    return -1;
#else
    /* These legacy-shaped operands are intentionally not map-liveness proof.
     * Canonical native-reference callers pass NULL; no retired pointer is read. */
    (void)captured_globals;
    (void)captured_builtins;
    const SoacLifetimeOwnerRows rows = {
        .kind = SOAC_LIFETIME_REFERENCE_ROWS, .references = source_owner_slots,
    };
    return soac_lifetime_finish_owner_rows(
        object, source_function, NULL, NULL, source_namespace, &rows, count);
#endif
}


/* Caller-owned native source-parent scope storage. */
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

_Static_assert(sizeof(SoacLifetimeScopeStateV1) == sizeof(PySoacLifetimeScopeV1),
               "source-parent scope word count must match its public ABI");
_Static_assert(_Alignof(SoacLifetimeScopeStateV1) == _Alignof(PySoacLifetimeScopeV1),
               "source-parent scope alignment must match its public ABI");

static int
soac_scope_storage_valid(PySoacLifetimeScopeV1 *scope, size_t size)
{
    if (scope == NULL || size != sizeof(*scope) ||
        (uintptr_t)scope % _Alignof(PySoacLifetimeScopeV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "invalid source-parent scope size or alignment");
        return -1;
    }
    return 0;
}

int
PyFrame_GetSoacLifetimeScopeLayoutV1(
    uint32_t version, size_t layout_size, PySoacLifetimeScopeLayoutV1 *layout)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (version != PySoac_LIFETIME_SCOPE_ABI_VERSION || layout == NULL ||
        layout_size != sizeof(*layout) ||
        (uintptr_t)layout % _Alignof(PySoacLifetimeScopeLayoutV1) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "unsupported source-parent scope layout version or size");
        return -1;
    }
    *layout = (PySoacLifetimeScopeLayoutV1) {
        .version = PySoac_LIFETIME_SCOPE_ABI_VERSION,
        .word_count = PySoac_LIFETIME_SCOPE_V1_WORDS,
        .size = sizeof(PySoacLifetimeScopeV1),
        .alignment = _Alignof(PySoacLifetimeScopeV1),
    };
    return 0;
#endif
}

int
PyFrame_EnterSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, PyFrameObject *object,
    PyObject *actual_function, PyObject *captured_globals,
    PyObject *captured_builtins)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (soac_scope_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    for (size_t i = 0; i < PySoac_LIFETIME_SCOPE_V1_WORDS; i++) {
        if (scope->_opaque[i] != 0) {
            PyErr_SetString(PyExc_RuntimeError,
                            "source-parent scope storage is one-use");
            return -1;
        }
    }
    if (object == NULL || !PyFrame_Check(object) ||
        object->f_frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        actual_function == NULL || !PyFunction_Check(actual_function)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent entry requires an active frame and function");
        return -1;
    }
    _PyInterpreterFrame *frame = object->f_frame;
    if (captured_globals == NULL || captured_builtins == NULL) {
        PyErr_SetString(PyExc_ValueError,
                        "source-parent entry requires the captured environment");
        return -1;
    }
    if ((frame->soac_lifetime_owned_environment & ~SOAC_LIFETIME_BORROWED_CODE) != 0 ||
        frame->previous != NULL || object->f_back != NULL ||
        !PyStackRef_IsNull(frame->f_funcobj) ||
        frame->f_globals != NULL || frame->f_builtins != NULL ||
        frame->f_locals != NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent frame is already linked or terminal");
        return -1;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    SoacLifetimeScopeStateV1 state = {
        .version = PySoac_LIFETIME_SCOPE_ABI_VERSION,
        .state = SOAC_SCOPE_V1_LINKED,
        .storage = (uintptr_t)scope,
        .thread = (uintptr_t)tstate,
        .frame = (uintptr_t)frame,
        .previous = (uintptr_t)tstate->current_frame,
    };
    /* No Python ownership edge. In STACKREF_DEBUG this creates the native
     * borrowed handle, which must be closed before the external owner dies. */
    frame->f_funcobj = PyStackRef_FromPyObjectBorrow(actual_function);
    frame->f_globals = captured_globals;
    frame->f_builtins = captured_builtins;
    frame->previous = tstate->current_frame;
    frame->soac_lifetime_owned_environment |= SOAC_LIFETIME_SCOPE_LINKED;
    memcpy(scope, &state, sizeof(state));
    /* Publish only after every field visible to asynchronous readers is ready. */
    tstate->current_frame = frame;
    return 0;
#endif
}

int
PyFrame_LeaveSoacLifetimeScopeV1(
    PySoacLifetimeScopeV1 *scope, size_t scope_size, int reason)
{
#ifdef Py_GIL_DISABLED
    PyErr_SetString(PyExc_NotImplementedError,
                    "source-parent scopes require the selected GIL build");
    return -1;
#else
    if (soac_scope_storage_valid(scope, scope_size) < 0) {
        return -1;
    }
    if (reason != PySoac_LIFETIME_SCOPE_SUSPEND &&
        reason != PySoac_LIFETIME_SCOPE_TERMINAL) {
        PyErr_SetString(PyExc_ValueError, "invalid source-parent leave reason");
        return -1;
    }
    SoacLifetimeScopeStateV1 state;
    /* memcpy avoids accessing public uintptr storage through an aliased struct. */
    memcpy(&state, scope, sizeof(state));
    if (state.version != PySoac_LIFETIME_SCOPE_ABI_VERSION ||
        state.state != SOAC_SCOPE_V1_LINKED || state.storage != (uintptr_t)scope) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scope is inactive, copied, or already left");
        return -1;
    }
    PyThreadState *tstate = _PyThreadState_GET();
    if (state.thread != (uintptr_t)tstate) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scope belongs to another thread state");
        return -1;
    }
    /* Compare current identity before dereferencing the saved frame pointer. */
    if (state.frame != (uintptr_t)tstate->current_frame) {
        PyErr_SetString(PyExc_RuntimeError,
                        "source-parent scopes must leave in stack order");
        return -1;
    }
    _PyInterpreterFrame *frame = tstate->current_frame;
    if (frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        (frame->soac_lifetime_owned_environment & ~SOAC_LIFETIME_BORROWED_CODE) !=
            SOAC_LIFETIME_SCOPE_LINKED ||
        (uintptr_t)frame->previous != state.previous) {
        PyErr_SetString(PyExc_RuntimeError, "invalid linked source-parent frame");
        return -1;
    }
    PyFrameObject *object = _PyFrame_GetSoacLifetimeObject(frame);
    _PyInterpreterFrame *previous = frame->previous;
    int terminal = reason == PySoac_LIFETIME_SCOPE_TERMINAL;

    /* Valid leave cannot fail after this point. Detach before allocation or
     * any caller-owned primary cleanup, including reentrant finalizers. */
    tstate->current_frame = previous;
    frame->previous = NULL;
    frame->soac_lifetime_owned_environment =
        (frame->soac_lifetime_owned_environment & SOAC_LIFETIME_BORROWED_CODE) |
        (terminal ? SOAC_LIFETIME_SCOPE_TERMINAL : 0);
    if (tstate->last_profiled_frame == frame) {
        tstate->last_profiled_frame = previous;
    }
    state.state = terminal ? SOAC_SCOPE_V1_TERMINAL : SOAC_SCOPE_V1_SUSPENDED;
    state.thread = state.frame = state.previous = 0;
    memcpy(scope, &state, sizeof(state));

    _PyStackRef function = frame->f_funcobj;
    frame->f_funcobj = PyStackRef_NULL;
    frame->f_globals = NULL;
    frame->f_builtins = NULL;
    PyStackRef_CLOSE(function);  /* borrowed: closes debug handle, no DECREF */

    /* Suspension must not retain the resume caller. At terminal, mirror the
     * ordinary take_ownership predecessor link, including its OOM policy. */
    if (terminal && !_PyObject_IsUniquelyReferenced((PyObject *)object)) {
        assert(object->f_back == NULL);
        _PyInterpreterFrame *parent = _PyFrame_GetFirstComplete(previous);
        if (parent != NULL) {
            assert(parent->owner < FRAME_OWNED_BY_INTERPRETER ||
                   (parent->owner == FRAME_OWNED_BY_SOAC_ACTIVE &&
                    (parent->soac_lifetime_owned_environment & SOAC_LIFETIME_SCOPE_LINKED)));
            PyObject *error = PyErr_GetRaisedException();
            PyFrameObject *back = _PyFrame_GetFrameObject(parent);
            if (back == NULL) {
                assert(PyErr_ExceptionMatches(PyExc_MemoryError));
                PyErr_Clear();
            }
            else {
                object->f_back = (PyFrameObject *)Py_NewRef(back);
            }
            PyErr_SetRaisedException(error);
        }
    }
    return 0;
#endif
}

/* Append to the existing native22 lifetime unit. Scope21 layout/Enter/Leave
 * and all existing constructor/Finish/traceback bodies remain unchanged. */
_PyInterpreterFrame *
_PyFrame_BorrowSoacLifetimeScopeV1(
    const PySoacLifetimeScopeV1 *scope, size_t scope_size,
    PyCodeObject *expected_code)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    (void)scope; (void)scope_size; (void)expected_code;
    PyErr_SetString(PyExc_NotImplementedError,
                    "outgoing source calls require the selected 64-bit GIL build");
    return NULL;
#else
    /* The existing validator is read-only despite its older non-const API. */
    if (soac_scope_storage_valid((PySoacLifetimeScopeV1 *)(void *)scope,
                                 scope_size) < 0) {
        return NULL;
    }
    SoacLifetimeScopeStateV1 state;
    memcpy(&state, scope, sizeof(state));
    if (state.version != PySoac_LIFETIME_SCOPE_ABI_VERSION ||
        state.state != SOAC_SCOPE_V1_LINKED || state.storage != (uintptr_t)scope) {
        PyErr_SetString(PyExc_RuntimeError,
                        "outgoing call needs its original linked source scope");
        return NULL;
    }
    PyThreadState *thread = _PyThreadState_GET();
    if (state.thread != (uintptr_t)thread ||
        state.frame != (uintptr_t)thread->current_frame) {
        PyErr_SetString(PyExc_RuntimeError,
                        "outgoing source call used outside its current native interval");
        return NULL;
    }
    _PyInterpreterFrame *frame = thread->current_frame;
    if (frame == NULL || frame->owner != FRAME_OWNED_BY_SOAC_ACTIVE ||
        (frame->soac_lifetime_owned_environment & ~SOAC_LIFETIME_BORROWED_CODE) !=
            SOAC_LIFETIME_SCOPE_LINKED ||
        (uintptr_t)frame->previous != state.previous ||
        _PyFrame_GetCode(frame) != expected_code) {
        PyErr_SetString(PyExc_RuntimeError,
                        "outgoing call does not match its captured source frame/code");
        return NULL;
    }
    return frame;
#endif
}
