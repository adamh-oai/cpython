/* Execute compiled code */

#include "ceval.h"
#include "pycore_soac_call_v1.h"
#include "pycore_soac_source_entry_v1.h"
#include "pycore_soac_vm_call_v1.h"
#include "pycore_soac_outgoing_v1.h"
#include "soac_reference_v1.inc"  // one native opaque-reference implementation

int
Py_GetRecursionLimit(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return interp->ceval.recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    _PyEval_StopTheWorld(interp);
    interp->ceval.recursion_limit = new_limit;
    _Py_FOR_EACH_TSTATE_BEGIN(interp, p) {
        int depth = p->py_recursion_limit - p->py_recursion_remaining;
        p->py_recursion_limit = new_limit;
        p->py_recursion_remaining = new_limit - depth;
    }
    _Py_FOR_EACH_TSTATE_END(interp);
    _PyEval_StartTheWorld(interp);
}

int
_Py_ReachedRecursionLimitWithMargin(PyThreadState *tstate, int margin_count)
{
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    _PyThreadStateImpl *_tstate = (_PyThreadStateImpl *)tstate;
#if _Py_STACK_GROWS_DOWN
    if (here_addr > _tstate->c_stack_soft_limit + margin_count * _PyOS_STACK_MARGIN_BYTES) {
#else
    if (here_addr <= _tstate->c_stack_soft_limit - margin_count * _PyOS_STACK_MARGIN_BYTES) {
#endif
        return 0;
    }
    if (_tstate->c_stack_hard_limit == 0) {
        _Py_InitializeRecursionLimits(tstate);
    }
#if _Py_STACK_GROWS_DOWN
    return here_addr <= _tstate->c_stack_soft_limit + margin_count * _PyOS_STACK_MARGIN_BYTES &&
        here_addr >= _tstate->c_stack_soft_limit - 2 * _PyOS_STACK_MARGIN_BYTES;
#else
    return here_addr > _tstate->c_stack_soft_limit - margin_count * _PyOS_STACK_MARGIN_BYTES &&
        here_addr <= _tstate->c_stack_soft_limit + 2 * _PyOS_STACK_MARGIN_BYTES;
#endif
}

void
_Py_EnterRecursiveCallUnchecked(PyThreadState *tstate)
{
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    _PyThreadStateImpl *_tstate = (_PyThreadStateImpl *)tstate;
#if _Py_STACK_GROWS_DOWN
    if (here_addr < _tstate->c_stack_hard_limit) {
#else
    if (here_addr > _tstate->c_stack_hard_limit) {
#endif
        Py_FatalError("Unchecked stack overflow.");
    }
}

#if defined(__s390x__)
#  define Py_C_STACK_SIZE 320000
#elif defined(_WIN32)
   // Don't define Py_C_STACK_SIZE, ask the O/S
#elif defined(__ANDROID__)
#  define Py_C_STACK_SIZE 1200000
#elif defined(__sparc__)
#  define Py_C_STACK_SIZE 1600000
#elif defined(__hppa__) || defined(__powerpc64__)
#  define Py_C_STACK_SIZE 2000000
#else
#  define Py_C_STACK_SIZE 4000000
#endif

#if defined(__EMSCRIPTEN__)

// Temporary workaround to make `pthread_getattr_np` work on Emscripten.
// Emscripten 4.0.6 will contain a fix:
// https://github.com/emscripten-core/emscripten/pull/23887

#include "emscripten/stack.h"

#define pthread_attr_t workaround_pthread_attr_t
#define pthread_getattr_np workaround_pthread_getattr_np
#define pthread_attr_getguardsize workaround_pthread_attr_getguardsize
#define pthread_attr_getstack workaround_pthread_attr_getstack
#define pthread_attr_destroy workaround_pthread_attr_destroy

typedef struct {
    void *_a_stackaddr;
    size_t _a_stacksize, _a_guardsize;
} pthread_attr_t;

extern __attribute__((__visibility__("hidden"))) unsigned __default_guardsize;

// Modified version of pthread_getattr_np from the upstream PR.

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr) {
  attr->_a_stackaddr = (void*)emscripten_stack_get_base();
  attr->_a_stacksize = emscripten_stack_get_base() - emscripten_stack_get_end();
  attr->_a_guardsize = __default_guardsize;
  return 0;
}

// These three functions copied without any changes from Emscripten libc.

int pthread_attr_getguardsize(const pthread_attr_t *restrict a, size_t *restrict size)
{
	*size = a->_a_guardsize;
	return 0;
}

int pthread_attr_getstack(const pthread_attr_t *restrict a, void **restrict addr, size_t *restrict size)
{
/// XXX musl is not standard-conforming? It should not report EINVAL if _a_stackaddr is zero, and it should
///     report EINVAL if a is null: http://pubs.opengroup.org/onlinepubs/009695399/functions/pthread_attr_getstack.html
	if (!a) return EINVAL;
//	if (!a->_a_stackaddr)
//		return EINVAL;

	*size = a->_a_stacksize;
	*addr = (void *)(a->_a_stackaddr - *size);
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *a)
{
	return 0;
}

#endif

static void
hardware_stack_limits(uintptr_t *base, uintptr_t *top, uintptr_t sp)
{
#ifdef WIN32
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    *top = (uintptr_t)high;
    ULONG guarantee = 0;
    SetThreadStackGuarantee(&guarantee);
    *base = (uintptr_t)low + guarantee;
#elif defined(__APPLE__)
    pthread_t this_thread = pthread_self();
    void *stack_addr = pthread_get_stackaddr_np(this_thread); // top of the stack
    size_t stack_size = pthread_get_stacksize_np(this_thread);
    *top = (uintptr_t)stack_addr;
    *base = ((uintptr_t)stack_addr) - stack_size;
#else
    /// XXX musl supports HAVE_PTHRED_GETATTR_NP, but the resulting stack size
    /// (on alpine at least) is much smaller than expected and imposes undue limits
    /// compared to the old stack size estimation.  (We assume musl is not glibc.)
#  if defined(HAVE_PTHREAD_GETATTR_NP) && !defined(_AIX) && \
        !defined(__NetBSD__) && (defined(__GLIBC__) || !defined(__linux__))
    size_t stack_size, guard_size;
    void *stack_addr;
    pthread_attr_t attr;
    int err = pthread_getattr_np(pthread_self(), &attr);
    if (err == 0) {
        err = pthread_attr_getguardsize(&attr, &guard_size);
        err |= pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        err |= pthread_attr_destroy(&attr);
    }
    if (err == 0) {
        *base = ((uintptr_t)stack_addr) + guard_size;
        *top = (uintptr_t)stack_addr + stack_size;
        return;
    }
#  endif
    // Add some space for caller function then round to minimum page size
    // This is a guess at the top of the stack, but should be a reasonably
    // good guess if called from _PyThreadState_Attach when creating a thread.
    // If the thread is attached deep in a call stack, then the guess will be poor.
#if _Py_STACK_GROWS_DOWN
    uintptr_t top_addr = _Py_SIZE_ROUND_UP(sp + 8*sizeof(void*), SYSTEM_PAGE_SIZE);
    *top = top_addr;
    *base = top_addr - Py_C_STACK_SIZE;
#  else
    uintptr_t base_addr = _Py_SIZE_ROUND_DOWN(sp - 8*sizeof(void*), SYSTEM_PAGE_SIZE);
    *base = base_addr;
    *top = base_addr + Py_C_STACK_SIZE;
#endif
#endif
}

static void
tstate_set_stack(PyThreadState *tstate,
                 uintptr_t base, uintptr_t top)
{
    assert(base < top);
    assert((top - base) >= _PyOS_MIN_STACK_SIZE);

#ifdef _Py_THREAD_SANITIZER
    // Thread sanitizer crashes if we use more than half the stack.
    uintptr_t stacksize = top - base;
#  if _Py_STACK_GROWS_DOWN
    base += stacksize/2;
#  else
    top -= stacksize/2;
#  endif
#endif
    _PyThreadStateImpl *_tstate = (_PyThreadStateImpl *)tstate;
#if _Py_STACK_GROWS_DOWN
    _tstate->c_stack_top = top;
    _tstate->c_stack_hard_limit = base + _PyOS_STACK_MARGIN_BYTES;
    _tstate->c_stack_soft_limit = base + _PyOS_STACK_MARGIN_BYTES * 2;
#  ifndef NDEBUG
    // Sanity checks
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    assert(ts->c_stack_hard_limit <= ts->c_stack_soft_limit);
    assert(ts->c_stack_soft_limit < ts->c_stack_top);
#  endif
#else
    _tstate->c_stack_top = base;
    _tstate->c_stack_hard_limit = top - _PyOS_STACK_MARGIN_BYTES;
    _tstate->c_stack_soft_limit = top - _PyOS_STACK_MARGIN_BYTES * 2;
#  ifndef NDEBUG
    // Sanity checks
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    assert(ts->c_stack_hard_limit >= ts->c_stack_soft_limit);
    assert(ts->c_stack_soft_limit > ts->c_stack_top);
#  endif
#endif
}


void
_Py_InitializeRecursionLimits(PyThreadState *tstate)
{
    uintptr_t base, top;
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    hardware_stack_limits(&base, &top, here_addr);
    assert(top != 0);

    tstate_set_stack(tstate, base, top);
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    ts->c_stack_init_base = base;
    ts->c_stack_init_top = top;
}


int
PyUnstable_ThreadState_SetStackProtection(PyThreadState *tstate,
                                void *stack_start_addr, size_t stack_size)
{
    if (stack_size < _PyOS_MIN_STACK_SIZE) {
        PyErr_Format(PyExc_ValueError,
                     "stack_size must be at least %zu bytes",
                     _PyOS_MIN_STACK_SIZE);
        return -1;
    }

    uintptr_t base = (uintptr_t)stack_start_addr;
    uintptr_t top = base + stack_size;
    tstate_set_stack(tstate, base, top);
    return 0;
}


void
PyUnstable_ThreadState_ResetStackProtection(PyThreadState *tstate)
{
    _PyThreadStateImpl *ts = (_PyThreadStateImpl *)tstate;
    if (ts->c_stack_init_top != 0) {
        tstate_set_stack(tstate,
                         ts->c_stack_init_base,
                         ts->c_stack_init_top);
        return;
    }

    _Py_InitializeRecursionLimits(tstate);
}


/* The function _Py_EnterRecursiveCallTstate() only calls _Py_CheckRecursiveCall()
   if the stack pointer is between the stack base and c_stack_hard_limit. */
int
_Py_CheckRecursiveCall(PyThreadState *tstate, const char *where)
{
    _PyThreadStateImpl *_tstate = (_PyThreadStateImpl *)tstate;
    uintptr_t here_addr = _Py_get_machine_stack_pointer();
    assert(_tstate->c_stack_soft_limit != 0);
    assert(_tstate->c_stack_hard_limit != 0);
#if _Py_STACK_GROWS_DOWN
    assert(here_addr >= _tstate->c_stack_hard_limit - _PyOS_STACK_MARGIN_BYTES);
    if (here_addr < _tstate->c_stack_hard_limit) {
        /* Overflowing while handling an overflow. Give up. */
        int kbytes_used = (int)(_tstate->c_stack_top - here_addr)/1024;
#else
    assert(here_addr <= _tstate->c_stack_hard_limit + _PyOS_STACK_MARGIN_BYTES);
    if (here_addr > _tstate->c_stack_hard_limit) {
        /* Overflowing while handling an overflow. Give up. */
        int kbytes_used = (int)(here_addr - _tstate->c_stack_top)/1024;
#endif
        char buffer[80];
        snprintf(buffer, 80, "Unrecoverable stack overflow (used %d kB)%s", kbytes_used, where);
        Py_FatalError(buffer);
    }
    if (tstate->recursion_headroom) {
        return 0;
    }
    else {
#if _Py_STACK_GROWS_DOWN
        int kbytes_used = (int)(_tstate->c_stack_top - here_addr)/1024;
#else
        int kbytes_used = (int)(here_addr - _tstate->c_stack_top)/1024;
#endif
        tstate->recursion_headroom++;
        _PyErr_Format(tstate, PyExc_RecursionError,
                    "Stack overflow (used %d kB)%s",
                    kbytes_used,
                    where);
        tstate->recursion_headroom--;
        return -1;
    }
}


const binaryfunc _PyEval_BinaryOps[] = {
    [NB_ADD] = PyNumber_Add,
    [NB_AND] = PyNumber_And,
    [NB_FLOOR_DIVIDE] = PyNumber_FloorDivide,
    [NB_LSHIFT] = PyNumber_Lshift,
    [NB_MATRIX_MULTIPLY] = PyNumber_MatrixMultiply,
    [NB_MULTIPLY] = PyNumber_Multiply,
    [NB_REMAINDER] = PyNumber_Remainder,
    [NB_OR] = PyNumber_Or,
    [NB_POWER] = _PyNumber_PowerNoMod,
    [NB_RSHIFT] = PyNumber_Rshift,
    [NB_SUBTRACT] = PyNumber_Subtract,
    [NB_TRUE_DIVIDE] = PyNumber_TrueDivide,
    [NB_XOR] = PyNumber_Xor,
    [NB_INPLACE_ADD] = PyNumber_InPlaceAdd,
    [NB_INPLACE_AND] = PyNumber_InPlaceAnd,
    [NB_INPLACE_FLOOR_DIVIDE] = PyNumber_InPlaceFloorDivide,
    [NB_INPLACE_LSHIFT] = PyNumber_InPlaceLshift,
    [NB_INPLACE_MATRIX_MULTIPLY] = PyNumber_InPlaceMatrixMultiply,
    [NB_INPLACE_MULTIPLY] = PyNumber_InPlaceMultiply,
    [NB_INPLACE_REMAINDER] = PyNumber_InPlaceRemainder,
    [NB_INPLACE_OR] = PyNumber_InPlaceOr,
    [NB_INPLACE_POWER] = _PyNumber_InPlacePowerNoMod,
    [NB_INPLACE_RSHIFT] = PyNumber_InPlaceRshift,
    [NB_INPLACE_SUBTRACT] = PyNumber_InPlaceSubtract,
    [NB_INPLACE_TRUE_DIVIDE] = PyNumber_InPlaceTrueDivide,
    [NB_INPLACE_XOR] = PyNumber_InPlaceXor,
    [NB_SUBSCR] = PyObject_GetItem,
};

const conversion_func _PyEval_ConversionFuncs[4] = {
    [FVC_STR] = PyObject_Str,
    [FVC_REPR] = PyObject_Repr,
    [FVC_ASCII] = PyObject_ASCII
};

const _Py_SpecialMethod _Py_SpecialMethods[] = {
    [SPECIAL___ENTER__] = {
        .name = &_Py_ID(__enter__),
        .error = (
            "'%T' object does not support the context manager protocol "
            "(missed __enter__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the context manager protocol "
            "(missed __enter__ method) but it supports the asynchronous "
            "context manager protocol. Did you mean to use 'async with'?"
        )
    },
    [SPECIAL___EXIT__] = {
        .name = &_Py_ID(__exit__),
        .error = (
            "'%T' object does not support the context manager protocol "
            "(missed __exit__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the context manager protocol "
            "(missed __exit__ method) but it supports the asynchronous "
            "context manager protocol. Did you mean to use 'async with'?"
        )
    },
    [SPECIAL___AENTER__] = {
        .name = &_Py_ID(__aenter__),
        .error = (
            "'%T' object does not support the asynchronous "
            "context manager protocol (missed __aenter__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the asynchronous context manager "
            "protocol (missed __aenter__ method) but it supports the context "
            "manager protocol. Did you mean to use 'with'?"
        )
    },
    [SPECIAL___AEXIT__] = {
        .name = &_Py_ID(__aexit__),
        .error = (
            "'%T' object does not support the asynchronous "
            "context manager protocol (missed __aexit__ method)"
        ),
        .error_suggestion = (
            "'%T' object does not support the asynchronous context manager "
            "protocol (missed __aexit__ method) but it supports the context "
            "manager protocol. Did you mean to use 'with'?"
        )
    }
};

const size_t _Py_FunctionAttributeOffsets[] = {
    [MAKE_FUNCTION_CLOSURE] = offsetof(PyFunctionObject, func_closure),
    [MAKE_FUNCTION_ANNOTATIONS] = offsetof(PyFunctionObject, func_annotations),
    [MAKE_FUNCTION_KWDEFAULTS] = offsetof(PyFunctionObject, func_kwdefaults),
    [MAKE_FUNCTION_DEFAULTS] = offsetof(PyFunctionObject, func_defaults),
    [MAKE_FUNCTION_ANNOTATE] = offsetof(PyFunctionObject, func_annotate),
};

// PEP 634: Structural Pattern Matching


// Return a tuple of values corresponding to keys, with error checks for
// duplicate/missing keys.
PyObject *
_PyEval_MatchKeys(PyThreadState *tstate, PyObject *map, PyObject *keys)
{
    assert(PyTuple_CheckExact(keys));
    Py_ssize_t nkeys = PyTuple_GET_SIZE(keys);
    if (!nkeys) {
        // No keys means no items.
        return PyTuple_New(0);
    }
    PyObject *seen = NULL;
    PyObject *dummy = NULL;
    PyObject *values = NULL;
    // We use the two argument form of map.get(key, default) for two reasons:
    // - Atomically check for a key and get its value without error handling.
    // - Don't cause key creation or resizing in dict subclasses like
    //   collections.defaultdict that define __missing__ (or similar).
    _PyCStackRef cref;
    _PyThreadState_PushCStackRef(tstate, &cref);
    int meth_found = _PyObject_GetMethodStackRef(tstate, map, &_Py_ID(get), &cref.ref);
    PyObject *get = PyStackRef_AsPyObjectBorrow(cref.ref);
    if (get == NULL) {
        goto fail;
    }
    seen = PySet_New(NULL);
    if (seen == NULL) {
        goto fail;
    }
    // dummy = object()
    dummy = _PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
    if (dummy == NULL) {
        goto fail;
    }
    values = PyTuple_New(nkeys);
    if (values == NULL) {
        goto fail;
    }
    for (Py_ssize_t i = 0; i < nkeys; i++) {
        PyObject *key = PyTuple_GET_ITEM(keys, i);
        if (PySet_Contains(seen, key) || PySet_Add(seen, key)) {
            if (!_PyErr_Occurred(tstate)) {
                // Seen it before!
                _PyErr_Format(tstate, PyExc_ValueError,
                              "mapping pattern checks duplicate key (%R)", key);
            }
            goto fail;
        }
        PyObject *args[] = { map, key, dummy };
        PyObject *value = NULL;
        if (meth_found) {
            value = PyObject_Vectorcall(get, args, 3, NULL);
        }
        else {
            value = PyObject_Vectorcall(get, &args[1], 2, NULL);
        }
        if (value == NULL) {
            goto fail;
        }
        if (value == dummy) {
            // key not in map!
            Py_DECREF(value);
            Py_DECREF(values);
            // Return None:
            values = Py_NewRef(Py_None);
            goto done;
        }
        PyTuple_SET_ITEM(values, i, value);
    }
    // Success:
done:
    _PyThreadState_PopCStackRef(tstate, &cref);
    Py_DECREF(seen);
    Py_DECREF(dummy);
    return values;
fail:
    _PyThreadState_PopCStackRef(tstate, &cref);
    Py_XDECREF(seen);
    Py_XDECREF(dummy);
    Py_XDECREF(values);
    return NULL;
}

// Extract a named attribute from the subject, with additional bookkeeping to
// raise TypeErrors for repeated lookups. On failure, return NULL (with no
// error set). Use _PyErr_Occurred(tstate) to disambiguate.
static PyObject *
match_class_attr(PyThreadState *tstate, PyObject *subject, PyObject *type,
                 PyObject *name, PyObject *seen)
{
    assert(PyUnicode_CheckExact(name));
    assert(PySet_CheckExact(seen));
    if (PySet_Contains(seen, name) || PySet_Add(seen, name)) {
        if (!_PyErr_Occurred(tstate)) {
            // Seen it before!
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%s() got multiple sub-patterns for attribute %R",
                          ((PyTypeObject*)type)->tp_name, name);
        }
        return NULL;
    }
    PyObject *attr;
    (void)PyObject_GetOptionalAttr(subject, name, &attr);
    return attr;
}

// On success (match), return a tuple of extracted attributes. On failure (no
// match), return NULL. Use _PyErr_Occurred(tstate) to disambiguate.
PyObject*
_PyEval_MatchClass(PyThreadState *tstate, PyObject *subject, PyObject *type,
                   Py_ssize_t nargs, PyObject *kwargs)
{
    if (!PyType_Check(type)) {
        const char *e = "called match pattern must be a class";
        _PyErr_Format(tstate, PyExc_TypeError, e);
        return NULL;
    }
    assert(PyTuple_CheckExact(kwargs));
    // First, an isinstance check:
    if (PyObject_IsInstance(subject, type) <= 0) {
        return NULL;
    }
    // So far so good:
    PyObject *seen = PySet_New(NULL);
    if (seen == NULL) {
        return NULL;
    }
    PyObject *attrs = PyList_New(0);
    if (attrs == NULL) {
        Py_DECREF(seen);
        return NULL;
    }
    // NOTE: From this point on, goto fail on failure:
    PyObject *match_args = NULL;
    // First, the positional subpatterns:
    if (nargs) {
        int match_self = 0;
        if (PyObject_GetOptionalAttr(type, &_Py_ID(__match_args__), &match_args) < 0) {
            goto fail;
        }
        if (match_args) {
            if (!PyTuple_CheckExact(match_args)) {
                const char *e = "%s.__match_args__ must be a tuple (got %s)";
                _PyErr_Format(tstate, PyExc_TypeError, e,
                              ((PyTypeObject *)type)->tp_name,
                              Py_TYPE(match_args)->tp_name);
                goto fail;
            }
        }
        else {
            // _Py_TPFLAGS_MATCH_SELF is only acknowledged if the type does not
            // define __match_args__. This is natural behavior for subclasses:
            // it's as if __match_args__ is some "magic" value that is lost as
            // soon as they redefine it.
            match_args = PyTuple_New(0);
            match_self = PyType_HasFeature((PyTypeObject*)type,
                                            _Py_TPFLAGS_MATCH_SELF);
        }
        assert(PyTuple_CheckExact(match_args));
        Py_ssize_t allowed = match_self ? 1 : PyTuple_GET_SIZE(match_args);
        if (allowed < nargs) {
            const char *plural = (allowed == 1) ? "" : "s";
            _PyErr_Format(tstate, PyExc_TypeError,
                          "%s() accepts %d positional sub-pattern%s (%d given)",
                          ((PyTypeObject*)type)->tp_name,
                          allowed, plural, nargs);
            goto fail;
        }
        if (match_self) {
            // Easy. Copy the subject itself, and move on to kwargs.
            if (PyList_Append(attrs, subject) < 0) {
                goto fail;
            }
        }
        else {
            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject *name = PyTuple_GET_ITEM(match_args, i);
                if (!PyUnicode_CheckExact(name)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "__match_args__ elements must be strings "
                                  "(got %s)", Py_TYPE(name)->tp_name);
                    goto fail;
                }
                PyObject *attr = match_class_attr(tstate, subject, type, name,
                                                  seen);
                if (attr == NULL) {
                    goto fail;
                }
                if (PyList_Append(attrs, attr) < 0) {
                    Py_DECREF(attr);
                    goto fail;
                }
                Py_DECREF(attr);
            }
        }
        Py_CLEAR(match_args);
    }
    // Finally, the keyword subpatterns:
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwargs); i++) {
        PyObject *name = PyTuple_GET_ITEM(kwargs, i);
        PyObject *attr = match_class_attr(tstate, subject, type, name, seen);
        if (attr == NULL) {
            goto fail;
        }
        if (PyList_Append(attrs, attr) < 0) {
            Py_DECREF(attr);
            goto fail;
        }
        Py_DECREF(attr);
    }
    Py_SETREF(attrs, PyList_AsTuple(attrs));
    Py_DECREF(seen);
    return attrs;
