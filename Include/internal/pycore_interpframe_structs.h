/* Structures used by pycore_debug_offsets.h.
 *
 * See InternalDocs/frames.md for an explanation of the frame stack
 * including explanation of the PyFrameObject and _PyInterpreterFrame
 * structs.
 */

#ifndef Py_INTERNAL_INTERP_FRAME_STRUCTS_H
#define Py_INTERNAL_INTERP_FRAME_STRUCTS_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_structs.h"       // _PyStackRef
#include "pycore_typedefs.h"      // _PyInterpreterFrame

#ifdef __cplusplus
extern "C" {
#endif

enum _frameowner {
    FRAME_OWNED_BY_THREAD = 0,
    FRAME_OWNED_BY_GENERATOR = 1,
    FRAME_OWNED_BY_FRAME_OBJECT = 2,
    FRAME_OWNED_BY_INTERPRETER = 3,
};

struct _PyInterpreterFrame {
    _PyStackRef f_executable; /* Deferred or strong reference (code object or None) */
    struct _PyInterpreterFrame *previous;
    _PyStackRef f_funcobj; /* Deferred or strong reference. Only valid if not on C stack */
    PyObject *f_globals; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_builtins; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_locals; /* Strong reference, may be NULL. Only valid if not on C stack */
    PyFrameObject *frame_obj; /* Strong reference, may be NULL. Only valid if not on C stack */
    _Py_CODEUNIT *instr_ptr; /* Instruction currently executing (or about to begin) */
    _PyStackRef *stackpointer;
#ifdef Py_GIL_DISABLED
    /* Index of thread-local bytecode containing instr_ptr. */
    int32_t tlbc_index;
#endif
    uint16_t return_offset;  /* Only relevant during a function call */
    char owner;
#ifdef Py_DEBUG
    uint8_t visited:1;
    uint8_t lltrace:7;
#else
    uint8_t visited;
#endif
    /* Native adapter provenance, never original-strict-bytecode permission.
     * The scalar occupies existing GIL-build padding. The GC edge is cleared
     * when execution ends, even if a Python traceback retains the frame. */
    unsigned int soac_dataclass_role;
    PyObject *soac_dataclass_invocation;
    /* One mutually exclusive generated-dataclass or interpreter activation.
     * This metadata edge retires before ordinary escaped-frame promotion. */
    PyObject *soac_checked_activation;
#if !defined(Py_GIL_DISABLED) && defined(Py_STACKREF_DEBUG)
    /* Diagnostic-only borrowed view of f_executable. Signal/watchdog dumps
     * cannot consult the mutable debug handle table or an attached tstate.
     * This is not an owning reference, GC edge, or runtime code accessor. */
    PyObject *f_executable_view;
#endif
    /* Locals and stack */
    _PyStackRef localsplus[1];
};


/* _PyGenObject_HEAD defines the initial segment of generator
   and coroutine objects. */
struct _PySoacManagedGenerator;

#define _PyGenObject_HEAD(prefix)                                           \
    PyObject_HEAD                                                           \
    /* List of weak reference. */                                           \
    PyObject *prefix##_weakreflist;                                         \
    /* Name of the generator. */                                            \
    PyObject *prefix##_name;                                                \
    /* Qualified name of the generator. */                                  \
    PyObject *prefix##_qualname;                                            \
    _PyErr_StackItem prefix##_exc_state;                                    \
    PyObject *prefix##_origin_or_finalizer;                                 \
    struct _PySoacManagedGenerator *prefix##_soac_managed;                   \
    char prefix##_hooks_inited;                                             \
    char prefix##_closed;                                                   \
    char prefix##_running_async;                                            \
    /* The frame */                                                         \
    int8_t prefix##_frame_state;                                            \
    _PyInterpreterFrame prefix##_iframe;                                    \

struct _PyGenObject {
    /* The gi_ prefix is intended to remind of generator-iterator. */
    _PyGenObject_HEAD(gi)
};

struct _PyCoroObject {
    _PyGenObject_HEAD(cr)
};

struct _PyAsyncGenObject {
    _PyGenObject_HEAD(ag)
};

#undef _PyGenObject_HEAD


#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_INTERP_FRAME_STRUCTS_H
