/* Private native CALL/KW/EX producer storage, never a Rust layout. */
#ifndef Py_INTERNAL_SOAC_VM_CALL_V1_H
#define Py_INTERNAL_SOAC_VM_CALL_V1_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_soac_source_entry_v1.h"

enum {
    _Py_SOAC_VM_CALL_FAILED_V1 = 1,
    _Py_SOAC_VM_CALL_BOUND_V1,
    _Py_SOAC_VM_CALL_FINISHED_V1,
};

typedef struct {
    uint32_t state;
    int pinned;
    PySoacBoundCallViewV1 view;   /* initialized only when native Bind starts */
    PySoacSourceEntrySpecV1 entry;
    void *context;
    unsigned char *supplied;
    unsigned char inline_supplied[8];
} _PySoacVMCallV1;

/* Current-identity query, with the same callback-free/no-error semantics as
 * CopyV1. It does not pin the record or grant execution authority. */
PyAPI_FUNC(int) _PySoacVMCall_IsRegisteredV1(PyObject *function);

/* Consistency check on an already authenticated registrar role. Returns 0 or
 * -1/PyErr before any EX input conversion/unpacking/consumption. Never use
 * CO_OPTIMIZED as a substitute for the catalogue/role/body correspondence. */
PyAPI_FUNC(int) _PySoacVMCall_RequireOptimizedExpandedV1(PyObject *function);

/* Fresh address-stable call. Consume function, raw owned locals, and every
 * argument token on BOTH outcomes. No argument token is normalized to owned.
 * kwnames is borrowed; the opcode closes its actual token after this returns.
 * Return Bound or Failed. Do not Execute or Release until the opcode has
 * retired its remaining inputs and published DEAD/SYNC_SP. */
PyAPI_FUNC(void) _PySoacVMCall_BindVectorV1(
    _PySoacVMCallV1 *call, PyThreadState *thread, uint32_t kind,
    _PyStackRef function, PyObject *locals, const _PyStackRef *arguments,
    Py_ssize_t positional, PyObject *kwnames);

/* Uninstrumented exact-function EX only. Tuple normalization and the native
 * early CO_OPTIMIZED consistency check precede this consuming call. All
 * legitimate registrar roles have CO_OPTIMIZED, hence native locals is NULL.
 * A nonoptimized original must be rejected before consuming any VM input;
 * the separate ordinary-native nonoptimized EX error finding stays unresolved.
 *
 * Consumes function/callargs/kwargs on success/error. Native unpacking and
 * token acquisition precede final code capture/Pin/Bind. Retire unpack scratch
 * and key tuple, then callargs, then kwargs before returning to DEAD/SYNC_SP.
 * No public borrowed adapter or second source body is called here. */
PyAPI_FUNC(void) _PySoacVMCall_BindExpandedV1(
    _PySoacVMCallV1 *call, PyThreadState *thread, _PyStackRef function,
    Py_ssize_t positional, PyObject *callargs, PyObject *kwargs);

/* After actual opcode retirement and SP publication, exactly once: Failed
 * releases the immutable context if Pin occurred, preserving the bind error.
 * Bound marks BodyReady then Execute owns the context. Returns the exact
 * heap-safe result token transported to native form, or native NULL + PyErr.
 * It never converts a native operand through a borrowed PyObject vector. */
PyAPI_FUNC(_PyStackRef) _PySoacVMCall_FinishV1(
    _PySoacVMCallV1 *call, _PyStackRef *published_stackpointer);

#endif