fail:
    // We really don't care whether an error was raised or not... that's our
    // caller's problem. All we know is that the match failed.
    Py_XDECREF(match_args);
    Py_DECREF(seen);
    Py_DECREF(attrs);
    return NULL;
}


static int do_raise(PyThreadState *tstate, PyObject *exc, PyObject *cause);

PyObject *
PyEval_EvalCode(PyObject *co, PyObject *globals, PyObject *locals)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (locals == NULL) {
        locals = globals;
    }
    PyObject *builtins = _PyDict_LoadBuiltinsFromGlobals(globals);
    if (builtins == NULL) {
        return NULL;
    }
    PyFrameConstructor desc = {
        .fc_globals = globals,
        .fc_builtins = builtins,
        .fc_name = ((PyCodeObject *)co)->co_name,
        .fc_qualname = ((PyCodeObject *)co)->co_name,
        .fc_code = co,
        .fc_defaults = NULL,
        .fc_kwdefaults = NULL,
        .fc_closure = NULL
    };
    PyFunctionObject *func = _PyFunction_FromConstructor(&desc);
    _Py_DECREF_BUILTINS(builtins);
    if (func == NULL) {
        return NULL;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_LEGACY);
    PyObject *res = _PyEval_Vector(tstate, func, locals, NULL, 0, NULL);
    Py_DECREF(func);
    return res;
}


/* Interpreter main loop */

PyObject *
PyEval_EvalFrame(PyFrameObject *f)
{
    /* Function kept for backward compatibility */
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f->f_frame, 0);
}

PyObject *
PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_EvalFrame(tstate, f->f_frame, throwflag);
}

#include "ceval_macros.h"


/* Helper functions to keep the size of the largest uops down */

PyObject *
_Py_VectorCall_StackRefSteal(
    _PyStackRef callable,
    _PyStackRef *arguments,
    int total_args,
    _PyStackRef kwnames,
    _PyInterpreterFrame *frame)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    PyObject *callable_o = PyStackRef_AsPyObjectBorrow(callable);
    PyObject *kwnames_o = PyStackRef_AsPyObjectBorrow(kwnames);
    int positional_args = total_args;
    if (kwnames_o != NULL) {
        positional_args -= (int)PyTuple_GET_SIZE(kwnames_o);
    }
    res = _PySOAC_DataclassVectorcallFromFrame(
        frame, callable_o, args_o,
        positional_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
        kwnames_o);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    PyStackRef_XCLOSE(kwnames);
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject*
_Py_VectorCallInstrumentation_StackRefSteal(
    _PyStackRef callable,
    _PyStackRef* arguments,
    int total_args,
    _PyStackRef kwnames,
    bool call_instrumentation,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT* this_instr,
    PyThreadState* tstate)
{
    PyObject* res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    PyObject* callable_o = PyStackRef_AsPyObjectBorrow(callable);
    PyObject* kwnames_o = PyStackRef_AsPyObjectBorrow(kwnames);
    int positional_args = total_args;
    if (kwnames_o != NULL) {
        positional_args -= (int)PyTuple_GET_SIZE(kwnames_o);
    }
    res = _PySOAC_DataclassVectorcallFromFrame(
        frame, callable_o, args_o,
        positional_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
        kwnames_o);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    if (call_instrumentation) {
        PyObject* arg = total_args == 0 ?
            &_PyInstrumentation_MISSING : PyStackRef_AsPyObjectBorrow(arguments[0]);
        if (res == NULL) {
            _Py_call_instrumentation_exc2(
                tstate, PY_MONITORING_EVENT_C_RAISE,
                frame, this_instr, callable_o, arg);
        }
        else {
            int err = _Py_call_instrumentation_2args(
                tstate, PY_MONITORING_EVENT_C_RETURN,
                frame, this_instr, callable_o, arg);
            if (err < 0) {
                Py_CLEAR(res);
            }
        }
    }
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    PyStackRef_XCLOSE(kwnames);
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args - 1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_Py_BuiltinCallFast_StackRefSteal(
    _PyStackRef callable,
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    PyObject *callable_o = PyStackRef_AsPyObjectBorrow(callable);
    PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable_o);
    res = _PyCFunctionFast_CAST(cfunc)(
        PyCFunction_GET_SELF(callable_o),
        args_o,
        total_args
    );
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_Py_BuiltinCallFastWithKeywords_StackRefSteal(
    _PyStackRef callable,
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    PyObject *callable_o = PyStackRef_AsPyObjectBorrow(callable);
    PyCFunctionFastWithKeywords cfunc =
        _PyCFunctionFastWithKeywords_CAST(PyCFunction_GET_FUNCTION(callable_o));
    res = cfunc(PyCFunction_GET_SELF(callable_o), args_o, total_args, NULL);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_PyCallMethodDescriptorFast_StackRefSteal(
    _PyStackRef callable,
    PyMethodDef *meth,
    PyObject *self,
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    assert(((PyMethodDescrObject *)PyStackRef_AsPyObjectBorrow(callable))->d_method == meth);
    assert(self == PyStackRef_AsPyObjectBorrow(arguments[0]));

    PyCFunctionFast cfunc = _PyCFunctionFast_CAST(meth->ml_meth);
    res = cfunc(self, (args_o + 1), total_args - 1);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_PyCallMethodDescriptorFastWithKeywords_StackRefSteal(
    _PyStackRef callable,
    PyMethodDef *meth,
    PyObject *self,
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    assert(((PyMethodDescrObject *)PyStackRef_AsPyObjectBorrow(callable))->d_method == meth);
    assert(self == PyStackRef_AsPyObjectBorrow(arguments[0]));

    PyCFunctionFastWithKeywords cfunc =
        _PyCFunctionFastWithKeywords_CAST(meth->ml_meth);
    res = cfunc(self, (args_o + 1), total_args-1, NULL);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_Py_CallBuiltinClass_StackRefSteal(
    _PyStackRef callable,
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    PyTypeObject *tp = (PyTypeObject *)PyStackRef_AsPyObjectBorrow(callable);
    res = tp->tp_vectorcall((PyObject *)tp, args_o, total_args | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    PyStackRef_CLOSE(callable);
    return res;
}

PyObject *
_Py_BuildString_StackRefSteal(
    _PyStackRef *arguments,
    int total_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, total_args, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    res = _PyUnicode_JoinArray(&_Py_STR(empty), args_o, total_args);
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = total_args-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    return res;
}

PyObject *
_Py_BuildMap_StackRefSteal(
    _PyStackRef *arguments,
    int half_args)
{
    PyObject *res;
    STACKREFS_TO_PYOBJECTS(arguments, half_args*2, args_o);
    if (CONVERSION_FAILED(args_o)) {
        res = NULL;
        goto cleanup;
    }
    res = _PyDict_FromItems(
        args_o, 2,
        args_o+1, 2,
        half_args
    );
    STACKREFS_TO_PYOBJECTS_CLEANUP(args_o);
    assert((res != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    // arguments is a pointer into the GC visible stack,
    // so we must NULL out values as we clear them.
    for (int i = half_args*2-1; i >= 0; i--) {
        _PyStackRef tmp = arguments[i];
        arguments[i] = PyStackRef_NULL;
        PyStackRef_CLOSE(tmp);
    }
    return res;
}

_PyStackRef
_Py_LoadAttr_StackRefSteal(
    PyThreadState *tstate, _PyStackRef owner,
    PyObject *name, _PyStackRef *self_or_null)
{
    _PyCStackRef method;
    _PyThreadState_PushCStackRef(tstate, &method);
    int is_meth = _PyObject_GetMethodStackRef(tstate, PyStackRef_AsPyObjectBorrow(owner), name, &method.ref);
    if (is_meth) {
        /* We can bypass temporary bound method object.
           meth is unbound method and obj is self.
           meth | self | arg1 | ... | argN
         */
        assert(!PyStackRef_IsNull(method.ref)); // No errors on this branch
        self_or_null[0] = owner;  // Transfer ownership
        return _PyThreadState_PopCStackRefSteal(tstate, &method);
    }
    /* meth is not an unbound method (but a regular attr, or
       something was returned by a descriptor protocol).  Set
       the second element of the stack to NULL, to signal
       CALL that it's not a method call.
       meth | NULL | arg1 | ... | argN
    */
    PyStackRef_CLOSE(owner);
    self_or_null[0] = PyStackRef_NULL;
    return _PyThreadState_PopCStackRefSteal(tstate, &method);
}

#ifdef Py_DEBUG
void
_Py_assert_within_stack_bounds(
    _PyInterpreterFrame *frame, _PyStackRef *stack_pointer,
    const char *filename, int lineno
) {
    if (frame->owner == FRAME_OWNED_BY_INTERPRETER) {
        return;
    }
    int level = (int)(stack_pointer - _PyFrame_Stackbase(frame));
    if (level < 0) {
        printf("Stack underflow (depth = %d) at %s:%d\n", level, filename, lineno);
        fflush(stdout);
        abort();
    }
    int size = _PyFrame_GetCode(frame)->co_stacksize;
    if (level > size) {
        printf("Stack overflow (depth = %d) at %s:%d\n", level, filename, lineno);
        fflush(stdout);
        abort();
    }
}
#endif

int _Py_CheckRecursiveCallPy(
    PyThreadState *tstate)
{
    if (tstate->recursion_headroom) {
        if (tstate->py_recursion_remaining < -50) {
            /* Overflowing while handling an overflow. Give up. */
            Py_FatalError("Cannot recover from Python stack overflow.");
        }
    }
    else {
        if (tstate->py_recursion_remaining <= 0) {
            tstate->recursion_headroom++;
            _PyErr_Format(tstate, PyExc_RecursionError,
                        "maximum recursion depth exceeded");
            tstate->recursion_headroom--;
            return -1;
        }
    }
    return 0;
}

static const _Py_CODEUNIT _Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS[] = {
    /* Put a NOP at the start, so that the IP points into
    * the code, rather than before it */
    { .op.code = NOP, .op.arg = 0 },
    { .op.code = INTERPRETER_EXIT, .op.arg = 0 },  /* reached on return */
    { .op.code = NOP, .op.arg = 0 },
    { .op.code = INTERPRETER_EXIT, .op.arg = 0 },  /* reached on yield */
    { .op.code = RESUME, .op.arg = RESUME_OPARG_DEPTH1_MASK | RESUME_AT_FUNC_START }
};

const _Py_CODEUNIT *_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS_PTR = (_Py_CODEUNIT*)&_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS;

#ifdef Py_DEBUG
extern void _PyUOpPrint(const _PyUOpInstruction *uop);
#endif


PyObject **
_PyObjectArray_FromStackRefArray(_PyStackRef *input, Py_ssize_t nargs, PyObject **scratch)
{
    PyObject **result;
    if (nargs > MAX_STACKREF_SCRATCH) {
        // +1 in case PY_VECTORCALL_ARGUMENTS_OFFSET is set.
        result = PyMem_Malloc((nargs + 1) * sizeof(PyObject *));
        if (result == NULL) {
            return NULL;
        }
    }
    else {
        result = scratch;
    }
    result++;
    result[0] = NULL; /* Keep GCC happy */
    for (int i = 0; i < nargs; i++) {
        result[i] = PyStackRef_AsPyObjectBorrow(input[i]);
    }
    return result;
}

void
_PyObjectArray_Free(PyObject **array, PyObject **scratch)
{
    if (array != scratch) {
        PyMem_Free(array);
    }
}

#if _Py_TIER2
// 0 for success, -1  for error.
static int
stop_tracing_and_jit(PyThreadState *tstate, _PyInterpreterFrame *frame)
{
    int _is_sys_tracing = (tstate->c_tracefunc != NULL) || (tstate->c_profilefunc != NULL);
    int err = 0;
    if (!_PyErr_Occurred(tstate) && !_is_sys_tracing) {
        err = _PyOptimizer_Optimize(frame, tstate);
    }
    _PyJit_FinalizeTracing(tstate, err);
    return err;
}
#endif

/* _PyEval_EvalFrameDefault is too large to optimize for speed with PGO on MSVC.
 */
#if (defined(_MSC_VER) && \
     (_MSC_VER < 1943) && \
     defined(_Py_USING_PGO))
#define DO_NOT_OPTIMIZE_INTERP_LOOP
#endif

#ifdef DO_NOT_OPTIMIZE_INTERP_LOOP
#  pragma optimize("t", off)
/* This setting is reversed below following _PyEval_EvalFrameDefault */
#endif

#if _Py_TAIL_CALL_INTERP
#include "opcode_targets.h"
#include "generated_cases.c.h"
#endif

#if (defined(__GNUC__) && __GNUC__ >= 10 && !defined(__clang__)) && defined(__x86_64__)
/*
 * gh-129987: The SLP autovectorizer can cause poor code generation for
 * opcode dispatch in some GCC versions (observed in GCCs 12 through 15,
 * probably caused by https://gcc.gnu.org/bugzilla/show_bug.cgi?id=115777),
 * negating any benefit we get from vectorization elsewhere in the
 * interpreter loop. Disabling it significantly affected older GCC versions
 * (prior to GCC 9, 40% performance drop), so we have to selectively disable
 * it.
 */
#define DONT_SLP_VECTORIZE __attribute__((optimize ("no-tree-slp-vectorize")))
#else
#define DONT_SLP_VECTORIZE
#endif

PyObject* _Py_HOT_FUNCTION DONT_SLP_VECTORIZE
_PyEval_EvalFrameDefault(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    _Py_EnsureTstateNotNULL(tstate);
    if (_PyFrame_CheckSoacLifetimeExecution(frame) < 0) {
        return NULL;
    }
    check_invalid_reentrancy();
    CALL_STAT_INC(pyeval_calls);

#if USE_COMPUTED_GOTOS && !_Py_TAIL_CALL_INTERP
/* Import the static jump table */
#include "opcode_targets.h"
    void **opcode_targets = opcode_targets_table;
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
#if !_Py_TAIL_CALL_INTERP
    uint8_t opcode;    /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
    assert(tstate->current_frame == NULL || tstate->current_frame->stackpointer != NULL);
#if !USE_COMPUTED_GOTOS
    uint8_t tracing_mode = 0;
    uint8_t dispatch_code;
#endif
#endif
    _PyEntryFrame entry;

    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */
    _Py_CODEUNIT *next_instr;
    _PyStackRef *stack_pointer;
    entry.stack[0] = PyStackRef_NULL;
#ifdef Py_STACKREF_DEBUG
    entry.frame.f_funcobj = PyStackRef_None;
#elif defined(Py_DEBUG)
    /* Set these to invalid but identifiable values for debugging. */
    entry.frame.f_funcobj = (_PyStackRef){.bits = 0xaaa0};
    entry.frame.f_locals = (PyObject*)0xaaa1;
    entry.frame.frame_obj = (PyFrameObject*)0xaaa2;
    entry.frame.f_globals = (PyObject*)0xaaa3;
    entry.frame.f_builtins = (PyObject*)0xaaa4;
#endif
    entry.frame.f_executable = PyStackRef_None;
    entry.frame.instr_ptr = (_Py_CODEUNIT *)_Py_INTERPRETER_TRAMPOLINE_INSTRUCTIONS + 1;
    entry.frame.stackpointer = entry.stack;
    entry.frame.owner = FRAME_OWNED_BY_INTERPRETER;
    entry.frame.visited = 0;
    entry.frame.soac_dataclass_role = 0;
    entry.frame.soac_dataclass_invocation = NULL;
    entry.frame.soac_dataclass_checked_activation = NULL;
    entry.frame.return_offset = 0;
#ifdef Py_DEBUG
    entry.frame.lltrace = 0;
#endif
    /* Push frame */
    entry.frame.previous = tstate->current_frame;
    frame->previous = &entry.frame;
    tstate->current_frame = frame;
    entry.frame.localsplus[0] = PyStackRef_NULL;
#ifdef _Py_TIER2
    if (tstate->current_executor != NULL) {
        entry.frame.localsplus[0] = PyStackRef_FromPyObjectNew(tstate->current_executor);
        tstate->current_executor = NULL;
    }
#endif

    /* support for generator.throw() */
    if (throwflag) {
        if (_Py_EnterRecursivePy(tstate)) {
            goto early_exit;
        }
        if (_PyFrame_CheckSoacExecution(frame) < 0) {
            goto early_exit;
        }
#ifdef Py_GIL_DISABLED
        /* Load thread-local bytecode */
        if (frame->tlbc_index != ((_PyThreadStateImpl *)tstate)->tlbc_index) {
            _Py_CODEUNIT *bytecode =
                _PyEval_GetExecutableCode(tstate, _PyFrame_GetCode(frame));
            if (bytecode == NULL) {
                goto early_exit;
            }
            ptrdiff_t off = frame->instr_ptr - _PyFrame_GetBytecode(frame);
            frame->tlbc_index = ((_PyThreadStateImpl *)tstate)->tlbc_index;
            frame->instr_ptr = bytecode + off;
        }
#endif
        /* Because this avoids the RESUME, we need to update instrumentation */
        _Py_Instrument(_PyFrame_GetCode(frame), tstate->interp);
        next_instr = frame->instr_ptr;
        monitor_throw(tstate, frame, next_instr);
        stack_pointer = _PyFrame_GetStackPointer(frame);
#if _Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, instruction_funcptr_handler_table, 0, lastopcode);
#   else
        return _TAIL_CALL_error(frame, stack_pointer, tstate, next_instr, instruction_funcptr_handler_table, 0);
#   endif
#else
        goto error;
#endif
    }

#if _Py_TAIL_CALL_INTERP
#   if Py_STATS
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, instruction_funcptr_handler_table, 0, lastopcode);
#   else
        return _TAIL_CALL_start_frame(frame, NULL, tstate, NULL, instruction_funcptr_handler_table, 0);
#   endif
#else
    goto start_frame;
#   include "generated_cases.c.h"
#endif


early_exit:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallPy(tstate);
    assert(frame->owner != FRAME_OWNED_BY_INTERPRETER);
    // GH-99729: We need to unlink the frame *before* clearing it:
    _PyInterpreterFrame *dying = frame;
    frame = tstate->current_frame = dying->previous;
    _PyEval_FrameClearAndPop(tstate, dying);
    frame->return_offset = 0;
    assert(frame->owner == FRAME_OWNED_BY_INTERPRETER);
    /* Restore previous frame and exit */
    tstate->current_frame = frame->previous;
    return NULL;
}
#ifdef _Py_TIER2
#ifdef _Py_JIT
_PyJitEntryFuncPtr _Py_jit_entry = _Py_LazyJitShim;
#else
_PyJitEntryFuncPtr _Py_jit_entry = _PyTier2Interpreter;
#endif
#endif

#if defined(_Py_TIER2) && !defined(_Py_JIT)

_Py_CODEUNIT *
_PyTier2Interpreter(
    _PyExecutorObject *current_executor, _PyInterpreterFrame *frame,
    _PyStackRef *stack_pointer, PyThreadState *tstate
) {
    const _PyUOpInstruction *next_uop;
    int oparg;
    /* Set up "jit" state after entry from tier 1.
     * This mimics what the jit shim function does. */
    tstate->jit_exit = NULL;
    _PyStackRef _tos_cache0 = PyStackRef_ZERO_BITS;
    _PyStackRef _tos_cache1 = PyStackRef_ZERO_BITS;
    _PyStackRef _tos_cache2 = PyStackRef_ZERO_BITS;
    int current_cached_values = 0;

tier2_start:

    next_uop = current_executor->trace;
    assert(next_uop->opcode == _START_EXECUTOR_r00 + current_cached_values ||
        next_uop->opcode == _COLD_EXIT_r00 + current_cached_values ||
        next_uop->opcode == _COLD_DYNAMIC_EXIT_r00 + current_cached_values);

#undef LOAD_IP
#define LOAD_IP(UNUSED) (void)0

#ifdef Py_STATS
// Disable these macros that apply to Tier 1 stats when we are in Tier 2
#undef STAT_INC
#define STAT_INC(opname, name) ((void)0)
#undef STAT_DEC
#define STAT_DEC(opname, name) ((void)0)
#endif

#undef ENABLE_SPECIALIZATION
#define ENABLE_SPECIALIZATION 0

    uint16_t uopcode;
#ifdef Py_STATS
    int lastuop = 0;
    uint64_t trace_uop_execution_counter = 0;
#endif

    assert(next_uop->opcode == _START_EXECUTOR_r00 ||
        next_uop->opcode == _COLD_EXIT_r00 ||
        next_uop->opcode == _COLD_DYNAMIC_EXIT_r00);
tier2_dispatch:
    for (;;) {
        uopcode = next_uop->opcode;
#ifdef Py_DEBUG
        if (frame->lltrace >= 3) {
            dump_stack(frame, stack_pointer);
            printf("    cache=[");
            dump_cache_item(_tos_cache0, 0, current_cached_values);
            printf(", ");
            dump_cache_item(_tos_cache1, 1, current_cached_values);
            printf(", ");
            dump_cache_item(_tos_cache2, 2, current_cached_values);
            printf("]\n");
            if (next_uop->opcode == _START_EXECUTOR_r00) {
                printf("%4d uop: ", 0);
            }
            else {
                printf("%4d uop: ", (int)(next_uop - current_executor->trace));
            }
            _PyUOpPrint(next_uop);
            printf("\n");
            fflush(stdout);
        }
#endif
        next_uop++;
        OPT_STAT_INC(uops_executed);
        UOP_STAT_INC(uopcode, execution_count);
        UOP_PAIR_INC(uopcode, lastuop);
#ifdef Py_STATS
        trace_uop_execution_counter++;
        ((_PyUOpInstruction  *)next_uop)[-1].execution_count++;
#endif

        switch (uopcode) {

#include "executor_cases.c.h"

            default:
#ifdef Py_DEBUG
            {
                printf("Unknown uop: ");
                _PyUOpPrint(&next_uop[-1]);
                printf(" @ %d\n", (int)(next_uop - current_executor->trace - 1));
                Py_FatalError("Unknown uop");
            }
#else
            Py_UNREACHABLE();
#endif

        }
    }

jump_to_error_target:
#ifdef Py_DEBUG
    if (frame->lltrace >= 2) {
        printf("Error: [UOp ");
        _PyUOpPrint(&next_uop[-1]);
        printf(" @ %d -> %s]\n",
               (int)(next_uop - current_executor->trace - 1),
               _PyOpcode_OpName[frame->instr_ptr->op.code]);
        fflush(stdout);
    }
#endif
    assert(next_uop[-1].format == UOP_FORMAT_JUMP);
    uint16_t target = uop_get_error_target(&next_uop[-1]);
    next_uop = current_executor->trace + target;
    goto tier2_dispatch;

jump_to_jump_target:
    assert(next_uop[-1].format == UOP_FORMAT_JUMP);
    target = uop_get_jump_target(&next_uop[-1]);
    next_uop = current_executor->trace + target;
    goto tier2_dispatch;

}
#endif // _Py_TIER2


#ifdef DO_NOT_OPTIMIZE_INTERP_LOOP
#  pragma optimize("", on)
#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER) /* MS_WINDOWS */
#  pragma warning(pop)
#endif

static void
format_missing(PyThreadState *tstate, const char *kind,
               PyCodeObject *co, PyObject *names, PyObject *qualname)
{
    int err;
    Py_ssize_t len = PyList_GET_SIZE(names);
    PyObject *name_str, *comma, *tail, *tmp;

    assert(PyList_CheckExact(names));
    assert(len >= 1);
    /* Deal with the joys of natural language. */
    switch (len) {
    case 1:
        name_str = PyList_GET_ITEM(names, 0);
        Py_INCREF(name_str);
        break;
    case 2:
        name_str = PyUnicode_FromFormat("%U and %U",
                                        PyList_GET_ITEM(names, len - 2),
                                        PyList_GET_ITEM(names, len - 1));
        break;
    default:
        tail = PyUnicode_FromFormat(", %U, and %U",
                                    PyList_GET_ITEM(names, len - 2),
                                    PyList_GET_ITEM(names, len - 1));
        if (tail == NULL)
            return;
        /* Chop off the last two objects in the list. This shouldn't actually
           fail, but we can't be too careful. */
        err = PyList_SetSlice(names, len - 2, len, NULL);
        if (err == -1) {
            Py_DECREF(tail);
            return;
        }
        /* Stitch everything up into a nice comma-separated list. */
        comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            Py_DECREF(tail);
            return;
        }
        tmp = PyUnicode_Join(comma, names);
        Py_DECREF(comma);
        if (tmp == NULL) {
            Py_DECREF(tail);
            return;
        }
        name_str = PyUnicode_Concat(tmp, tail);
        Py_DECREF(tmp);
        Py_DECREF(tail);
        break;
    }
    if (name_str == NULL)
        return;
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() missing %i required %s argument%s: %U",
                  qualname,
                  len,
                  kind,
                  len == 1 ? "" : "s",
                  name_str);
    Py_DECREF(name_str);
}

static void
missing_arguments(PyThreadState *tstate, PyCodeObject *co,
                  Py_ssize_t missing, Py_ssize_t defcount,
                  _PyStackRef *localsplus, PyObject *qualname)
{
    Py_ssize_t i, j = 0;
    Py_ssize_t start, end;
    int positional = (defcount != -1);
    const char *kind = positional ? "positional" : "keyword-only";
    PyObject *missing_names;

    /* Compute the names of the arguments that are missing. */
    missing_names = PyList_New(missing);
    if (missing_names == NULL)
        return;
    if (positional) {
        start = 0;
        end = co->co_argcount - defcount;
    }
    else {
        start = co->co_argcount;
        end = start + co->co_kwonlyargcount;
    }
    for (i = start; i < end; i++) {
        if (PyStackRef_IsNull(localsplus[i])) {
            PyObject *raw = PyTuple_GET_ITEM(co->co_localsplusnames, i);
            PyObject *name = PyObject_Repr(raw);
            if (name == NULL) {
                Py_DECREF(missing_names);
                return;
            }
            PyList_SET_ITEM(missing_names, j++, name);
        }
    }
    assert(j == missing);
    format_missing(tstate, kind, co, missing_names, qualname);
    Py_DECREF(missing_names);
}

static void
too_many_positional(PyThreadState *tstate, PyCodeObject *co,
                    Py_ssize_t given, PyObject *defaults,
                    _PyStackRef *localsplus, PyObject *qualname)
{
    int plural;
    Py_ssize_t kwonly_given = 0;
    Py_ssize_t i;
    PyObject *sig, *kwonly_sig;
    Py_ssize_t co_argcount = co->co_argcount;

    assert((co->co_flags & CO_VARARGS) == 0);
    /* Count missing keyword-only args. */
    for (i = co_argcount; i < co_argcount + co->co_kwonlyargcount; i++) {
        if (PyStackRef_AsPyObjectBorrow(localsplus[i]) != NULL) {
            kwonly_given++;
        }
    }
    Py_ssize_t defcount = defaults == NULL ? 0 : PyTuple_GET_SIZE(defaults);
    if (defcount) {
        Py_ssize_t atleast = co_argcount - defcount;
        plural = 1;
        sig = PyUnicode_FromFormat("from %zd to %zd", atleast, co_argcount);
    }
    else {
        plural = (co_argcount != 1);
        sig = PyUnicode_FromFormat("%zd", co_argcount);
    }
    if (sig == NULL)
        return;
    if (kwonly_given) {
        const char *format = " positional argument%s (and %zd keyword-only argument%s)";
        kwonly_sig = PyUnicode_FromFormat(format,
                                          given != 1 ? "s" : "",
                                          kwonly_given,
                                          kwonly_given != 1 ? "s" : "");
        if (kwonly_sig == NULL) {
            Py_DECREF(sig);
            return;
        }
    }
    else {
        /* This will not fail. */
        kwonly_sig = Py_GetConstant(Py_CONSTANT_EMPTY_STR);
        assert(kwonly_sig != NULL);
    }
    _PyErr_Format(tstate, PyExc_TypeError,
                  "%U() takes %U positional argument%s but %zd%U %s given",
                  qualname,
                  sig,
                  plural ? "s" : "",
                  given,
                  kwonly_sig,
                  given == 1 && !kwonly_given ? "was" : "were");
    Py_DECREF(sig);
    Py_DECREF(kwonly_sig);
}

static int
positional_only_passed_as_keyword(PyThreadState *tstate, PyCodeObject *co,
                                  Py_ssize_t kwcount, PyObject* kwnames,
                                  PyObject *qualname)
{
    int posonly_conflicts = 0;
    PyObject* posonly_names = PyList_New(0);
    if (posonly_names == NULL) {
        goto fail;
    }
    for(int k=0; k < co->co_posonlyargcount; k++){
        PyObject* posonly_name = PyTuple_GET_ITEM(co->co_localsplusnames, k);

        for (int k2=0; k2<kwcount; k2++){
            /* Compare the pointers first and fallback to PyObject_RichCompareBool*/
            PyObject* kwname = PyTuple_GET_ITEM(kwnames, k2);
            if (kwname == posonly_name){
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
                continue;
            }

            int cmp = PyObject_RichCompareBool(posonly_name, kwname, Py_EQ);

            if ( cmp > 0) {
                if(PyList_Append(posonly_names, kwname) != 0) {
                    goto fail;
                }
                posonly_conflicts++;
            } else if (cmp < 0) {
                goto fail;
            }

        }
    }
    if (posonly_conflicts) {
        PyObject* comma = PyUnicode_FromString(", ");
        if (comma == NULL) {
            goto fail;
        }
        PyObject* error_names = PyUnicode_Join(comma, posonly_names);
        Py_DECREF(comma);
        if (error_names == NULL) {
            goto fail;
        }
        _PyErr_Format(tstate, PyExc_TypeError,
                      "%U() got some positional-only arguments passed"
                      " as keyword arguments: '%U'",
                      qualname, error_names);
        Py_DECREF(error_names);
        goto fail;
    }

    Py_DECREF(posonly_names);
    return 0;

fail:
    Py_XDECREF(posonly_names);
    return 1;

}

static int
initialize_locals(PyThreadState *tstate, PyFunctionObject *func, PyCodeObject *co,
    _PyStackRef *localsplus, _PyStackRef const *args,
    Py_ssize_t argcount, PyObject *kwnames, unsigned char *soac_supplied)
{
    /* Execute/bind the native frame's captured code even if argument
     * comparison callbacks replace the function's current code. */
    const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount;
    /* Create a dictionary for keyword parameters (**kwags) */
    PyObject *kwdict;
    Py_ssize_t i;
    if (co->co_flags & CO_VARKEYWORDS) {
        kwdict = PyDict_New();
        if (kwdict == NULL) {
            goto fail_pre_positional;
        }
        i = total_args;
        if (co->co_flags & CO_VARARGS) {
            i++;
        }
        assert(PyStackRef_IsNull(localsplus[i]));
        localsplus[i] = PyStackRef_FromPyObjectSteal(kwdict);
    }
    else {
        kwdict = NULL;
    }

    /* Copy all positional arguments into local variables */
    Py_ssize_t j, n;
    if (argcount > co->co_argcount) {
        n = co->co_argcount;
    }
    else {
        n = argcount;
    }
    for (j = 0; j < n; j++) {
        assert(PyStackRef_IsNull(localsplus[j]));
        localsplus[j] = args[j];
    }

    /* Pack other positional arguments into the *args argument */
    if (co->co_flags & CO_VARARGS) {
        PyObject *u = NULL;
        if (argcount == n) {
            u = (PyObject *)&_Py_SINGLETON(tuple_empty);
        }
        else {
            u = _PyTuple_FromStackRefStealOnSuccess(args + n, argcount - n);
            if (u == NULL) {
                for (Py_ssize_t i = n; i < argcount; i++) {
                    PyStackRef_CLOSE(args[i]);
                }
            }
        }
        if (u == NULL) {
            goto fail_post_positional;
        }
        assert(PyStackRef_AsPyObjectBorrow(localsplus[total_args]) == NULL);
        localsplus[total_args] = PyStackRef_FromPyObjectSteal(u);
    }
    else if (argcount > n) {
        /* Too many positional args. Error is reported later */
        for (j = n; j < argcount; j++) {
            PyStackRef_CLOSE(args[j]);
        }
    }

    /* Handle keyword arguments */
    if (kwnames != NULL) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < kwcount; i++) {
            PyObject **co_varnames;
            PyObject *keyword = PyTuple_GET_ITEM(kwnames, i);
            _PyStackRef value_stackref = args[i+argcount];
            Py_ssize_t j;

            if (keyword == NULL || !PyUnicode_Check(keyword)) {
                _PyErr_Format(tstate, PyExc_TypeError,
                            "%U() keywords must be strings",
                          func->func_qualname);
                goto kw_fail;
            }

            /* Speed hack: do raw pointer compares. As names are
            normally interned this should almost always hit. */
            co_varnames = ((PyTupleObject *)(co->co_localsplusnames))->ob_item;
            for (j = co->co_posonlyargcount; j < total_args; j++) {
                PyObject *varname = co_varnames[j];
                if (varname == keyword) {
                    goto kw_found;
                }
            }

            /* Slow fallback, just in case */
            for (j = co->co_posonlyargcount; j < total_args; j++) {
                PyObject *varname = co_varnames[j];
                int cmp = PyObject_RichCompareBool( keyword, varname, Py_EQ);
                if (cmp > 0) {
                    goto kw_found;
                }
                else if (cmp < 0) {
                    goto kw_fail;
                }
            }

            assert(j >= total_args);
            if (kwdict == NULL) {

                if (co->co_posonlyargcount
                    && positional_only_passed_as_keyword(tstate, co,
                                                        kwcount, kwnames,
                                                        func->func_qualname))
                {
                    goto kw_fail;
                }

                PyObject* suggestion_keyword = NULL;
                if (total_args > co->co_posonlyargcount) {
                    PyObject* possible_keywords = PyList_New(total_args - co->co_posonlyargcount);

                    if (!possible_keywords) {
                        PyErr_Clear();
                    } else {
                        for (Py_ssize_t k = co->co_posonlyargcount; k < total_args; k++) {
                            PyList_SET_ITEM(possible_keywords, k - co->co_posonlyargcount, co_varnames[k]);
                        }

                        suggestion_keyword = _Py_CalculateSuggestions(possible_keywords, keyword);
                        Py_DECREF(possible_keywords);
                    }
                }

                if (suggestion_keyword) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                "%U() got an unexpected keyword argument '%S'. Did you mean '%S'?",
                                func->func_qualname, keyword, suggestion_keyword);
                    Py_DECREF(suggestion_keyword);
                } else {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                "%U() got an unexpected keyword argument '%S'",
                                func->func_qualname, keyword);
                }

                goto kw_fail;
            }

            if (PyDict_SetItem(kwdict, keyword, PyStackRef_AsPyObjectBorrow(value_stackref)) == -1) {
                goto kw_fail;
            }
            PyStackRef_CLOSE(value_stackref);
            continue;

        kw_fail:
            for (;i < kwcount; i++) {
                PyStackRef_CLOSE(args[i+argcount]);
            }
            goto fail_post_args;

        kw_found:
            if (PyStackRef_AsPyObjectBorrow(localsplus[j]) != NULL) {
                _PyErr_Format(tstate, PyExc_TypeError,
                            "%U() got multiple values for argument '%S'",
                          func->func_qualname, keyword);
                goto kw_fail;
            }
            localsplus[j] = value_stackref;
        }
    }

    /* Check the number of positional arguments */
    if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
        too_many_positional(tstate, co, argcount, func->func_defaults, localsplus,
                            func->func_qualname);
        goto fail_post_args;
    }

    /* The native binder, not a second name lookup or a sentinel comparison,
     * decides which actual slots came from this caller. No callback occurs
     * between this snapshot and beginning ordinary default insertion. */
    if (soac_supplied != NULL) {
        for (i = 0; i < total_args; i++) {
            soac_supplied[i] = !PyStackRef_IsNull(localsplus[i]);
        }
    }

    /* Add missing positional arguments (copy default values from defs) */
    if (argcount < co->co_argcount) {
        Py_ssize_t defcount = func->func_defaults == NULL ? 0 : PyTuple_GET_SIZE(func->func_defaults);
        Py_ssize_t m = co->co_argcount - defcount;
        Py_ssize_t missing = 0;
        for (i = argcount; i < m; i++) {
            if (PyStackRef_IsNull(localsplus[i])) {
                missing++;
            }
        }
        if (missing) {
            missing_arguments(tstate, co, missing, defcount, localsplus,
                              func->func_qualname);
            goto fail_post_args;
        }
        if (n > m)
            i = n - m;
        else
            i = 0;
        if (defcount) {
            PyObject **defs = &PyTuple_GET_ITEM(func->func_defaults, 0);
            for (; i < defcount; i++) {
                if (PyStackRef_AsPyObjectBorrow(localsplus[m+i]) == NULL) {
                    PyObject *def = defs[i];
                    localsplus[m+i] = PyStackRef_FromPyObjectNew(def);
                }
            }
        }
    }

    /* Add missing keyword arguments (copy default values from kwdefs) */
    if (co->co_kwonlyargcount > 0) {
        Py_ssize_t missing = 0;
        for (i = co->co_argcount; i < total_args; i++) {
            if (PyStackRef_AsPyObjectBorrow(localsplus[i]) != NULL)
                continue;
            PyObject *varname = PyTuple_GET_ITEM(co->co_localsplusnames, i);
            if (func->func_kwdefaults != NULL) {
                /* Equality can replace func_kwdefaults and release its last
                 * reference. Keep this lookup's mapping alive, then reload
                 * the function field for the next missing parameter. */
                PyObject *kwdefaults = Py_NewRef(func->func_kwdefaults);
                PyObject *def;
                int found = PyDict_GetItemRef(kwdefaults, varname, &def);
                Py_DECREF(kwdefaults);
                if (found < 0) {
                    goto fail_post_args;
                }
                if (def) {
                    localsplus[i] = PyStackRef_FromPyObjectSteal(def);
                    continue;
                }
            }
            missing++;
        }
        if (missing) {
            missing_arguments(tstate, co, missing, -1, localsplus,
                              func->func_qualname);
            goto fail_post_args;
        }
    }
    return 0;

fail_pre_positional:
    for (j = 0; j < argcount; j++) {
        PyStackRef_CLOSE(args[j]);
    }
    /* fall through */
fail_post_positional:
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (j = argcount; j < argcount+kwcount; j++) {
            PyStackRef_CLOSE(args[j]);
        }
    }
    /* fall through */
fail_post_args:
    return -1;
}

static void
clear_thread_frame(PyThreadState *tstate, _PyInterpreterFrame * frame)
{
    assert(frame->owner == FRAME_OWNED_BY_THREAD);
    // Make sure that this is, indeed, the top frame. We can't check this in
    // _PyThreadState_PopFrame, since f_code is already cleared at that point:
    assert((PyObject **)frame + _PyFrame_GetCode(frame)->co_framesize ==
        tstate->datastack_top);
    assert(frame->frame_obj == NULL || frame->frame_obj->f_frame == frame);
    _PyFrame_ClearExceptCode(frame);
    PyStackRef_CLEAR(frame->f_executable);
    _PyThreadState_PopFrame(tstate, frame);
}

/* Native source-call binding/view transaction.
 * Opaque reference operations use the matching native primitive layer;
 * registration and call-site dispatch remain separate requirements.
 */

_Static_assert(sizeof(PySoacRefV1) == sizeof(_PyStackRef),
               "binding transfer must preserve the native reference");
_Static_assert(_Alignof(PySoacRefV1) == _Alignof(_PyStackRef),
               "binding transfer must preserve native reference alignment");

static int
soac_call_error(const char *message)
{
    PyErr_SetString(PyExc_ValueError, message);
    return -1;
}

static int
soac_call_supported(void)
{
#if defined(Py_GIL_DISABLED) || SIZEOF_VOID_P != 8
    PyErr_SetString(PyExc_NotImplementedError,
                    "SOAC source binding V1 requires the 64-bit GIL build");
    return 0;
#else
    assert(PyGILState_Check());
    return 1;
#endif
}

static void
soac_call_require_supported(void)
{
    if (!soac_call_supported()) {
        Py_FatalError("SOAC binding producer used without a supported ABI query");
    }
}

static int
soac_call_range(const void *ptr, size_t bytes, size_t alignment)
{
    if (bytes == 0) {
        return 1;
    }
    uintptr_t start = (uintptr_t)ptr;
    return ptr != NULL && start % alignment == 0 &&
           bytes <= UINTPTR_MAX - start;
}

static int
soac_call_overlaps(const void *left, size_t left_size,
                   const void *right, size_t right_size)
{
    if (left_size == 0 || right_size == 0) {
        return 0;
    }
    /* All caller ranges have already passed the overflow check. Private view
     * and datastack ranges are valid C allocations owned by this producer. */
    uintptr_t a = (uintptr_t)left;
    uintptr_t b = (uintptr_t)right;
    return a < b + right_size && b < a + left_size;
}

static int
soac_call_aliases_primaries(const PySoacBoundCallViewV1 *view,
                           const void *ptr, size_t bytes, int mask_is_output)
{
    return soac_call_overlaps(ptr, bytes, view, sizeof(*view)) ||
           soac_call_overlaps(ptr, bytes, view->frame,
                              view->frame_size * sizeof(PyObject *)) ||
           (!mask_is_output &&
            soac_call_overlaps(ptr, bytes, view->supplied,
                               (size_t)view->info.supplied_count));
}

/* Validate before dereferencing any borrowed frame/code fields. Terminal and
 * copied views fail at their own header, without following dead pointers. */
static int
soac_call_validate(const PySoacBoundCallViewV1 *view, int require_ready)
{
    if (!soac_call_supported()) {
        return -1;
    }
    if (!soac_call_range(view, sizeof(*view), _Alignof(PySoacBoundCallViewV1)) ||
        view->self != view || view->abi_version != Py_SOAC_SOURCE_ENTRY_ABI_V1 ||
        (view->state != _Py_SOAC_CALL_BOUND_V1 &&
         view->state != _Py_SOAC_CALL_BODY_READY_V1) ||
        (require_ready && view->state != _Py_SOAC_CALL_BODY_READY_V1)) {
        return soac_call_error("invalid, terminal, or not-BodyReady SOAC call view");
    }
    PyThreadState *thread = _PyThreadState_GET();
    if (thread != view->thread || thread->current_frame != view->caller) {
        return soac_call_error("SOAC call view used on a different thread or parent");
    }
    _PyInterpreterFrame *frame = view->frame;
    if (frame == NULL || (PyObject **)frame + view->frame_size != thread->datastack_top) {
        return soac_call_error("SOAC call view is not the top temporary frame");
    }
    if (frame == thread->current_frame || frame->owner != FRAME_OWNED_BY_THREAD ||
        frame->frame_obj != NULL || frame->previous != view->caller ||
        PyStackRef_AsPyObjectBorrow(frame->f_funcobj) != view->info.function ||
        PyStackRef_AsPyObjectBorrow(frame->f_executable) != (PyObject *)view->info.code ||
        frame->f_globals != view->info.globals || frame->f_builtins != view->info.builtins ||
        frame->f_locals != NULL || frame->soac_dataclass_role != 0 ||
        frame->soac_dataclass_invocation != NULL ||
        frame->soac_dataclass_checked_activation != NULL ||
        frame->stackpointer != frame->localsplus + view->info.code->co_nlocalsplus) {
        return soac_call_error("SOAC temporary binding frame changed before transfer");
    }
    for (Py_ssize_t i = view->info.parameter_count;
         i < view->info.code->co_nlocalsplus; i++) {
        if (!PyStackRef_IsNull(frame->localsplus[i])) {
            return soac_call_error("SOAC binding frame executed before transfer");
        }
    }
    if (view->state == _Py_SOAC_CALL_BODY_READY_V1 && view->caller != NULL &&
        view->caller->stackpointer != view->ready_stackpointer) {
        return soac_call_error("SOAC caller stack changed after BodyReady");
    }
    return 0;
}

static void
soac_call_publish_terminal(PySoacBoundCallViewV1 *view, uint32_t state)
{
    view->state = state;
    view->thread = NULL;
    view->frame = NULL;
    view->caller = NULL;
    view->ready_stackpointer = NULL;
    view->frame_size = 0;
    view->info = (PySoacSourceCallInfoV1){0};
    view->supplied = NULL;
    assert(PySoacRef_IsEmptyV1(view->namespace_value));
}

/* Both Bound Abort and native binding failure use the SAME native clear. The
 * namespace is still raw if binding failed before its token was prepared.
 * Publish terminal and detach the prepared token before any Python teardown.
 */
static void
soac_call_abort_frame(PySoacBoundCallViewV1 *view)
{
    PyThreadState *thread = view->thread;
    _PyInterpreterFrame *frame = view->frame;
    PySoacRefV1 namespace_value = PySoacRef_TakeV1(&view->namespace_value);
    soac_call_publish_terminal(view, _Py_SOAC_CALL_ABORTED_V1);
    PyObject *pending = PyErr_GetRaisedException();
    if (!PySoacRef_IsEmptyV1(namespace_value)) {
        assert(frame->f_locals == NULL);
        /* This token was created from the already-owned f_locals reference,
         * never from Borrow. Restore it without an INCREF, even in debug. */
        frame->f_locals = PySoacRef_IntoOwnedV1(namespace_value);
    }
    frame->previous = NULL;
    clear_thread_frame(thread, frame);  /* locals -> namespace -> func -> code */
    PyErr_SetRaisedException(pending);
}

void
_PySoacCall_CaptureInfoV1(PySoacSourceCallInfoV1 *out, uint32_t kind,
                        _PyStackRef function, PyCodeObject *code)
{
    soac_call_require_supported();
    assert(out != NULL && kind <= Py_SOAC_CALL_VM_EXPANDED_V1);
    PyFunctionObject *func = (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(function);
    assert(PyFunction_Check(func) && PyCode_Check(code));
    assert(func->func_code == (PyObject *)code);
    Py_ssize_t supplied = (Py_ssize_t)code->co_argcount + code->co_kwonlyargcount;
    Py_ssize_t parameters = supplied + !!(code->co_flags & CO_VARARGS) +
                           !!(code->co_flags & CO_VARKEYWORDS);
    assert(parameters <= code->co_nlocalsplus);
    *out = (PySoacSourceCallInfoV1){
        .kind = kind,
        .function = (PyObject *)func,
        .code = code,
        .globals = func->func_globals,
        .builtins = func->func_builtins,
        .parameter_count = parameters,
        .supplied_count = supplied,
    };
}

int
_PySoacCall_BindV1(PySoacBoundCallViewV1 *view, PyThreadState *thread,
                 const PySoacSourceCallInfoV1 *captured,
                 _PyStackRef function, PyObject *locals,
                 const _PyStackRef *args, Py_ssize_t argcount, PyObject *kwnames,
                 unsigned char *supplied, Py_ssize_t supplied_count)
{
    soac_call_require_supported();
    assert(view != NULL && thread == _PyThreadState_GET() && captured != NULL);
    assert(captured->reserved == 0 && captured->kind <= Py_SOAC_CALL_VM_EXPANDED_V1);
    assert(argcount >= 0 && (kwnames == NULL || PyTuple_Check(kwnames)));
    assert(argcount <= PY_SSIZE_T_MAX - (kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames)));
    assert(args != NULL || (argcount == 0 &&
           (kwnames == NULL || PyTuple_GET_SIZE(kwnames) == 0)));
    assert(supplied_count == captured->supplied_count &&
           (supplied_count == 0 || supplied != NULL));
    PyFunctionObject *func = (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(function);
    PyCodeObject *code = captured->code;
    assert((PyObject *)func == captured->function && PyFunction_Check(func));
    assert(func->func_code == (PyObject *)code && PyCode_Check(code));
    assert(func->func_globals == captured->globals && func->func_builtins == captured->builtins);
    *view = (PySoacBoundCallViewV1){
        .self = view,
        .abi_version = Py_SOAC_SOURCE_ENTRY_ABI_V1,
        .state = _Py_SOAC_CALL_BINDING_V1,
        .thread = thread,
        .caller = thread->current_frame,
        .frame_size = (size_t)code->co_framesize,
        .info = *captured,
        .supplied = supplied,
        .namespace_value = PySoacRef_EmptyV1(),
    };
    CALL_STAT_INC(frames_pushed);
    _PyInterpreterFrame *frame = _PyThreadState_PushFrame(thread, code->co_framesize);
    if (frame == NULL) {
        soac_call_publish_terminal(view, _Py_SOAC_CALL_ABORTED_V1);
        /* Match the existing native allocation-failure ownership order. There
         * is no frame/code owner yet and no initialized parameter to inspect. */
        PyStackRef_CLOSE(function);
        Py_XDECREF(locals);
        Py_ssize_t count = argcount + (kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames));
        for (Py_ssize_t i = 0; i < count; i++) {
            PyStackRef_CLOSE(args[i]);
        }
        PyErr_NoMemory();
        return -1;
    }
    view->frame = frame;
    _PyFrame_Initialize(thread, frame, function, locals, code, 0, view->caller);
    if (initialize_locals(thread, func, code, frame->localsplus, args,
                          argcount, kwnames, supplied) < 0) {
        soac_call_abort_frame(view);
        return -1;
    }
    /* Token creation can allocate a debug handle; do it BEFORE BodyReady, not
     * in Take. This moves the existing raw owner without an extra refcount.
     * On pre-staging failure, Abort's Empty token leaves raw f_locals intact. */
    if (frame->f_locals != NULL) {
        view->namespace_value = PySoacRef_FromOwnedV1(frame->f_locals);
        frame->f_locals = NULL;
    }
    view->state = _Py_SOAC_CALL_BOUND_V1;
    return 0;
}

int
_PySoacCall_MarkBodyReadyV1(PySoacBoundCallViewV1 *view,
                          _PyStackRef *post_retirement_stackpointer)
{
    if (soac_call_validate(view, 0) < 0) {
        return -1;
    }
    if (view->state != _Py_SOAC_CALL_BOUND_V1 ||
        (view->caller == NULL && post_retirement_stackpointer != NULL) ||
        (view->caller != NULL && (post_retirement_stackpointer == NULL ||
         view->caller->stackpointer != post_retirement_stackpointer))) {
        return soac_call_error("SOAC BodyReady requires the published post-retirement stack");
    }
    view->ready_stackpointer = post_retirement_stackpointer;
    view->state = _Py_SOAC_CALL_BODY_READY_V1;
    return 0;
}

int
PySoacCall_GetInfoV1(const PySoacBoundCallViewV1 *view,
                   PySoacSourceCallInfoV1 *out, size_t out_size)
{
    if (soac_call_validate(view, 1) < 0) {
        return -1;
    }
    if (out_size != sizeof(*out) ||
        !soac_call_range(out, sizeof(*out), _Alignof(PySoacSourceCallInfoV1)) ||
        soac_call_aliases_primaries(view, out, sizeof(*out), 0)) {
        return soac_call_error("invalid SOAC source-call info output");
    }
    *out = view->info;
    return 0;
}

static PySoacRefV1
soac_call_move_native(_PyStackRef *slot)
{
    PySoacRefV1 result;
    memcpy(&result, slot, sizeof(result));  /* transport, not semantic DUP */
    *slot = PyStackRef_NULL;
    return result;
}

int
PySoacCall_TakeBindingV1(PySoacBoundCallViewV1 *view,
                       PySoacBoundActivationV1 *activation, size_t activation_size,
                       PySoacRefV1 *parameters, Py_ssize_t parameter_count,
                       unsigned char *supplied, Py_ssize_t supplied_count)
{
    if (soac_call_validate(view, 1) < 0) {
        return -1;
    }
    if (activation_size != sizeof(*activation) ||
        parameter_count != view->info.parameter_count ||
        supplied_count != view->info.supplied_count ||
        parameter_count < 0 || supplied_count < 0 ||
        (size_t)parameter_count > SIZE_MAX / sizeof(*parameters)) {
        return soac_call_error("invalid SOAC binding output sizes");
    }
    size_t parameter_bytes = (size_t)parameter_count * sizeof(*parameters);
    size_t supplied_bytes = (size_t)supplied_count;
    if (!soac_call_range(activation, sizeof(*activation), _Alignof(PySoacBoundActivationV1)) ||
        !soac_call_range(parameters, parameter_bytes, _Alignof(PySoacRefV1)) ||
        !soac_call_range(supplied, supplied_bytes, 1) ||
        soac_call_aliases_primaries(view, activation, sizeof(*activation), 0) ||
        soac_call_aliases_primaries(view, parameters, parameter_bytes, 0) ||
        soac_call_aliases_primaries(view, supplied, supplied_bytes, 1) ||
        soac_call_overlaps(activation, sizeof(*activation), parameters, parameter_bytes) ||
        soac_call_overlaps(activation, sizeof(*activation), supplied, supplied_bytes) ||
        soac_call_overlaps(parameters, parameter_bytes, supplied, supplied_bytes)) {
        return soac_call_error("invalid or overlapping SOAC binding output buffers");
    }
    if (!PySoacRef_IsEmptyV1(activation->function) ||
        !PySoacRef_IsEmptyV1(activation->code) ||
        !PySoacRef_IsEmptyV1(activation->namespace_value)) {
        return soac_call_error("SOAC binding activation outputs must be Empty");
    }
    for (Py_ssize_t i = 0; i < parameter_count; i++) {
        if (!PySoacRef_IsEmptyV1(parameters[i])) {
            return soac_call_error("SOAC binding parameter outputs must be Empty");
        }
    }
    /* No fallible operation, allocation, callback, semantic DUP or close below.
     * All source primaries move once, including parameters never read by code. */
    _PyInterpreterFrame *frame = view->frame;
    PyThreadState *thread = view->thread;
    activation->globals = view->info.globals;
    activation->builtins = view->info.builtins;
    if (supplied_count != 0) {
        memmove(supplied, view->supplied, supplied_bytes);
    }
    for (Py_ssize_t i = 0; i < parameter_count; i++) {
        parameters[i] = soac_call_move_native(&frame->localsplus[i]);
    }
    activation->function = soac_call_move_native(&frame->f_funcobj);
    activation->code = soac_call_move_native(&frame->f_executable);
    activation->namespace_value = PySoacRef_TakeV1(&view->namespace_value);
    frame->f_globals = NULL;
    frame->f_builtins = NULL;
    frame->previous = NULL;
    frame->stackpointer = frame->localsplus;
    soac_call_publish_terminal(view, _Py_SOAC_CALL_TAKEN_V1);
    _PyThreadState_PopFrame(thread, frame);
    return 0;
}

int
PySoacCall_AbortV1(PySoacBoundCallViewV1 *view)
{
    if (soac_call_validate(view, 0) < 0) {
        return -1;
    }
    soac_call_abort_frame(view);
    return 0;
}

static void
clear_gen_frame(PyThreadState *tstate, _PyInterpreterFrame * frame)
{
    assert(frame->owner == FRAME_OWNED_BY_GENERATOR);
    PyGenObject *gen = _PyGen_GetGeneratorFromFrame(frame);
    FT_ATOMIC_STORE_INT8_RELEASE(gen->gi_frame_state, FRAME_CLEARED);
    assert(tstate->exc_info == &gen->gi_exc_state);
    tstate->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    assert(frame->frame_obj == NULL || frame->frame_obj->f_frame == frame);
    frame->previous = NULL;
    _PyFrame_ClearExceptCode(frame);
    _PyErr_ClearExcState(&gen->gi_exc_state);
    // gh-143939: There must not be any escaping calls between setting
    // the generator return kind and returning from _PyEval_EvalFrame.
    ((_PyThreadStateImpl *)tstate)->generator_return_kind = GENERATOR_RETURN;
}

void
_PyEval_FrameClearAndPop(PyThreadState *tstate, _PyInterpreterFrame * frame)
{
    // Update last_profiled_frame for remote profiler frame caching.
    // By this point, tstate->current_frame is already set to the parent frame.
    // Only update if we're popping the exact frame that was last profiled.
    // This avoids corrupting the cache when transient frames (called and returned
    // between profiler samples) update last_profiled_frame to addresses the
    // profiler never saw.
    if (tstate->last_profiled_frame != NULL && tstate->last_profiled_frame == frame) {
        tstate->last_profiled_frame = tstate->current_frame;
    }

    if (frame->owner == FRAME_OWNED_BY_THREAD) {
        clear_thread_frame(tstate, frame);
    }
    else {
        clear_gen_frame(tstate, frame);
    }
}

/* Borrowed public C source-entry adapter.
 * VM producers retain their separate consuming-operand transactions.
 * This adapter does not establish source-body or dataclass authority.
 */

static PyObject *
soac_source_entry_unavailable(const char *message)
{
    PyObject *exception = PySoac_GetStrictRuntimeUnavailableError();
    if (exception != NULL) {
        PyErr_SetString(exception, message);
    }
    return NULL;
}

static void
soac_source_entry_release_failed_bind(PySoacReleaseSourceCallV1 release,
                                      void *context)
{
    /* Release may destroy the final immutable context pin after native bind
     * cleanup has destroyed its function/code. It must not dereference either.
     * Preserve the original bind error even if cleanup changes PyErr. */
    PyObject *pending = PyErr_GetRaisedException();
    release(context);
    PyErr_SetRaisedException(pending);
}

/* Shared output contract only; VM and borrowed-C input producers stay
 * separate. The callback consumes context and must retire its view. */
static _PyStackRef
soac_source_entry_execute_result(PySoacBoundCallViewV1 *view,
                                PySoacExecuteSourceCallV1 execute, void *context)
{
    PySoacRefV1 result = PySoacRef_EmptyV1();
    int status = execute(view, context, &result);
    if (view->state != _Py_SOAC_CALL_TAKEN_V1 &&
        view->state != _Py_SOAC_CALL_ABORTED_V1) {
        Py_FatalError("SOAC Execute returned without retiring its bound view");
    }
    if (status == 0 && !PySoacRef_IsEmptyV1(result) &&
        PySoacRef_IsHeapSafeV1(result) && !PyErr_Occurred()) {
        _PyStackRef native;
        /* Move the very same close/debug-handle obligation; no DUP, INCREF,
         * IntoOwned/FromOwned normalization or tag inspection. */
        memcpy(&native, &result, sizeof(native));
        return native;
    }
    if (status == -1 && PySoacRef_IsEmptyV1(result) && PyErr_Occurred()) {
        return PyStackRef_NULL;
    }
    Py_FatalError("SOAC Execute violated its native-reference result contract");
}

PyObject *
PySoac_CallBorrowedVectorcallV1(PyObject *object, PyObject *const *args,
                               size_t nargsf, PyObject *kwnames)
{
    if (object == NULL || !PyFunction_Check(object) ||
        (kwnames != NULL && !PyTuple_Check(kwnames))) {
        PyErr_SetString(PyExc_TypeError,
                        "SOAC borrowed source call needs a function and keyword tuple");
        return NULL;
    }
    PySoacReferenceAbiV1 abi;
    if (PySoacRef_GetAbiV1(&abi, sizeof(abi)) < 0) {
        return NULL;
    }
    if (PyErr_Occurred()) {
        return NULL;
    }
    PyThreadState *thread = _PyThreadState_GET();
    PyFunctionObject *function = (PyFunctionObject *)object;
    PySoacSourceEntrySpecV1 spec;
    if (!_PySoacSourceEntry_CopyV1(function, &spec)) {
        return soac_source_entry_unavailable("no matching SOAC native source-entry registration");
    }
    if (PyFunction_CheckSoacStrictDefaults(object) < 0) {
        return NULL;
    }
    Py_ssize_t argcount = PyVectorcall_NARGS(nargsf);
    Py_ssize_t kwcount = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    if (argcount < 0 || kwcount > PY_SSIZE_T_MAX - argcount) {
        PyErr_SetString(PyExc_ValueError, "SOAC source-call argument count overflow");
        return NULL;
    }
    Py_ssize_t total_args = argcount + kwcount;
    if ((total_args != 0 && args == NULL) ||
        (size_t)total_args > SIZE_MAX / sizeof(_PyStackRef)) {
        PyErr_SetString(PyExc_ValueError, "invalid SOAC borrowed argument array");
        return NULL;
    }
    for (Py_ssize_t i = 0; i < total_args; i++) {
        if (args[i] == NULL) {
            PyErr_SetString(PyExc_ValueError, "NULL SOAC borrowed argument");
            return NULL;
        }
    }
    /* Match _PyFunction_Vectorcall's namespace choice and _PyEval_Vector's
     * borrowed-C acquisition order. The same borrowed keyword tuple, including
     * a tuple subclass, survives untouched for its actual caller's lifetime.
     * The OFFSET slot is neither read nor written. */
    PyCodeObject *code = spec.expected_code;
    PyObject *locals = (code->co_flags & CO_OPTIMIZED) ? NULL : function->func_globals;
    Py_ssize_t supplied_count = (Py_ssize_t)code->co_argcount + code->co_kwonlyargcount;
    _PyStackRef stack_array[8];
    _PyStackRef *arguments = stack_array;
    unsigned char stack_supplied[8];
    unsigned char *supplied = stack_supplied;
    if (total_args > 8) {
        arguments = PyMem_Malloc(sizeof(*arguments) * (size_t)total_args);
        if (arguments == NULL) {
            return PyErr_NoMemory();
        }
    }
    if (supplied_count > 8) {
        supplied = PyMem_Malloc((size_t)supplied_count);
        if (supplied == NULL) {
            if (arguments != stack_array) PyMem_Free(arguments);
            return PyErr_NoMemory();
        }
    }
    /* Revalidate before acquiring any native primary, then Capture->Pin->Bind
     * has no Python callback except those inside native binding itself. The
     * native record never pins function/code or normalizes borrowed VM tokens. */
    PySoacSourceEntrySpecV1 rechecked;
    if (!_PySoacSourceEntry_CopyV1(function, &rechecked) ||
        rechecked.expected_code != code ||
        rechecked.expected_metadata != spec.expected_metadata ||
        rechecked.expected_function_id != spec.expected_function_id ||
        rechecked.expected_strict_id != spec.expected_strict_id ||
        rechecked.expected_owner != spec.expected_owner ||
        rechecked.expected_vectorcall != spec.expected_vectorcall ||
        rechecked.pin != spec.pin || rechecked.release != spec.release ||
        rechecked.execute != spec.execute) {
        if (arguments != stack_array) PyMem_Free(arguments);
        if (supplied != stack_supplied) PyMem_Free(supplied);
        return soac_source_entry_unavailable("SOAC source entry changed before binding");
    }
    Py_XINCREF(locals);
    for (Py_ssize_t i = 0; i < argcount; i++) {
        arguments[i] = PyStackRef_FromPyObjectNew(args[i]);
    }
    for (Py_ssize_t i = 0; i < kwcount; i++) {
        arguments[argcount + i] = PyStackRef_FromPyObjectNew(args[argcount + i]);
    }
    _PyStackRef callable = PyStackRef_FromPyObjectNew(object);
    PySoacSourceCallInfoV1 captured;
    _PySoacCall_CaptureInfoV1(&captured, Py_SOAC_CALL_BORROWED_VECTOR_V1, callable, code);
    void *context = spec.pin(spec.expected_metadata, &captured);
    if (PyErr_Occurred()) {
        Py_FatalError("SOAC source Pin must be infallible and preserve the exception state");
    }
    PySoacBoundCallViewV1 view;
    int bound = _PySoacCall_BindV1(
        &view, thread, &captured, callable, locals, arguments, argcount,
        kwnames, supplied, supplied_count);
    /* All argument/function/namespace tokens were consumed by Bind on BOTH
     * outcomes. Free only obsolete transport storage, never close it again. */
    if (arguments != stack_array) {
        PyMem_Free(arguments);
    }
    if (bound < 0) {
        if (supplied != stack_supplied) PyMem_Free(supplied);
        soac_source_entry_release_failed_bind(spec.release, context);
        return NULL;
    }
    /* Borrowed C callers retain their argument/key owners. There are no EX
     * containers or caller tokens to steal here. Native _PyEval_Vector retires
     * only the copied transport array before body evaluation; that is done.
     * The caller's already-synchronized SP remains unchanged, not DEAD-ed. */
    _PyStackRef *ready_sp = view.caller == NULL ? NULL : view.caller->stackpointer;
    if (_PySoacCall_MarkBodyReadyV1(&view, ready_sp) < 0) {
        Py_FatalError("SOAC borrowed producer could not establish BodyReady");
    }
    _PyStackRef result = soac_source_entry_execute_result(&view, spec.execute, context);
    /* Execute owns context on both outcomes; do not Release again. */
    if (supplied != stack_supplied) {
        PyMem_Free(supplied);
    }
    return PyStackRef_IsNull(result) ? NULL : PyStackRef_AsPyObjectSteal(result);
}


/* Native consuming CALL/KW/EX producers.
 * Native Python-function entry receives the exact VM references;
 * no original strict bytecode is evaluated. */

int
_PySoacVMCall_IsRegisteredV1(PyObject *object)
{
    if (!PyFunction_Check(object)) {
        return 0;
    }
    PySoacSourceEntrySpecV1 entry;
    return _PySoacSourceEntry_CopyV1((PyFunctionObject *)object, &entry);
}

int
_PySoacVMCall_RequireOptimizedExpandedV1(PyObject *object)
{
    assert(PyFunction_Check(object));
    if (((PyCodeObject *)PyFunction_GET_CODE(object))->co_flags & CO_OPTIMIZED) {
        return 0;
    }
    (void)soac_source_entry_unavailable(
        "SOAC source EX entry requires its function-like original code; nonoptimized EX is not admitted");
    return -1;
}

static void
soac_vm_call_init(_PySoacVMCallV1 *call)
{
    call->state = _Py_SOAC_VM_CALL_FAILED_V1;
    call->pinned = 0;
    call->context = NULL;
    call->supplied = call->inline_supplied;
    /* view, entry and scratch are not token owners until Bind initializes
     * them. Do not zero-fill an opaque native reference and call it Empty. */
}

static void
soac_vm_close_unbound(_PyStackRef function, PyObject *locals,
                     const _PyStackRef *arguments, Py_ssize_t count)
{
    /* Pre-frame failure: native frame-push order, with the actual token
     * reference kinds. The producer's cause survives reentrant cleanup. */
    PyObject *pending = PyErr_GetRaisedException();
    PyStackRef_CLOSE(function);
    Py_XDECREF(locals);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyStackRef_CLOSE(arguments[i]);
    }
    PyErr_SetRaisedException(pending);
}

void
_PySoacVMCall_BindVectorV1(_PySoacVMCallV1 *call, PyThreadState *thread,
                          uint32_t kind, _PyStackRef function, PyObject *locals,
                          const _PyStackRef *arguments, Py_ssize_t positional,
                          PyObject *kwnames)
{
    soac_call_require_supported();
    assert(call != NULL && thread == _PyThreadState_GET());
    assert(kind >= Py_SOAC_CALL_VM_POSITIONAL_V1 && kind <= Py_SOAC_CALL_VM_EXPANDED_V1);
    assert(positional >= 0 && (kwnames == NULL || PyTuple_Check(kwnames)));
    soac_vm_call_init(call);
    Py_ssize_t keyword_count = kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
    assert(keyword_count <= PY_SSIZE_T_MAX - positional);
    Py_ssize_t total = positional + keyword_count;
    PyObject *object = PyStackRef_AsPyObjectBorrow(function);
    if (!_PySoacSourceEntry_CopyV1((PyFunctionObject *)object, &call->entry)) {
        (void)soac_source_entry_unavailable("SOAC VM entry changed before native binding");
        soac_vm_close_unbound(function, locals, arguments, total);
        return;
    }
    if (PyFunction_CheckSoacStrictDefaults(object) < 0) {
        soac_vm_close_unbound(function, locals, arguments, total);
        return;
    }
    PyCodeObject *code = call->entry.expected_code;
    Py_ssize_t supplied_count = (Py_ssize_t)code->co_argcount + code->co_kwonlyargcount;
    if (supplied_count > (Py_ssize_t)sizeof(call->inline_supplied)) {
        call->supplied = PyMem_Malloc((size_t)supplied_count);
        if (call->supplied == NULL) {
            PyErr_NoMemory();
            soac_vm_close_unbound(function, locals, arguments, total);
            return;
        }
    }
    /* Native EX has already unpacked and acquired its tokens. This is its
     * final frame-code capture, not the earlier namespace/flags read. Native
     * GIL allocation schedules GC; it is not a Python callback here. Pin is
     * infallible, C/Rust-only and immediately precedes native binding. */
    PySoacSourceCallInfoV1 captured;
    _PySoacCall_CaptureInfoV1(&captured, kind, function, code);
    call->context = call->entry.pin(call->entry.expected_metadata, &captured);
    call->pinned = 1;
    if (PyErr_Occurred()) {
        Py_FatalError("SOAC source Pin must be infallible and preserve the exception state");
    }
    if (_PySoacCall_BindV1(&call->view, thread, &captured, function, locals,
                         arguments, positional, kwnames,
                         call->supplied, supplied_count) == 0) {
        call->state = _Py_SOAC_VM_CALL_BOUND_V1;
    }
    /* Bind consumed every token on both outcomes. The opcode, not this
     * helper, will now DEAD its operand slots and publish its actual SP. */
}

void
_PySoacVMCall_BindExpandedV1(_PySoacVMCallV1 *call, PyThreadState *thread,
                            _PyStackRef function, Py_ssize_t positional,
                            PyObject *callargs, PyObject *kwargs)
{
    soac_call_require_supported();
    assert(call != NULL && thread == _PyThreadState_GET());
    assert(PyFunction_Check(PyStackRef_AsPyObjectBorrow(function)));
    assert(PyTuple_CheckExact(callargs));
    assert(kwargs == NULL || PyDict_CheckExact(kwargs));
    assert(positional == PyTuple_GET_SIZE(callargs));
    /* The opcode checked this before taking containers. Function-like native
     * compiler scopes include every admitted source/provider role. This is
     * not a role proof; the trusted registrar must supply that independently. */
    assert(((PyCodeObject *)PyFunction_GET_CODE(PyStackRef_AsPyObjectBorrow(function)))->co_flags & CO_OPTIMIZED);
    soac_vm_call_init(call);
    bool has_dict = kwargs != NULL && PyDict_GET_SIZE(kwargs) > 0;
    PyObject *kwnames = NULL;
    PyObject *const *object_array = NULL;
    _PyStackRef stack_array[8];
    _PyStackRef *arguments;
    if (has_dict) {
        object_array = _PyStack_UnpackDict(thread, _PyTuple_ITEMS(callargs),
                                         positional, kwargs, &kwnames);
        if (object_array == NULL) {
            PyStackRef_CLOSE(function);
            goto retire_containers;
        }
        Py_ssize_t keyword_count = PyDict_GET_SIZE(kwargs);
        arguments = (_PyStackRef *)object_array;
        /* Exact native EX acquisition: positional values are borrowed from
         * callargs, but keyword values already have owned unpack references. */
        for (Py_ssize_t i = 0; i < positional; i++) {
            arguments[i] = PyStackRef_FromPyObjectNew(object_array[i]);
        }
        for (Py_ssize_t i = 0; i < keyword_count; i++) {
            arguments[positional + i] = PyStackRef_FromPyObjectSteal(object_array[positional + i]);
        }
    }
    else {
        arguments = positional <= 8 ? stack_array
            : PyMem_Malloc(sizeof(*arguments) * (size_t)positional);
        if (arguments == NULL) {
            PyErr_NoMemory();
            PyStackRef_CLOSE(function);
            goto retire_containers;
        }
        for (Py_ssize_t i = 0; i < positional; i++) {
            arguments[i] = PyStackRef_FromPyObjectNew(PyTuple_GET_ITEM(callargs, i));
        }
    }
    _PySoacVMCall_BindVectorV1(call, thread, Py_SOAC_CALL_VM_EXPANDED_V1,
                             function, NULL, arguments, positional, kwnames);
    /* The same native bind-success and bind-error retirement phases. No
     * Close of the stale argument transport: Bind already consumed it. */
    if (has_dict) {
        _PyStack_UnpackDict_FreeNoDecRef(object_array, kwnames);
    }
    else if (positional > 8) {
        PyMem_Free(arguments);
    }
retire_containers:
    Py_DECREF(callargs);
    Py_XDECREF(kwargs);
}

_PyStackRef
_PySoacVMCall_FinishV1(_PySoacVMCallV1 *call, _PyStackRef *published_stackpointer)
{
    assert(call != NULL);
    if (call->state != _Py_SOAC_VM_CALL_FAILED_V1 &&
        call->state != _Py_SOAC_VM_CALL_BOUND_V1) {
        Py_FatalError("SOAC VM producer finished twice or with an invalid state");
    }
    uint32_t previous = call->state;
    call->state = _Py_SOAC_VM_CALL_FINISHED_V1;
    void *context = call->context;
    call->context = NULL;
    int pinned = call->pinned;
    call->pinned = 0;
    if (previous == _Py_SOAC_VM_CALL_FAILED_V1) {
        if (call->supplied != NULL && call->supplied != call->inline_supplied) {
            PyMem_Free(call->supplied);
        }
        call->supplied = NULL;
        if (pinned) {
            soac_source_entry_release_failed_bind(call->entry.release, context);
        }
        assert(PyErr_Occurred());
        return PyStackRef_NULL;
    }
    assert(pinned);
    if (_PySoacCall_MarkBodyReadyV1(&call->view, published_stackpointer) < 0) {
        Py_FatalError("SOAC VM opcode did not establish its actual BodyReady boundary");
    }
    _PyStackRef result = soac_source_entry_execute_result(&call->view, call->entry.execute, context);
    if (call->supplied != call->inline_supplied) {
        PyMem_Free(call->supplied);
    }
    call->supplied = NULL;
    return result;
}


/* IGNORED native22 C implementation. Insert after the frozen VM22 producer
 * in ceval.c. No Rust body or registration is enabled by these operations. */

#define SOAC_OUTGOING_INLINE_VALUES 8

typedef struct {
    _PyInterpreterFrame *frame;       /* borrowed actual current source parent */
    _Py_CODEUNIT *instruction;        /* supplied validated original emission */
    int instrumented;                /* form snapshot, not callback suppression */
    PyObject *namespace_value;        /* existing resolved namespace borrow */
} SoacOutgoingSiteV1;

static int
soac_outgoing_invalid(const char *message)
{
    PyErr_SetString(PyExc_ValueError, message);
    return -1;
}

static int
soac_outgoing_preflight(
    const PySoacOutgoingCallContextV1 *context, size_t context_size,
    uint32_t kind, PySoacRefV1 *operands, Py_ssize_t argument_count,
    PySoacRefV1 *result, SoacOutgoingSiteV1 *site)
{
    if (!soac_call_supported()) return -1;
    /* A call is not an error-cleanup primitive. Preserve an already pending
     * exception and leave every input untouched, rather than running a callee
     * with an unrelated cause installed. */
    if (PyErr_Occurred()) return -1;
    if (context_size != sizeof(*context) ||
        !soac_call_range(context, context_size, _Alignof(PySoacOutgoingCallContextV1)) ||
        context->abi_version != Py_SOAC_OUTGOING_CONTEXT_ABI_V1 || context->reserved != 0) {
        return soac_outgoing_invalid("invalid outgoing source-call context ABI");
    }
    if (context->source_scope_size != sizeof(PySoacLifetimeScopeV1) ||
        !soac_call_range(context->source_scope, context->source_scope_size,
                         _Alignof(PySoacLifetimeScopeV1))) {
        return soac_outgoing_invalid("invalid outgoing source-scope range");
    }
    if (kind < Py_SOAC_CALL_VM_POSITIONAL_V1 || kind > Py_SOAC_CALL_VM_EXPANDED_V1 ||
        argument_count < 0 || argument_count > INT_MAX - 1) {
        return soac_outgoing_invalid("outgoing argument count/kind is invalid or overflows native CALL");
    }
    size_t count = kind == Py_SOAC_CALL_VM_EXPANDED_V1 ? 4 : (size_t)argument_count + 3;
    if (count > SIZE_MAX / sizeof(*operands) ||
        !soac_call_range(operands, count * sizeof(*operands), _Alignof(PySoacRefV1)) ||
        !soac_call_range(result, sizeof(*result), _Alignof(PySoacRefV1))) {
        return soac_outgoing_invalid("invalid outgoing native-reference region");
    }
    size_t bytes = count * sizeof(*operands);
    if (soac_call_overlaps(operands, bytes, result, sizeof(*result)) ||
        soac_call_overlaps(operands, bytes, context, context_size) ||
        soac_call_overlaps(result, sizeof(*result), context, context_size) ||
        soac_call_overlaps(operands, bytes, context->source_scope, context->source_scope_size) ||
        soac_call_overlaps(result, sizeof(*result), context->source_scope, context->source_scope_size)) {
        return soac_outgoing_invalid("outgoing operand/result/context storage overlaps");
    }
    if (!PySoacRef_IsEmptyV1(*result)) {
        return soac_outgoing_invalid("outgoing result slot must start Empty");
    }
    _PyInterpreterFrame *frame = _PyFrame_BorrowSoacLifetimeScopeV1(
        context->source_scope, context->source_scope_size, context->source_code);
    if (frame == NULL) return -1;
    if (frame->soac_dataclass_invocation != NULL || frame->soac_dataclass_role != 0 ||
        frame->soac_dataclass_checked_activation != NULL) {
        return soac_outgoing_invalid("ordinary outgoing call cannot consume a trusted dataclass edge");
    }
    if (frame->f_globals == NULL || frame->f_builtins == NULL ||
        (context->source_namespace != NULL &&
         ((context->source_code->co_flags & CO_OPTIMIZED) ||
          !PyMapping_Check(context->source_namespace)))) {
        return soac_outgoing_invalid("outgoing call lacks its resolved source namespace");
    }
    if (PySoacRef_IsEmptyV1(operands[0])) {
        return soac_outgoing_invalid("outgoing callable operand is Empty");
    }
    if (kind == Py_SOAC_CALL_VM_EXPANDED_V1) {
        PyObject *tuple = PySoacRef_ObjectViewV1(operands[2]);
        PyObject *dict = PySoacRef_ObjectViewV1(operands[3]);
        if (!PySoacRef_IsEmptyV1(operands[1]) || tuple == NULL ||
            !PyTuple_CheckExact(tuple) || (dict != NULL && !PyDict_CheckExact(dict))) {
            return soac_outgoing_invalid("outgoing EX requires its already-prepared exact containers");
        }
    }
    else {
        PyObject *names = PySoacRef_ObjectViewV1(operands[argument_count + 2]);
        if ((kind == Py_SOAC_CALL_VM_POSITIONAL_V1 && names != NULL) ||
            (kind == Py_SOAC_CALL_VM_KEYWORDS_V1 &&
             (names == NULL || !PyTuple_CheckExact(names) ||
              PyTuple_GET_SIZE(names) > argument_count))) {
            return soac_outgoing_invalid("outgoing CALL/KW keyword shape is invalid");
        }
        for (Py_ssize_t i = 0; i < argument_count; i++) {
            if (PySoacRef_IsEmptyV1(operands[i + 2])) {
                return soac_outgoing_invalid("outgoing argument operand is Empty");
            }
        }
    }
    site->frame = frame;
    site->namespace_value = context->source_namespace;
    return _PySoac_OutgoingCallSiteV1(
        _PyThreadState_GET(), context->source_code, context->instruction_offset,
        kind, argument_count, &site->instrumented, &site->instruction);
}

static void
soac_outgoing_move(_PyStackRef *native, PySoacRefV1 *public, Py_ssize_t count)
{
    _Static_assert(sizeof(PySoacRefV1) == sizeof(_PyStackRef), "native reference size changed");
    _Static_assert(_Alignof(PySoacRefV1) == _Alignof(_PyStackRef), "native reference alignment changed");
    for (Py_ssize_t i = 0; i < count; i++) {
        PySoacRefV1 moved = PySoacRef_TakeV1(&public[i]);
        memcpy(&native[i], &moved, sizeof(native[i]));
    }
    /* No alias cast and no second close obligation. The public lanes are all
     * Empty before any method/monitor/bind/finalizer callback can happen. */
}

static _PyStackRef
soac_outgoing_take_native(_PyStackRef *slot)
{
    _PyStackRef result = *slot;
    *slot = PyStackRef_NULL;
    return result;
}

static void
soac_outgoing_close_native(_PyStackRef *slot)
{
    PyStackRef_XCLOSE(soac_outgoing_take_native(slot));
}

static void
soac_outgoing_discard_transports(_PyStackRef *slots, Py_ssize_t count)
{
    /* The binder/frame now owns these exact native handles. Never Close the
     * obsolete transport values, including on a bind-error return. */
    for (Py_ssize_t i = 0; i < count; i++) slots[i] = PyStackRef_NULL;
}

static void
soac_outgoing_unwind_vector(_PyStackRef *slots, Py_ssize_t count)
{
    PyObject *pending = PyErr_GetRaisedException();
    for (Py_ssize_t i = count; i > 0; i--) soac_outgoing_close_native(&slots[i - 1]);
    PyErr_SetRaisedException(pending);
}

static int
soac_outgoing_publish(_PyStackRef native, PySoacRefV1 *result)
{
    if (PyStackRef_IsNull(native)) {
        assert(PyErr_Occurred());
        return -1;
    }
    assert(!PyErr_Occurred() && PyStackRef_IsHeapSafe(native));
    memcpy(result, &native, sizeof(native));
    return 0;
}

static int
soac_outgoing_monitor_call(SoacOutgoingSiteV1 *site,
                           PyObject *callable, PyObject *first)
{
    if (!site->instrumented) return 0;
    assert(site->instruction != NULL);
    return _Py_call_instrumentation_2args(
        _PyThreadState_GET(), PY_MONITORING_EVENT_CALL, site->frame,
        site->instruction, callable, first);
}

static PyObject *
soac_outgoing_monitor_c_result(SoacOutgoingSiteV1 *site,
                               PyObject *callable, PyObject *first, PyObject *result)
{
    if (site->instrumented) {
        if (result == NULL) {
            _Py_call_instrumentation_exc2(_PyThreadState_GET(), PY_MONITORING_EVENT_C_RAISE,
                                          site->frame, site->instruction, callable, first);
        }
        else if (_Py_call_instrumentation_2args(
            _PyThreadState_GET(), PY_MONITORING_EVENT_C_RETURN, site->frame,
            site->instruction, callable, first) < 0) {
            Py_CLEAR(result);
        }
    }
    return result;
}

static _PyStackRef
soac_outgoing_custom_vector(SoacOutgoingSiteV1 *site, _PyStackRef *slots,
                             _PyStackRef *arguments, int total, _PyStackRef *names_slot)
{
    /* Same borrowed object-view conversion/OFFSET room as the native CALL
     * helper. Native scratch tokens, not new Python references, support it. */
    PyObject *result;
    STACKREFS_TO_PYOBJECTS(arguments, total, objects);
    if (CONVERSION_FAILED(objects)) {
        result = NULL;
        goto cleanup;
    }
    PyObject *callable = PyStackRef_AsPyObjectBorrow(slots[0]);
    PyObject *names = PyStackRef_AsPyObjectBorrow(*names_slot);
    int positional = total - (names == NULL ? 0 : (int)PyTuple_GET_SIZE(names));
    result = PySoac_VectorcallWithContext(
        callable, objects, (size_t)positional | PY_VECTORCALL_ARGUMENTS_OFFSET,
        names, site->frame->f_globals, site->namespace_value);
    STACKREFS_TO_PYOBJECTS_CLEANUP(objects);
    PyObject *first = total == 0 ? &_PyInstrumentation_MISSING
        : PyStackRef_AsPyObjectBorrow(arguments[0]);
    result = soac_outgoing_monitor_c_result(site, callable, first, result);
    assert((result != NULL) ^ (PyErr_Occurred() != NULL));
cleanup:
    /* Native C/custom CALL closes keyword names, reverse values/self, then
     * callable, after its result monitor. Publish each lane before Close. */
    soac_outgoing_close_native(names_slot);
    for (int i = total; i > 0; i--) soac_outgoing_close_native(&arguments[i - 1]);
    soac_outgoing_close_native(&slots[0]);
    return result == NULL ? PyStackRef_NULL : PyStackRef_FromPyObjectSteal(result);
}

PyAPI_FUNC(int)
PySoac_CallVectorWithReferencesV1(
    const PySoacOutgoingCallContextV1 *context, size_t context_size,
    uint32_t kind, PySoacRefV1 *operands, Py_ssize_t argument_count,
    PySoacRefV1 *result)
{
    /* Preserve an existing cause before kind, context or build validation. */
    if (PyErr_Occurred()) return -1;
    if (kind != Py_SOAC_CALL_VM_POSITIONAL_V1 && kind != Py_SOAC_CALL_VM_KEYWORDS_V1) {
        return soac_outgoing_invalid("vector reference call requires CALL or CALL_KW");
    }
    SoacOutgoingSiteV1 site;
    if (soac_outgoing_preflight(context, context_size, kind, operands,
                                argument_count, result, &site) < 0) return -1;
    Py_ssize_t count = argument_count + 3;
    _PyStackRef inline_slots[SOAC_OUTGOING_INLINE_VALUES + 3];
    _PyStackRef *slots = inline_slots;
    if (argument_count > SOAC_OUTGOING_INLINE_VALUES) {
        slots = PyMem_Malloc((size_t)count * sizeof(*slots));
        if (slots == NULL) { PyErr_NoMemory(); return -1; }
    }
    soac_outgoing_move(slots, operands, count);  /* COMMIT, no callback */
    PyThreadState *thread = _PyThreadState_GET();
    _PyStackRef returned = PyStackRef_NULL;

    /* Native _MAYBE_EXPAND_METHOD precedes CALL monitoring. Acquire its self
     * and function before closing the old method; a weakref callback can
     * mutate the evaluated function, so admission is still later. */
    PyObject *callable = PyStackRef_AsPyObjectBorrow(slots[0]);
    if (Py_TYPE(callable) == &PyMethod_Type && PyStackRef_IsNull(slots[1])) {
        PyObject *self = PyMethod_GET_SELF(callable);
        PyObject *function = PyMethod_GET_FUNCTION(callable);
        slots[1] = PyStackRef_FromPyObjectNew(self);
        _PyStackRef method = soac_outgoing_take_native(&slots[0]);
        slots[0] = PyStackRef_FromPyObjectNew(function);
        PyStackRef_CLOSE(method);
        callable = PyStackRef_AsPyObjectBorrow(slots[0]);
    }
    int has_self = !PyStackRef_IsNull(slots[1]);
    int total = (int)argument_count + has_self;
    _PyStackRef *arguments = slots + (has_self ? 1 : 2);
    _PyStackRef *names_slot = &slots[argument_count + 2];
    PyObject *first = total == 0 ? &_PyInstrumentation_MISSING
        : PyStackRef_AsPyObjectBorrow(arguments[0]);
    if (soac_outgoing_monitor_call(&site, callable, first) < 0) {
        soac_outgoing_unwind_vector(slots, count);
        goto done;
    }
    /* Snapshot the same post-monitor/pre-bind dispatch choice as native VM.
     * Do not reconsult PEP523 after keyword/default callbacks. */
    int native_fast = Py_TYPE(callable) == &PyFunction_Type && !IS_PEP523_HOOKED(thread);
    PyObject *names = PyStackRef_AsPyObjectBorrow(*names_slot);
    Py_ssize_t positional = total - (names == NULL ? 0 : PyTuple_GET_SIZE(names));
    if (native_fast && _PySoacVMCall_IsRegisteredV1(callable)) {
        _PySoacVMCallV1 call;
        int flags = ((PyCodeObject *)PyFunction_GET_CODE(callable))->co_flags;
        PyObject *locals = flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(callable));
        _PySoacVMCall_BindVectorV1(&call, thread, kind,
                                  soac_outgoing_take_native(&slots[0]), locals,
                                  arguments, positional, names);
        soac_outgoing_discard_transports(arguments, total);
        soac_outgoing_close_native(names_slot);
        /* Caller operand region is Empty and scope SP is its real current
         * unchanged source-parent cursor, not a manufactured VM stack. */
        returned = _PySoacVMCall_FinishV1(&call, site.frame->stackpointer);
    }
    else if (native_fast && ((PyFunctionObject *)callable)->vectorcall == _PyFunction_Vectorcall) {
        PyCodeObject *code = (PyCodeObject *)PyFunction_GET_CODE(callable);
        if (code->_co_soac_strict_source_id != 0) {
            (void)soac_source_entry_unavailable("outgoing call cannot evaluate original strict bytecode");
            soac_outgoing_unwind_vector(slots, count);
            goto done;
        }
        PyObject *locals = code->co_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(callable));
        _PyInterpreterFrame *callee = _PyEvalFramePushAndInit(
            thread, soac_outgoing_take_native(&slots[0]), locals,
            arguments, (size_t)positional, names, site.frame);
        soac_outgoing_discard_transports(arguments, total);
        soac_outgoing_close_native(names_slot);
        if (callee != NULL) {
            /* The selected VM fast path would DISPATCH_INLINED regardless of
             * a PEP523 hook installed later during argument binding. */
            PyObject *object = _PyEval_EvalFrameDefault(thread, callee, 0);
            if (object != NULL) returned = PyStackRef_FromPyObjectSteal(object);
        }
    }
    else {
        returned = soac_outgoing_custom_vector(&site, slots, arguments, total, names_slot);
    }
done:
    if (slots != inline_slots) PyMem_Free(slots);  /* bytes only, never Close */
    return soac_outgoing_publish(returned, result);
}

PyAPI_FUNC(int)
PySoac_CallPreparedWithReferencesV1(
    const PySoacOutgoingCallContextV1 *context, size_t context_size,
    PySoacRefV1 *operands, PySoacRefV1 *result)
{
    /* Preserve an existing cause before kind, context or build validation. */
    if (PyErr_Occurred()) return -1;
    SoacOutgoingSiteV1 site;
    if (soac_outgoing_preflight(context, context_size, Py_SOAC_CALL_VM_EXPANDED_V1,
                                operands, 0, result, &site) < 0) return -1;
    _PyStackRef slots[4];
    soac_outgoing_move(slots, operands, 4);  /* COMMIT, no callback */
    PyThreadState *thread = _PyThreadState_GET();
    PyObject *callable = PyStackRef_AsPyObjectBorrow(slots[0]);
    PyObject *tuple = PyStackRef_AsPyObjectBorrow(slots[2]);
    PyObject *kwargs = PyStackRef_AsPyObjectBorrow(slots[3]);
    Py_ssize_t positional = PyTuple_GET_SIZE(tuple);
    PyObject *first = positional == 0 ? &_PyInstrumentation_MISSING : PyTuple_GET_ITEM(tuple, 0);
    if (soac_outgoing_monitor_call(&site, callable, first) < 0) {
        soac_outgoing_unwind_vector(slots, 4);
        return -1;
    }
    /* Instrumented EX deliberately never takes the consuming branch, even
     * if callback delivery was suppressed or tools changed during CALL. */
    int native_fast = !site.instrumented && Py_TYPE(callable) == &PyFunction_Type
        && !IS_PEP523_HOOKED(thread);
    _PyStackRef returned = PyStackRef_NULL;
    if (native_fast && _PySoacVMCall_IsRegisteredV1(callable)) {
        if (_PySoacVMCall_RequireOptimizedExpandedV1(callable) < 0) {
            soac_outgoing_unwind_vector(slots, 4);
            return -1;
        }
        PyObject *owned_tuple = PyStackRef_AsPyObjectSteal(soac_outgoing_take_native(&slots[2]));
        PyObject *owned_kwargs = PyStackRef_IsNull(slots[3]) ? NULL
            : PyStackRef_AsPyObjectSteal(soac_outgoing_take_native(&slots[3]));
        _PySoacVMCallV1 call;
        _PySoacVMCall_BindExpandedV1(&call, thread, soac_outgoing_take_native(&slots[0]),
                                    positional, owned_tuple, owned_kwargs);
        returned = _PySoacVMCall_FinishV1(&call, site.frame->stackpointer);
    }
    else if (native_fast && ((PyFunctionObject *)callable)->vectorcall == _PyFunction_Vectorcall) {
        PyCodeObject *code = (PyCodeObject *)PyFunction_GET_CODE(callable);
        if (code->_co_soac_strict_source_id != 0) {
            (void)soac_source_entry_unavailable("outgoing EX cannot evaluate original strict bytecode");
            soac_outgoing_unwind_vector(slots, 4);
            return -1;
        }
        PyObject *owned_tuple = PyStackRef_AsPyObjectSteal(soac_outgoing_take_native(&slots[2]));
        PyObject *owned_kwargs = PyStackRef_IsNull(slots[3]) ? NULL
            : PyStackRef_AsPyObjectSteal(soac_outgoing_take_native(&slots[3]));
        PyObject *locals = code->co_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(callable));
        /* Deliberately reuse the actual ordinary helper, including its
         * separately recorded unoptimized early-namespace failure issue. */
        _PyInterpreterFrame *callee = _PyEvalFramePushAndInit_Ex(
            thread, soac_outgoing_take_native(&slots[0]), locals,
            positional, owned_tuple, owned_kwargs, site.frame);
        if (callee != NULL) {
            PyObject *object = _PyEval_EvalFrameDefault(thread, callee, 0);
            if (object != NULL) returned = PyStackRef_FromPyObjectSteal(object);
        }
    }
    else {
        PyObject *object = PySoac_ObjectCallWithContext(
            callable, tuple, kwargs, site.frame->f_globals, site.namespace_value);
        /* This is native EX's PyFunction/PyMethod exception to C completion
         * monitoring, not CALL/KW's unconditional custom-call rule. */
        if (!PyFunction_Check(callable) && !PyMethod_Check(callable)) {
            object = soac_outgoing_monitor_c_result(&site, callable, first, object);
        }
        soac_outgoing_close_native(&slots[3]);
        soac_outgoing_close_native(&slots[2]);
        soac_outgoing_close_native(&slots[0]);
        if (object != NULL) returned = PyStackRef_FromPyObjectSteal(object);
    }
    return soac_outgoing_publish(returned, result);
}


/* Consumes references to func, locals and all the args */
static _PyInterpreterFrame *
eval_frame_push_and_init(PyThreadState *tstate, _PyStackRef func,
                        PyObject *locals, _PyStackRef const* args,
                        size_t argcount, PyObject *kwnames, _PyInterpreterFrame *previous,
                        PyObject *soac_activation)
{
    PyFunctionObject *func_obj = (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(func);
    PyCodeObject * code = (PyCodeObject *)func_obj->func_code;
    CALL_STAT_INC(frames_pushed);
    _PyInterpreterFrame *frame = _PyThreadState_PushFrame(tstate, code->co_framesize);
    if (frame == NULL) {
        goto fail;
    }
    _PyFrame_Initialize(tstate, frame, func, locals, code, 0, previous);
    frame->soac_dataclass_checked_activation = Py_XNewRef(soac_activation);
    unsigned char *supplied = soac_activation == NULL ? NULL
        : _PySOAC_DataclassSuppliedMask(soac_activation);
    if (initialize_locals(tstate, func_obj, code, frame->localsplus, args, argcount, kwnames,
                          supplied)) {
        assert(frame->owner == FRAME_OWNED_BY_THREAD);
        clear_thread_frame(tstate, frame);
        return NULL;
    }
    return frame;
fail:
    /* Consume the references */
    PyStackRef_CLOSE(func);
    Py_XDECREF(locals);
    for (size_t i = 0; i < argcount; i++) {
        PyStackRef_CLOSE(args[i]);
    }
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            PyStackRef_CLOSE(args[i+argcount]);
        }
    }
    PyErr_NoMemory();
    return NULL;
}

_PyInterpreterFrame *
_PyEvalFramePushAndInit(PyThreadState *tstate, _PyStackRef func,
                        PyObject *locals, _PyStackRef const *args,
                        size_t argcount, PyObject *kwnames,
                        _PyInterpreterFrame *previous)
{
    return eval_frame_push_and_init(tstate, func, locals, args, argcount,
                                    kwnames, previous, NULL);
}

/* Same as _PyEvalFramePushAndInit but takes an args tuple and kwargs dict.
   Steals references to func, callargs and kwargs.
*/
_PyInterpreterFrame *
_PyEvalFramePushAndInit_Ex(PyThreadState *tstate, _PyStackRef func,
    PyObject *locals, Py_ssize_t nargs, PyObject *callargs, PyObject *kwargs, _PyInterpreterFrame *previous)
{
    bool has_dict = (kwargs != NULL && PyDict_GET_SIZE(kwargs) > 0);
    PyObject *kwnames = NULL;
    _PyStackRef *newargs;
    PyObject *const *object_array = NULL;
    _PyStackRef stack_array[8] = {0};
    if (has_dict) {
        object_array = _PyStack_UnpackDict(tstate, _PyTuple_ITEMS(callargs), nargs, kwargs, &kwnames);
        if (object_array == NULL) {
            PyStackRef_CLOSE(func);
            goto error;
        }
        size_t nkwargs = PyDict_GET_SIZE(kwargs);
        assert(sizeof(PyObject *) == sizeof(_PyStackRef));
        newargs = (_PyStackRef *)object_array;
        /* Positional args are borrowed from callargs tuple, need new reference */
        for (Py_ssize_t i = 0; i < nargs; i++) {
            newargs[i] = PyStackRef_FromPyObjectNew(object_array[i]);
        }
        /* Keyword args are owned by _PyStack_UnpackDict, steal them */
        for (size_t i = 0; i < nkwargs; i++) {
            newargs[nargs + i] = PyStackRef_FromPyObjectSteal(object_array[nargs + i]);
        }
    }
    else {
        if (nargs <= 8) {
            newargs = stack_array;
        }
        else {
            newargs = PyMem_Malloc(sizeof(_PyStackRef) *nargs);
            if (newargs == NULL) {
                PyErr_NoMemory();
                PyStackRef_CLOSE(func);
                goto error;
            }
        }
        /* We need to create a new reference for all our args since the new frame steals them. */
        for (Py_ssize_t i = 0; i < nargs; i++) {
            newargs[i] = PyStackRef_FromPyObjectNew(PyTuple_GET_ITEM(callargs, i));
        }
    }
    _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit(
        tstate, func, locals,
        newargs, nargs, kwnames, previous
    );
    if (has_dict) {
        _PyStack_UnpackDict_FreeNoDecRef(object_array, kwnames);
    }
    else if (nargs > 8) {
       PyMem_Free((void *)newargs);
    }
    /* No need to decref func here because the reference has been stolen by
       _PyEvalFramePushAndInit.
    */
    Py_DECREF(callargs);
    Py_XDECREF(kwargs);
    return new_frame;
error:
    Py_DECREF(callargs);
    Py_XDECREF(kwargs);
    return NULL;
}

static PyObject *
eval_vector_with_dataclass(PyThreadState *tstate, PyFunctionObject *func,
               PyObject *locals,
               PyObject* const* args, size_t argcount,
               PyObject *kwnames, PyObject *invocation, unsigned int stage,
               _PyInterpreterFrame *parent, PyObject *soac_activation)
{
    size_t total_args = argcount;
    if (kwnames) {
        total_args += PyTuple_GET_SIZE(kwnames);
    }
    _PyStackRef stack_array[8] = {0};
    _PyStackRef *arguments;
    if (total_args <= 8) {
        arguments = stack_array;
    }
    else {
        arguments = PyMem_Malloc(sizeof(_PyStackRef) * total_args);
        if (arguments == NULL) {
            return PyErr_NoMemory();
        }
    }
    /* _PyEvalFramePushAndInit consumes the references
     * to func, locals and all its arguments */
    Py_XINCREF(locals);
    for (size_t i = 0; i < argcount; i++) {
        arguments[i] = PyStackRef_FromPyObjectNew(args[i]);
    }
    if (kwnames) {
        Py_ssize_t kwcount = PyTuple_GET_SIZE(kwnames);
        for (Py_ssize_t i = 0; i < kwcount; i++) {
            arguments[i+argcount] = PyStackRef_FromPyObjectNew(args[i+argcount]);
        }
    }
    _PyInterpreterFrame *frame = eval_frame_push_and_init(
        tstate, PyStackRef_FromPyObjectNew(func), locals,
        arguments, argcount, kwnames, NULL, soac_activation);
    if (total_args > 8) {
        PyMem_Free(arguments);
    }
    if (frame == NULL) {
        return NULL;
    }
    if (soac_activation != NULL && _PySOAC_DataclassCheckBound(frame) < 0) {
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }
    if (invocation != NULL &&
        _PySOAC_DataclassEnterExplicit(invocation, stage, parent, frame) < 0) {
        _PyEval_FrameClearAndPop(tstate, frame);
        return NULL;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_VECTOR);
    return _PyEval_EvalFrame(tstate, frame, 0);
}

PyObject *
_PyEval_Vector(PyThreadState *tstate, PyFunctionObject *func,
               PyObject *locals, PyObject *const *args, size_t argcount,
               PyObject *kwnames)
{
    return eval_vector_with_dataclass(tstate, func, locals, args, argcount,
                                     kwnames, NULL, 0, NULL, NULL);
}

PyObject *
_PySOAC_DataclassEvalVector(PyThreadState *tstate, PyFunctionObject *func,
               PyObject *locals, PyObject *const *args, size_t argcount,
               PyObject *kwnames, PyObject *invocation, unsigned int stage,
               _PyInterpreterFrame *parent)
{
    return eval_vector_with_dataclass(tstate, func, locals, args, argcount,
                                     kwnames, invocation, stage, parent, NULL);
}

PyObject *
_PySOAC_DataclassEvalCheckedVector(PyThreadState *tstate, PyFunctionObject *func,
               PyObject *const *args, size_t argcount, PyObject *kwnames,
               PyObject *activation)
{
    return eval_vector_with_dataclass(tstate, func, NULL, args, argcount,
                                     kwnames, NULL, 0, NULL, activation);
}

/* Legacy API */
PyObject *
PyEval_EvalCodeEx(PyObject *_co, PyObject *globals, PyObject *locals,
                  PyObject *const *args, int argcount,
                  PyObject *const *kws, int kwcount,
                  PyObject *const *defs, int defcount,
                  PyObject *kwdefs, PyObject *closure)
{
    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *res = NULL;
    PyObject *defaults = PyTuple_FromArray(defs, defcount);
    if (defaults == NULL) {
        return NULL;
    }
    PyObject *builtins = _PyDict_LoadBuiltinsFromGlobals(globals);
    if (builtins == NULL) {
        Py_DECREF(defaults);
        return NULL;
    }
    if (locals == NULL) {
        locals = globals;
    }
    PyObject *kwnames = NULL;
    PyObject *const *allargs;
    PyObject **newargs = NULL;
    PyFunctionObject *func = NULL;
    if (kwcount == 0) {
        allargs = args;
    }
    else {
        kwnames = PyTuple_New(kwcount);
        if (kwnames == NULL) {
            goto fail;
        }
        newargs = PyMem_Malloc(sizeof(PyObject *)*(kwcount+argcount));
        if (newargs == NULL) {
            goto fail;
        }
        for (int i = 0; i < argcount; i++) {
            newargs[i] = args[i];
        }
        for (int i = 0; i < kwcount; i++) {
            PyTuple_SET_ITEM(kwnames, i, Py_NewRef(kws[2*i]));
            newargs[argcount+i] = kws[2*i+1];
        }
        allargs = newargs;
    }
    PyFrameConstructor constr = {
        .fc_globals = globals,
        .fc_builtins = builtins,
        .fc_name = ((PyCodeObject *)_co)->co_name,
        .fc_qualname = ((PyCodeObject *)_co)->co_name,
        .fc_code = _co,
        .fc_defaults = defaults,
        .fc_kwdefaults = kwdefs,
        .fc_closure = closure
    };
    func = _PyFunction_FromConstructor(&constr);
    if (func == NULL) {
        goto fail;
    }
    EVAL_CALL_STAT_INC(EVAL_CALL_LEGACY);
    res = _PyEval_Vector(tstate, func, locals,
                         allargs, argcount,
                         kwnames);
fail:
    Py_XDECREF(func);
    Py_XDECREF(kwnames);
    PyMem_Free(newargs);
    _Py_DECREF_BUILTINS(builtins);
    Py_DECREF(defaults);
    return res;
}

/* Logic for matching an exception in an except* clause (too
   complicated for inlining).
*/

int
_PyEval_ExceptionGroupMatch(_PyInterpreterFrame *frame, PyObject* exc_value,
                            PyObject *match_type, PyObject **match, PyObject **rest)
{
    if (Py_IsNone(exc_value)) {
        *match = Py_NewRef(Py_None);
        *rest = Py_NewRef(Py_None);
        return 0;
    }
    assert(PyExceptionInstance_Check(exc_value));

    if (PyErr_GivenExceptionMatches(exc_value, match_type)) {
        /* Full match of exc itself */
        bool is_eg = _PyBaseExceptionGroup_Check(exc_value);
        if (is_eg) {
            *match = Py_NewRef(exc_value);
        }
        else {
            /* naked exception - wrap it */
            PyObject *excs = PyTuple_Pack(1, exc_value);
            if (excs == NULL) {
                return -1;
            }
            PyObject *wrapped = _PyExc_CreateExceptionGroup("", excs);
            Py_DECREF(excs);
            if (wrapped == NULL) {
                return -1;
            }
            PyFrameObject *f = _PyFrame_GetFrameObject(frame);
            if (f != NULL) {
                PyObject *tb = _PyTraceBack_FromFrame(NULL, f);
                if (tb == NULL) {
                    return -1;
                }
                PyException_SetTraceback(wrapped, tb);
                Py_DECREF(tb);
            }
            *match = wrapped;
        }
        *rest = Py_NewRef(Py_None);
        return 0;
    }

    /* exc_value does not match match_type.
     * Check for partial match if it's an exception group.
     */
    if (_PyBaseExceptionGroup_Check(exc_value)) {
        PyObject *pair = PyObject_CallMethod(exc_value, "split", "(O)",
                                             match_type);
        if (pair == NULL) {
            return -1;
        }

        if (!PyTuple_CheckExact(pair)) {
            PyErr_Format(PyExc_TypeError,
                         "%.200s.split must return a tuple, not %.200s",
                         Py_TYPE(exc_value)->tp_name, Py_TYPE(pair)->tp_name);
            Py_DECREF(pair);
            return -1;
        }

        // allow tuples of length > 2 for backwards compatibility
        if (PyTuple_GET_SIZE(pair) < 2) {
            PyErr_Format(PyExc_TypeError,
                         "%.200s.split must return a 2-tuple, "
                         "got tuple of size %zd",
                         Py_TYPE(exc_value)->tp_name, PyTuple_GET_SIZE(pair));
            Py_DECREF(pair);
            return -1;
        }

        *match = Py_NewRef(PyTuple_GET_ITEM(pair, 0));
        *rest = Py_NewRef(PyTuple_GET_ITEM(pair, 1));
        Py_DECREF(pair);
        return 0;
    }
    /* no match */
    *match = Py_NewRef(Py_None);
    *rest = Py_NewRef(exc_value);
    return 0;
}

/* Iterate v argcnt times and store the results on the stack (via decreasing
   sp).  Return 1 for success, 0 if error.

   If argcntafter == -1, do a simple unpack. If it is >= 0, do an unpack
   with a variable target.
*/

int
_PyEval_UnpackIterableStackRef(PyThreadState *tstate, PyObject *v,
                       int argcnt, int argcntafter, _PyStackRef *sp)
{
    int i = 0, j = 0;
    Py_ssize_t ll = 0;
    PyObject *it;  /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */
    assert(v != NULL);

    it = PyObject_GetIter(v);
    if (it == NULL) {
        if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
            Py_TYPE(v)->tp_iter == NULL && !PySequence_Check(v))
        {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "cannot unpack non-iterable %.200s object",
                          Py_TYPE(v)->tp_name);
        }
        return 0;
    }

    for (; i < argcnt; i++) {
        w = PyIter_Next(it);
        if (w == NULL) {
            /* Iterator done, via error or exhaustion. */
            if (!_PyErr_Occurred(tstate)) {
                if (argcntafter == -1) {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected %d, got %d)",
                                  argcnt, i);
                }
                else {
                    _PyErr_Format(tstate, PyExc_ValueError,
                                  "not enough values to unpack "
                                  "(expected at least %d, got %d)",
                                  argcnt + argcntafter, i);
                }
            }
            goto Error;
        }
        *--sp = PyStackRef_FromPyObjectSteal(w);
    }

    if (argcntafter == -1) {
        /* We better have exhausted the iterator now. */
        w = PyIter_Next(it);
        if (w == NULL) {
            if (_PyErr_Occurred(tstate))
                goto Error;
            Py_DECREF(it);
            return 1;
        }
        Py_DECREF(w);

        if (PyList_CheckExact(v) || PyTuple_CheckExact(v)
              || PyDict_CheckExact(v)) {
            ll = PyDict_CheckExact(v) ? PyDict_Size(v) : Py_SIZE(v);
            if (ll > argcnt) {
                _PyErr_Format(tstate, PyExc_ValueError,
                              "too many values to unpack (expected %d, got %zd)",
                              argcnt, ll);
                goto Error;
            }
        }
        _PyErr_Format(tstate, PyExc_ValueError,
                      "too many values to unpack (expected %d)",
                      argcnt);
        goto Error;
    }

    l = PySequence_List(it);
    if (l == NULL)
        goto Error;
    *--sp = PyStackRef_FromPyObjectSteal(l);
    i++;

    ll = PyList_GET_SIZE(l);
    if (ll < argcntafter) {
        _PyErr_Format(tstate, PyExc_ValueError,
            "not enough values to unpack (expected at least %d, got %zd)",
            argcnt + argcntafter, argcnt + ll);
        goto Error;
    }

    /* Pop the "after-variable" args off the list. */
    for (j = argcntafter; j > 0; j--, i++) {
        *--sp = PyStackRef_FromPyObjectSteal(PyList_GET_ITEM(l, ll - j));
    }
    /* Resize the list. */
    Py_SET_SIZE(l, ll - argcntafter);
    Py_DECREF(it);
    return 1;

Error:
    for (; i > 0; i--, sp++) {
        PyStackRef_CLOSE(*sp);
    }
    Py_XDECREF(it);
    return 0;
}



void
_PyEval_MonitorRaise(PyThreadState *tstate, _PyInterpreterFrame *frame,
              _Py_CODEUNIT *instr)
{
    if (no_tools_for_global_event(tstate, PY_MONITORING_EVENT_RAISE)) {
        return;
    }
    do_monitor_exc(tstate, frame, instr, PY_MONITORING_EVENT_RAISE);
}

bool
_PyEval_NoToolsForUnwind(PyThreadState *tstate) {
    return no_tools_for_global_event(tstate, PY_MONITORING_EVENT_PY_UNWIND);
}


void
PyThreadState_EnterTracing(PyThreadState *tstate)
{
    assert(tstate->tracing >= 0);
    tstate->tracing++;
}

void
PyThreadState_LeaveTracing(PyThreadState *tstate)
{
    assert(tstate->tracing > 0);
    tstate->tracing--;
}


PyObject*
_PyEval_CallTracing(PyObject *func, PyObject *args)
{
    // Save and disable tracing
    PyThreadState *tstate = _PyThreadState_GET();
    int save_tracing = tstate->tracing;
    tstate->tracing = 0;

    // Call the tracing function
    PyObject *result = PyObject_Call(func, args, NULL);

    // Restore tracing
    tstate->tracing = save_tracing;
    return result;
}

void
PyEval_SetProfile(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetProfile(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetProfile");
    }
}

void
PyEval_SetProfileAllThreads(Py_tracefunc func, PyObject *arg)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (_PyEval_SetProfileAllThreads(interp, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetProfileAllThreads");
    }
}

void
PyEval_SetTrace(Py_tracefunc func, PyObject *arg)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyEval_SetTrace(tstate, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetTrace");
    }
}

void
PyEval_SetTraceAllThreads(Py_tracefunc func, PyObject *arg)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (_PyEval_SetTraceAllThreads(interp, func, arg) < 0) {
        /* Log _PySys_Audit() error */
        PyErr_FormatUnraisable("Exception ignored in PyEval_SetTraceAllThreads");
    }
}

int
_PyEval_SetCoroutineOriginTrackingDepth(int depth)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (depth < 0) {
        _PyErr_SetString(tstate, PyExc_ValueError, "depth must be >= 0");
        return -1;
    }
    tstate->coroutine_origin_tracking_depth = depth;
    return 0;
}


int
_PyEval_GetCoroutineOriginTrackingDepth(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->coroutine_origin_tracking_depth;
}

int
_PyEval_SetAsyncGenFirstiter(PyObject *firstiter)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_firstiter", NULL) < 0) {
        return -1;
    }

    Py_XSETREF(tstate->async_gen_firstiter, Py_XNewRef(firstiter));
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFirstiter(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_firstiter;
}

int
_PyEval_SetAsyncGenFinalizer(PyObject *finalizer)
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (_PySys_Audit(tstate, "sys.set_asyncgen_hook_finalizer", NULL) < 0) {
        return -1;
    }

    Py_XSETREF(tstate->async_gen_finalizer, Py_XNewRef(finalizer));
    return 0;
}

PyObject *
_PyEval_GetAsyncGenFinalizer(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return tstate->async_gen_finalizer;
}

_PyInterpreterFrame *
_PyEval_GetFrame(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyThreadState_GetFrame(tstate);
}

PyFrameObject *
PyEval_GetFrame(void)
{
    _PyInterpreterFrame *frame = _PyEval_GetFrame();
    if (frame == NULL) {
        return NULL;
    }
    PyFrameObject *f = _PyFrame_GetFrameObject(frame);
    if (f == NULL) {
        PyErr_Clear();
    }
    return f;
}

PyObject *
_PyEval_GetBuiltins(PyThreadState *tstate)
{
    _PyInterpreterFrame *frame = _PyThreadState_GetFrame(tstate);
    if (frame != NULL) {
        return frame->f_builtins;
    }
    return tstate->interp->builtins;
}

PyObject *
PyEval_GetBuiltins(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_GetBuiltins(tstate);
}

/* Convenience function to get a builtin from its name */
PyObject *
_PyEval_GetBuiltin(PyObject *name)
{
    PyObject *attr;
    if (PyMapping_GetOptionalItem(PyEval_GetBuiltins(), name, &attr) == 0) {
        PyErr_SetObject(PyExc_AttributeError, name);
    }
    return attr;
}

PyObject *
PyEval_GetLocals(void)
{
    // We need to return a borrowed reference here, so some tricks are needed
    PyThreadState *tstate = _PyThreadState_GET();
     _PyInterpreterFrame *current_frame = _PyThreadState_GetFrame(tstate);
    if (current_frame == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    // Be aware that this returns a new reference
    PyObject *locals = _PyFrame_GetLocals(current_frame);

    if (locals == NULL) {
        return NULL;
    }

    if (PyFrameLocalsProxy_Check(locals)) {
        PyFrameObject *f = _PyFrame_GetFrameObject(current_frame);
        PyObject *ret = f->f_locals_cache;
        if (ret == NULL) {
            ret = PyDict_New();
            if (ret == NULL) {
                Py_DECREF(locals);
                return NULL;
            }
            f->f_locals_cache = ret;
        }
        if (PyDict_Update(ret, locals) < 0) {
            // At this point, if the cache dict is broken, it will stay broken, as
            // trying to clean it up or replace it will just cause other problems
            ret = NULL;
        }
        Py_DECREF(locals);
        return ret;
    }

    assert(PyMapping_Check(locals));
    assert(Py_REFCNT(locals) > 1);
    Py_DECREF(locals);

    return locals;
}

PyObject *
_PyEval_GetFrameLocals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
     _PyInterpreterFrame *current_frame = _PyThreadState_GetFrame(tstate);
    if (current_frame == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "frame does not exist");
        return NULL;
    }

    PyObject *locals = _PyFrame_GetLocals(current_frame);
    if (locals == NULL) {
        return NULL;
    }

    if (PyFrameLocalsProxy_Check(locals)) {
        PyObject* ret = PyDict_New();
        if (ret == NULL) {
            Py_DECREF(locals);
            return NULL;
        }
        if (PyDict_Update(ret, locals) < 0) {
            Py_DECREF(ret);
            Py_DECREF(locals);
            return NULL;
        }
        Py_DECREF(locals);
        return ret;
    }

    assert(PyMapping_Check(locals));
    return locals;
}

static PyObject *
_PyEval_GetGlobals(PyThreadState *tstate)
{
    _PyInterpreterFrame *current_frame = _PyThreadState_GetFrame(tstate);
    if (current_frame == NULL) {
        return NULL;
    }
    return current_frame->f_globals;
}

PyObject *
PyEval_GetGlobals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return _PyEval_GetGlobals(tstate);
}

PyObject *
_PyEval_GetGlobalsFromRunningMain(PyThreadState *tstate)
{
    if (!_PyInterpreterState_IsRunningMain(tstate->interp)) {
        return NULL;
    }
    PyObject *mod = _Py_GetMainModule(tstate);
    if (_Py_CheckMainModule(mod) < 0) {
        Py_XDECREF(mod);
        return NULL;
    }
    PyObject *globals = PyModule_GetDict(mod);  // borrowed
    Py_DECREF(mod);
    return globals;
}

static PyObject *
get_globals_builtins(PyObject *globals)
{
    PyObject *builtins = NULL;
    if (PyDict_Check(globals)) {
        if (PyDict_GetItemRef(globals, &_Py_ID(__builtins__), &builtins) < 0) {
            return NULL;
        }
    }
    else {
        if (PyMapping_GetOptionalItem(
                        globals, &_Py_ID(__builtins__), &builtins) < 0)
        {
            return NULL;
        }
    }
    return builtins;
}

static int
set_globals_builtins(PyObject *globals, PyObject *builtins)
{
    if (PyDict_Check(globals)) {
        if (PyDict_SetItem(globals, &_Py_ID(__builtins__), builtins) < 0) {
            return -1;
        }
    }
    else {
        if (PyObject_SetItem(globals, &_Py_ID(__builtins__), builtins) < 0) {
            return -1;
        }
    }
    return 0;
}

int
_PyEval_EnsureBuiltins(PyThreadState *tstate, PyObject *globals,
                       PyObject **p_builtins)
{
    PyObject *builtins = get_globals_builtins(globals);
    if (builtins == NULL) {
        if (_PyErr_Occurred(tstate)) {
            return -1;
        }
        builtins = PyEval_GetBuiltins();  // borrowed
        if (builtins == NULL) {
            assert(_PyErr_Occurred(tstate));
            return -1;
        }
        Py_INCREF(builtins);
        if (set_globals_builtins(globals, builtins) < 0) {
            Py_DECREF(builtins);
            return -1;
        }
    }
    if (p_builtins != NULL) {
        *p_builtins = builtins;
    }
    else {
        Py_DECREF(builtins);
    }
    return 0;
}

int
_PyEval_EnsureBuiltinsWithModule(PyThreadState *tstate, PyObject *globals,
                                 PyObject **p_builtins)
{
    PyObject *builtins = get_globals_builtins(globals);
    if (builtins == NULL) {
        if (_PyErr_Occurred(tstate)) {
            return -1;
        }
        builtins = PyImport_ImportModuleLevel("builtins", NULL, NULL, NULL, 0);
        if (builtins == NULL) {
            return -1;
        }
        if (set_globals_builtins(globals, builtins) < 0) {
            Py_DECREF(builtins);
            return -1;
        }
    }
    if (p_builtins != NULL) {
        *p_builtins = builtins;
    }
    else {
        Py_DECREF(builtins);
    }
    return 0;
}

PyObject*
PyEval_GetFrameLocals(void)
{
    return _PyEval_GetFrameLocals();
}

PyObject* PyEval_GetFrameGlobals(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *current_frame = _PyThreadState_GetFrame(tstate);
    if (current_frame == NULL) {
        return NULL;
    }
    return Py_XNewRef(current_frame->f_globals);
}

PyObject* PyEval_GetFrameBuiltins(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return Py_XNewRef(_PyEval_GetBuiltins(tstate));
}

int
PyEval_MergeCompilerFlags(PyCompilerFlags *cf)
{
    PyThreadState *tstate = _PyThreadState_GET();
    _PyInterpreterFrame *current_frame = tstate->current_frame;
    if (current_frame == tstate->base_frame) {
        current_frame = NULL;
    }
    int result = cf->cf_flags != 0;

    if (current_frame != NULL) {
        const int codeflags = _PyFrame_GetCode(current_frame)->co_flags;
        const int compilerflags = codeflags & PyCF_MASK;
        if (compilerflags) {
            result = 1;
            cf->cf_flags |= compilerflags;
        }
    }
    return result;
}


const char *
PyEval_GetFuncName(PyObject *func)
{
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyFunction_Check(func))
        return PyUnicode_AsUTF8(((PyFunctionObject*)func)->func_name);
    else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else
        return Py_TYPE(func)->tp_name;
}

const char *
PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else
        return " object";
}

/* Extract a slice index from a PyLong or an object with the
   nb_index slot defined, and store in *pi.
   Silently reduce values larger than PY_SSIZE_T_MAX to PY_SSIZE_T_MAX,
   and silently boost values less than PY_SSIZE_T_MIN to PY_SSIZE_T_MIN.
   Return 0 on error, 1 on success.
*/
int
_PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (!Py_IsNone(v)) {
        Py_ssize_t x;
        if (_PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && _PyErr_Occurred(tstate))
                return 0;
        }
        else {
            _PyErr_SetString(tstate, PyExc_TypeError,
                             "slice indices must be integers or "
                             "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}

int
_PyEval_SliceIndexNotNone(PyObject *v, Py_ssize_t *pi)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Py_ssize_t x;
    if (_PyIndex_Check(v)) {
        x = PyNumber_AsSsize_t(v, NULL);
        if (x == -1 && _PyErr_Occurred(tstate))
            return 0;
    }
    else {
        _PyErr_SetString(tstate, PyExc_TypeError,
                         "slice indices must be integers or "
                         "have an __index__ method");
        return 0;
    }
    *pi = x;
    return 1;
}

PyObject *
_PyEval_ImportName(PyThreadState *tstate, _PyInterpreterFrame *frame,
            PyObject *name, PyObject *fromlist, PyObject *level)
{
    PyObject *import_func;
    if (PyMapping_GetOptionalItem(frame->f_builtins, &_Py_ID(__import__), &import_func) < 0) {
        return NULL;
    }
    if (import_func == NULL) {
        _PyErr_SetString(tstate, PyExc_ImportError, "__import__ not found");
        return NULL;
    }

    PyObject *locals = frame->f_locals;
    if (locals == NULL) {
        locals = Py_None;
    }

    /* Fast path for not overloaded __import__. */
    if (_PyImport_IsDefaultImportFunc(tstate->interp, import_func)) {
        Py_DECREF(import_func);
        int ilevel = PyLong_AsInt(level);
        if (ilevel == -1 && _PyErr_Occurred(tstate)) {
            return NULL;
        }
        return PyImport_ImportModuleLevelObject(
                        name,
                        frame->f_globals,
                        locals,
                        fromlist,
                        ilevel);
    }

    PyObject* args[5] = {name, frame->f_globals, locals, fromlist, level};
    PyObject *res = PyObject_Vectorcall(import_func, args, 5, NULL);
    Py_DECREF(import_func);
    return res;
}

PyObject *
_PyEval_ImportFrom(PyThreadState *tstate, PyObject *v, PyObject *name)
{
    PyObject *x;
    PyObject *fullmodname, *mod_name, *origin, *mod_name_or_unknown, *errmsg, *spec;

    if (PyObject_GetOptionalAttr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    if (PyObject_GetOptionalAttr(v, &_Py_ID(__name__), &mod_name) < 0) {
        return NULL;
    }
    if (mod_name == NULL || !PyUnicode_Check(mod_name)) {
        Py_CLEAR(mod_name);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", mod_name, name);
    if (fullmodname == NULL) {
        Py_DECREF(mod_name);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !_PyErr_Occurred(tstate)) {
        goto error;
    }
    Py_DECREF(mod_name);
    return x;

 error:
    if (mod_name == NULL) {
        mod_name_or_unknown = PyUnicode_FromString("<unknown module name>");
        if (mod_name_or_unknown == NULL) {
            return NULL;
        }
    } else {
        mod_name_or_unknown = mod_name;
    }
    // mod_name is no longer an owned reference
    assert(mod_name_or_unknown);
    assert(mod_name == NULL || mod_name == mod_name_or_unknown);

    origin = NULL;
    if (PyObject_GetOptionalAttr(v, &_Py_ID(__spec__), &spec) < 0) {
        Py_DECREF(mod_name_or_unknown);
        return NULL;
    }
    if (spec == NULL) {
        errmsg = PyUnicode_FromFormat(
            "cannot import name %R from %R (unknown location)",
            name, mod_name_or_unknown
        );
        goto done_with_errmsg;
    }
    if (_PyModuleSpec_GetFileOrigin(spec, &origin) < 0) {
        goto done;
    }

    int is_possibly_shadowing = _PyModule_IsPossiblyShadowing(origin);
    if (is_possibly_shadowing < 0) {
        goto done;
    }
    int is_possibly_shadowing_stdlib = 0;
    if (is_possibly_shadowing) {
        PyObject *stdlib_modules;
        if (PySys_GetOptionalAttrString("stdlib_module_names", &stdlib_modules) < 0) {
            goto done;
        }
        if (stdlib_modules && PyAnySet_Check(stdlib_modules)) {
            is_possibly_shadowing_stdlib = PySet_Contains(stdlib_modules, mod_name_or_unknown);
            if (is_possibly_shadowing_stdlib < 0) {
                Py_DECREF(stdlib_modules);
                goto done;
            }
        }
        Py_XDECREF(stdlib_modules);
    }

    if (origin == NULL && PyModule_Check(v)) {
        // Fall back to __file__ for diagnostics if we don't have
        // an origin that is a location
        origin = PyModule_GetFilenameObject(v);
        if (origin == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_SystemError)) {
                goto done;
            }
            // PyModule_GetFilenameObject raised "module filename missing"
            _PyErr_Clear(tstate);
        }
        assert(origin == NULL || PyUnicode_Check(origin));
    }

    if (is_possibly_shadowing_stdlib) {
        assert(origin);
        errmsg = PyUnicode_FromFormat(
            "cannot import name %R from %R "
            "(consider renaming %R since it has the same "
            "name as the standard library module named %R "
            "and prevents importing that standard library module)",
            name, mod_name_or_unknown, origin, mod_name_or_unknown
        );
    }
    else {
        int rc = _PyModuleSpec_IsInitializing(spec);
        if (rc < 0) {
            goto done;
        }
        else if (rc > 0) {
            if (is_possibly_shadowing) {
                assert(origin);
                // For non-stdlib modules, only mention the possibility of
                // shadowing if the module is being initialized.
                errmsg = PyUnicode_FromFormat(
                    "cannot import name %R from %R "
                    "(consider renaming %R if it has the same name "
                    "as a library you intended to import)",
                    name, mod_name_or_unknown, origin
                );
            }
            else if (origin) {
                errmsg = PyUnicode_FromFormat(
                    "cannot import name %R from partially initialized module %R "
                    "(most likely due to a circular import) (%S)",
                    name, mod_name_or_unknown, origin
                );
            }
            else {
                errmsg = PyUnicode_FromFormat(
                    "cannot import name %R from partially initialized module %R "
                    "(most likely due to a circular import)",
                    name, mod_name_or_unknown
                );
            }
        }
        else {
            assert(rc == 0);
            if (origin) {
                errmsg = PyUnicode_FromFormat(
                    "cannot import name %R from %R (%S)",
                    name, mod_name_or_unknown, origin
                );
            }
            else {
                errmsg = PyUnicode_FromFormat(
                    "cannot import name %R from %R (unknown location)",
                    name, mod_name_or_unknown
                );
            }
        }
    }

done_with_errmsg:
    if (errmsg != NULL) {
        /* NULL checks for mod_name and origin done by _PyErr_SetImportErrorWithNameFrom */
        _PyErr_SetImportErrorWithNameFrom(errmsg, mod_name, origin, name);
        Py_DECREF(errmsg);
    }

done:
    Py_XDECREF(origin);
    Py_XDECREF(spec);
    Py_DECREF(mod_name_or_unknown);
    return NULL;
}

#define CANNOT_CATCH_MSG "catching classes that do not inherit from "\
                         "BaseException is not allowed"

#define CANNOT_EXCEPT_STAR_EG "catching ExceptionGroup with except* "\
                              "is not allowed. Use except instead."

int
_PyEval_CheckExceptTypeValid(PyThreadState *tstate, PyObject* right)
{
    if (PyTuple_Check(right)) {
        Py_ssize_t i, length;
        length = PyTuple_GET_SIZE(right);
        for (i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            if (!PyExceptionClass_Check(exc)) {
                _PyErr_SetString(tstate, PyExc_TypeError,
                    CANNOT_CATCH_MSG);
                return -1;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(right)) {
            _PyErr_SetString(tstate, PyExc_TypeError,
                CANNOT_CATCH_MSG);
            return -1;
        }
    }
    return 0;
}

int
_PyEval_CheckExceptStarTypeValid(PyThreadState *tstate, PyObject* right)
{
    if (_PyEval_CheckExceptTypeValid(tstate, right) < 0) {
        return -1;
    }

    /* reject except *ExceptionGroup */

    int is_subclass = 0;
    if (PyTuple_Check(right)) {
        Py_ssize_t length = PyTuple_GET_SIZE(right);
        for (Py_ssize_t i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            is_subclass = PyObject_IsSubclass(exc, PyExc_BaseExceptionGroup);
            if (is_subclass < 0) {
                return -1;
            }
            if (is_subclass) {
                break;
            }
        }
    }
    else {
        is_subclass = PyObject_IsSubclass(right, PyExc_BaseExceptionGroup);
        if (is_subclass < 0) {
            return -1;
        }
    }
    if (is_subclass) {
        _PyErr_SetString(tstate, PyExc_TypeError,
            CANNOT_EXCEPT_STAR_EG);
            return -1;
    }
    return 0;
}

int
_Py_Check_ArgsIterable(PyThreadState *tstate, PyObject *func, PyObject *args)
{
    if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
        _PyErr_Format(tstate, PyExc_TypeError,
                      "Value after * must be an iterable, not %.200s",
                      Py_TYPE(args)->tp_name);
        return -1;
    }
    return 0;
}

void
_PyEval_FormatKwargsError(PyThreadState *tstate, PyObject *func, PyObject *kwargs)
{
    /* _PyDict_MergeEx raises attribute
     * error (percolated from an attempt
     * to get 'keys' attribute) instead of
     * a type error if its second argument
     * is not a mapping.
     */
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
        _PyErr_Format(
            tstate, PyExc_TypeError,
            "Value after ** must be a mapping, not %.200s",
            Py_TYPE(kwargs)->tp_name);
    }
    else if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
        PyObject *exc = _PyErr_GetRaisedException(tstate);
        PyObject *args = PyException_GetArgs(exc);
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 1) {
            _PyErr_Clear(tstate);
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(args, 0);
                _PyErr_Format(
                    tstate, PyExc_TypeError,
                    "%U got multiple values for keyword argument '%S'",
                    funcstr, key);
                Py_DECREF(funcstr);
            }
            Py_XDECREF(exc);
        }
        else {
            _PyErr_SetRaisedException(tstate, exc);
        }
        Py_DECREF(args);
    }
}

void
_PyEval_FormatExcCheckArg(PyThreadState *tstate, PyObject *exc,
                          const char *format_str, PyObject *obj)
{
    const char *obj_str;

    if (!obj)
        return;

    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;

    _PyErr_Format(tstate, exc, format_str, obj_str);

    if (exc == PyExc_NameError) {
        // Include the name in the NameError exceptions to offer suggestions later.
        PyObject *exc = PyErr_GetRaisedException();
        if (PyErr_GivenExceptionMatches(exc, PyExc_NameError)) {
            if (((PyNameErrorObject*)exc)->name == NULL) {
                // We do not care if this fails because we are going to restore the
                // NameError anyway.
                (void)PyObject_SetAttr(exc, &_Py_ID(name), obj);
            }
        }
        PyErr_SetRaisedException(exc);
    }
}

void
_PyEval_FormatExcUnbound(PyThreadState *tstate, PyCodeObject *co, int oparg)
{
    PyObject *name;
    /* Don't stomp existing exception */
    if (_PyErr_Occurred(tstate))
        return;
    name = PyTuple_GET_ITEM(co->co_localsplusnames, oparg);
    if (oparg < PyUnstable_Code_GetFirstFree(co)) {
        _PyEval_FormatExcCheckArg(tstate, PyExc_UnboundLocalError,
                                  UNBOUNDLOCAL_ERROR_MSG, name);
    } else {
        _PyEval_FormatExcCheckArg(tstate, PyExc_NameError,
                                  UNBOUNDFREE_ERROR_MSG, name);
    }
}

void
_PyEval_FormatAwaitableError(PyThreadState *tstate, PyTypeObject *type, int oparg)
{
    if (type->tp_as_async == NULL || type->tp_as_async->am_await == NULL) {
        if (oparg == 1) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aenter__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
        else if (oparg == 2) {
            _PyErr_Format(tstate, PyExc_TypeError,
                          "'async with' received an object from __aexit__ "
                          "that does not implement __await__: %.100s",
                          type->tp_name);
        }
    }
}


Py_ssize_t
PyUnstable_Eval_RequestCodeExtraIndex(freefunc free)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    Py_ssize_t new_index;

    if (interp->co_extra_user_count == MAX_CO_EXTRA_USERS - 1) {
        return -1;
    }
    new_index = interp->co_extra_user_count++;
    interp->co_extra_freefuncs[new_index] = free;
    return new_index;
}

/* Implement Py_EnterRecursiveCall() and Py_LeaveRecursiveCall() as functions
   for the limited API. */

int Py_EnterRecursiveCall(const char *where)
{
    return _Py_EnterRecursiveCall(where);
}

void Py_LeaveRecursiveCall(void)
{
    _Py_LeaveRecursiveCall();
}

PyObject *
_PyEval_GetANext(PyObject *aiter)
{
    unaryfunc getter = NULL;
    PyObject *next_iter = NULL;
    PyTypeObject *type = Py_TYPE(aiter);
    if (PyAsyncGen_CheckExact(aiter)) {
        return type->tp_as_async->am_anext(aiter);
    }
    if (type->tp_as_async != NULL){
        getter = type->tp_as_async->am_anext;
    }

    if (getter != NULL) {
        next_iter = (*getter)(aiter);
        if (next_iter == NULL) {
            return NULL;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                        "'async for' requires an iterator with "
                        "__anext__ method, got %.100s",
                        type->tp_name);
        return NULL;
    }

    PyObject *awaitable = _PyCoro_GetAwaitableIter(next_iter);
    if (awaitable == NULL) {
        _PyErr_FormatFromCause(
            PyExc_TypeError,
            "'async for' received an invalid object "
            "from __anext__: %.100s",
            Py_TYPE(next_iter)->tp_name);
    }
    Py_DECREF(next_iter);
    return awaitable;
}

void
_PyEval_LoadGlobalStackRef(PyObject *globals, PyObject *builtins, PyObject *name, _PyStackRef *writeto)
{
    if (PyDict_CheckExact(globals) && PyDict_CheckExact(builtins)) {
        _PyDict_LoadGlobalStackRef((PyDictObject *)globals,
                                    (PyDictObject *)builtins,
                                    name, writeto);
        if (PyStackRef_IsNull(*writeto) && !PyErr_Occurred()) {
            /* _PyDict_LoadGlobal() returns NULL without raising
                * an exception if the key doesn't exist */
            _PyEval_FormatExcCheckArg(PyThreadState_GET(), PyExc_NameError,
                                        NAME_ERROR_MSG, name);
        }
    }
    else {
        /* Slow-path if globals or builtins is not a dict */
        /* namespace 1: globals */
        PyObject *res;
        if (PyMapping_GetOptionalItem(globals, name, &res) < 0) {
            *writeto = PyStackRef_NULL;
            return;
        }
        if (res == NULL) {
            /* namespace 2: builtins */
            if (PyMapping_GetOptionalItem(builtins, name, &res) < 0) {
                *writeto = PyStackRef_NULL;
                return;
            }
            if (res == NULL) {
                _PyEval_FormatExcCheckArg(
                            PyThreadState_GET(), PyExc_NameError,
                            NAME_ERROR_MSG, name);
                *writeto = PyStackRef_NULL;
                return;
            }
        }
        *writeto = PyStackRef_FromPyObjectSteal(res);
    }
}

PyObject *
_PyEval_GetAwaitable(PyObject *iterable, int oparg)
{
    PyObject *iter = _PyCoro_GetAwaitableIter(iterable);

    if (iter == NULL) {
        _PyEval_FormatAwaitableError(PyThreadState_GET(),
            Py_TYPE(iterable), oparg);
    }
    else if (PyCoro_CheckExact(iter)) {
        PyCoroObject *coro = (PyCoroObject *)iter;
        int8_t frame_state = FT_ATOMIC_LOAD_INT8_RELAXED(coro->cr_frame_state);
        if (frame_state == FRAME_SUSPENDED_YIELD_FROM ||
            frame_state == FRAME_SUSPENDED_YIELD_FROM_LOCKED)
        {
            /* `iter` is a coroutine object that is being awaited. */
            Py_CLEAR(iter);
            _PyErr_SetString(PyThreadState_GET(), PyExc_RuntimeError,
                             "coroutine is being awaited already");
        }
    }
    return iter;
}

PyObject *
_PyEval_LoadName(PyThreadState *tstate, _PyInterpreterFrame *frame, PyObject *name)
{

    PyObject *value;
    if (frame->f_locals == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError,
                            "no locals found");
        return NULL;
    }
    if (PyMapping_GetOptionalItem(frame->f_locals, name, &value) < 0) {
        return NULL;
    }
    if (value != NULL) {
        return value;
    }
    if (PyDict_GetItemRef(frame->f_globals, name, &value) < 0) {
        return NULL;
    }
    if (value != NULL) {
        return value;
    }
    if (PyMapping_GetOptionalItem(frame->f_builtins, name, &value) < 0) {
        return NULL;
    }
    if (value == NULL) {
        _PyEval_FormatExcCheckArg(
                    tstate, PyExc_NameError,
                    NAME_ERROR_MSG, name);
    }
    return value;
}

static _PyStackRef
foriter_next(PyObject *seq, _PyStackRef index)
{
    assert(PyStackRef_IsTaggedInt(index));
    assert(PyTuple_CheckExact(seq) || PyList_CheckExact(seq));
    intptr_t i = PyStackRef_UntagInt(index);
    if (PyTuple_CheckExact(seq)) {
        size_t size = PyTuple_GET_SIZE(seq);
        if ((size_t)i >= size) {
            return PyStackRef_NULL;
        }
        return PyStackRef_FromPyObjectNew(PyTuple_GET_ITEM(seq, i));
    }
    PyObject *item = _PyList_GetItemRef((PyListObject *)seq, i);
    if (item == NULL) {
        return PyStackRef_NULL;
    }
    return PyStackRef_FromPyObjectSteal(item);
}

_PyStackRef _PyForIter_VirtualIteratorNext(PyThreadState* tstate, _PyInterpreterFrame* frame, _PyStackRef iter, _PyStackRef* index_ptr)
{
    PyObject *iter_o = PyStackRef_AsPyObjectBorrow(iter);
    _PyStackRef index = *index_ptr;
    if (PyStackRef_IsTaggedInt(index)) {
        *index_ptr = PyStackRef_IncrementTaggedIntNoOverflow(index);
        return foriter_next(iter_o, index);
    }
    PyObject *next_o = (*Py_TYPE(iter_o)->tp_iternext)(iter_o);
    if (next_o == NULL) {
        if (_PyErr_Occurred(tstate)) {
            if (_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
                _PyEval_MonitorRaise(tstate, frame, frame->instr_ptr);
                _PyErr_Clear(tstate);
            }
            else {
                return PyStackRef_ERROR;
            }
        }
        return PyStackRef_NULL;
    }
    return PyStackRef_FromPyObjectSteal(next_o);
}

/* Check if a 'cls' provides the given special method. */
static inline int
type_has_special_method(PyTypeObject *cls, PyObject *name)
{
    // _PyType_Lookup() does not set an exception and returns a borrowed ref
    assert(!PyErr_Occurred());
    PyObject *r = _PyType_Lookup(cls, name);
    return r != NULL && Py_TYPE(r)->tp_descr_get != NULL;
}

int
_PyEval_SpecialMethodCanSuggest(PyObject *self, int oparg)
{
    PyTypeObject *type = Py_TYPE(self);
    switch (oparg) {
        case SPECIAL___ENTER__:
        case SPECIAL___EXIT__: {
            return type_has_special_method(type, &_Py_ID(__aenter__))
                   && type_has_special_method(type, &_Py_ID(__aexit__));
        }
        case SPECIAL___AENTER__:
        case SPECIAL___AEXIT__: {
            return type_has_special_method(type, &_Py_ID(__enter__))
                   && type_has_special_method(type, &_Py_ID(__exit__));
        }
        default:
            Py_FatalError("unsupported special method");
    }
}
