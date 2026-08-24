/* DESIGN DRAFT ONLY. Not installed, built, or registered in native20.
 * Proposed public additions; the implementation stays in CPython C.
 * See TRANSACTION-V1.md for producer ordering and required joint consumers.
 */
#ifndef PY_SOAC_NATIVE_REFERENCE_V1_DRAFT_H
#define PY_SOAC_NATIVE_REFERENCE_V1_DRAFT_H

#include <Python.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Py_SOAC_REFERENCE_ABI_V1 1u
#define Py_SOAC_SOURCE_ENTRY_ABI_V1 1u
#define Py_SOAC_REFERENCE_GIL 1u
#define Py_SOAC_REFERENCE_DEBUG_HANDLES 2u
#define Py_SOAC_REFERENCE_NATIVE_GC_VISIT 4u

/* One native reference, not necessarily one object refcount. The field is
 * transport only: never decode, compare, zero-initialize, serialize, or use
 * a bit-copy as DUP. A move preserves a debug handle and its borrow ancestry.
 * One close obligation exists per live value, including a borrowed token.
 * Same-interpreter use with the GIL; heap-safe values may migrate threads.
 */
typedef struct { uintptr_t _opaque; } PySoacRefV1;

typedef struct {
    uint32_t abi_version;
    uint32_t capabilities;
    size_t reference_size;
    size_t reference_alignment;
    PySoacRefV1 empty;
} PySoacReferenceAbiV1;

/* Exact sizeof(*out) required. Returns 0/-1; unsupported GIL/width/build
 * returns NotImplementedError before any source-entry registration. V1
 * supports the selected 64-bit GIL build and its Py_STACKREF_DEBUG variant.
 */
PyAPI_FUNC(int) PySoacRef_GetAbiV1(PySoacReferenceAbiV1 *out, size_t out_size);
PyAPI_FUNC(PySoacRefV1) PySoacRef_EmptyV1(void);
PyAPI_FUNC(void) PySoacRef_InitSlotsV1(PySoacRefV1 *slots, Py_ssize_t count);
PyAPI_FUNC(int) PySoacRef_IsEmptyV1(PySoacRefV1 value);
PyAPI_FUNC(int) PySoacRef_IsHeapSafeV1(PySoacRefV1 value);
PyAPI_FUNC(int) PySoacRef_IsV1(PySoacRefV1 left, PySoacRefV1 right);

/* Non-consuming view. Empty produces NULL without setting/clearing PyErr.
 * The caller must not retain the pointer beyond the supporting token/owner.
 */
PyAPI_FUNC(PyObject *) PySoacRef_ObjectViewV1(PySoacRefV1 value);

/* Both return a NEW closeable token; neither consumes value. value must be
 * nonempty. DUP of an already-borrowed mortal preserves its native ancestry;
 * Borrow creates a dependency on this exact source token. They are distinct
 * from ObjectView and from Rust's non-owning SSA mirror of a physical slot.
 */
PyAPI_FUNC(PySoacRefV1) PySoacRef_DupV1(PySoacRefV1 value);
PyAPI_FUNC(PySoacRefV1) PySoacRef_BorrowV1(PySoacRefV1 value);

/* Non-NULL object inputs. FromOwned consumes one Python reference;
 * FromBorrowed acquires exactly the native public-C-argument reference.
 */
PyAPI_FUNC(PySoacRefV1) PySoacRef_FromOwnedV1(PyObject *value);
PyAPI_FUNC(PySoacRefV1) PySoacRef_FromBorrowedV1(PyObject *value);

/* IntoOwned and MakeHeapSafe consume their token. IntoOwned requires
 * nonempty; MakeHeapSafe accepts Empty. They preserve native debug checking,
 * including the requirement to finish child borrows before closing/promoting
 * their support token. Never promote every active preserved Store eagerly.
 */
PyAPI_FUNC(PyObject *) PySoacRef_IntoOwnedV1(PySoacRefV1 value);
PyAPI_FUNC(PySoacRefV1) PySoacRef_MakeHeapSafeV1(PySoacRefV1 value);

/* Slot operations MOVE, without DUP/INCREF/Close/callbacks. Exchange publishes
 * incoming before returning the displaced token. Invalidate checked-value
 * proofs before a source primary is mutated; Close the displaced token at
 * the selected native phase, not inside Exchange.
 */
PyAPI_FUNC(PySoacRefV1) PySoacRef_TakeV1(PySoacRefV1 *slot);
PyAPI_FUNC(PySoacRefV1) PySoacRef_ExchangeV1(
    PySoacRefV1 *slot, PySoacRefV1 incoming);
PyAPI_FUNC(void) PySoacRef_CloseV1(PySoacRefV1 value);

/* Close accepts Empty and otherwise uses native XCLOSE, not Py_DECREF on
 * an object view. It may run finalizers and does not add a new error policy;
 * the already-selected error-cleanup transaction preserves pending PyErr.
 * Visit uses _PyGC_VisitStackRef, including its special subtracting visitor
 * handling. Do NOT globally skip borrowed tokens for ordinary GC visitors.
 */
PyAPI_FUNC(int) PySoacRef_VisitOwnerV1(
    PySoacRefV1 value, visitproc visit, void *arg);

/* Exact cells only. Get returns the native acquired value or Empty/no error
 * for an unbound cell. MakeCell mirrors MAKE_CELL: allocate from the current
 * RAW object (possibly itself a cell), publish the new cell token, then close
 * the displaced token. On allocation failure the input slot is unchanged.
 */
PyAPI_FUNC(PySoacRefV1) PySoacRef_CellGetV1(PyObject *cell);
PyAPI_FUNC(int) PySoacRef_MakeCellV1(PySoacRefV1 *slot);

/* Callback-only; never a Python object, persistent capability, or public
 * interpreter-frame pointer. Native binding has succeeded and ordinary
 * caller-stack / keyword-container retirement has reached BodyReady before
 * execute receives this view. No original bytecode is evaluated.
 */
typedef struct PySoacBoundCallViewV1 PySoacBoundCallViewV1;

enum {
    Py_SOAC_CALL_BORROWED_VECTOR_V1 = 0,
    Py_SOAC_CALL_VM_POSITIONAL_V1 = 1,
    Py_SOAC_CALL_VM_KEYWORDS_V1 = 2,
    Py_SOAC_CALL_VM_EXPANDED_V1 = 3
};

typedef struct {
    uint32_t kind;
    uint32_t reserved;
    PyObject *function;             /* borrowed actual admitted function */
    PyCodeObject *code;             /* borrowed CAPTURED native frame code */
    PyObject *globals;              /* borrowed captured activation maps */
    PyObject *builtins;
    Py_ssize_t parameter_count;     /* pos + kwonly + optional varargs and varkwargs */
    Py_ssize_t supplied_count;      /* pos + kwonly, before default filling */
} PySoacSourceCallInfoV1;

typedef struct {
    PySoacRefV1 function;           /* exact native f_funcobj token */
    PySoacRefV1 code;               /* exact native f_executable token */
    PySoacRefV1 namespace_value;    /* native f_locals owner, or Empty */
    PyObject *globals;              /* borrowed; existing activation owns maps */
    PyObject *builtins;
} PySoacBoundActivationV1;

PyAPI_FUNC(int) PySoacCall_GetInfoV1(
    const PySoacBoundCallViewV1 *view,
    PySoacSourceCallInfoV1 *out, size_t out_size);

/* Native binding is already complete. Valid TakeBinding is callback- and
 * allocation-free. Exact-size buffers and Empty output slots are required.
 * Native parameter order is co_argcount, co_kwonlyargcount, then *args and
 * **kwargs when present. Supplied bytes cover only pos/kwonly parameters.
 * Move all outputs once, detach every native primary, pop the now-empty
 * temporary frame, and mark the view Taken. No separate binder owner remains.
 * Invalid arguments return -1 with the still-Bound view unchanged, allowing
 * Abort; a successful take makes further view access invalid.
 */
PyAPI_FUNC(int) PySoacCall_TakeBindingV1(
    PySoacBoundCallViewV1 *view,
    PySoacBoundActivationV1 *activation, size_t activation_size,
    PySoacRefV1 *parameters, Py_ssize_t parameter_count,
    unsigned char *supplied, Py_ssize_t supplied_count);

/* Bound -> Aborted, preserving an already-pending exception. Publish terminal
 * view state before native frame clear can reenter. No borrowed function/code
 * accessor is valid after this call, including on the Rust error Drop path.
 */
PyAPI_FUNC(int) PySoacCall_AbortV1(PySoacBoundCallViewV1 *view);

/* Pin is infallible and callback-/allocation-free: it acquires only an
 * immutable Rust execution/metadata handle, NOT new Python source-value refs.
 * It runs before native binding, while the initial function/code are live.
 * A pinned Arc does not resurrect sealed-language or checked-value authority.
 * Release is used ONLY for native bind failure; function/code may be dead.
 * Execute owns context on both success/error and retires it with the actual
 * activation, not in a C tail after source teardown. It must Take or Abort.
 * Success: return 0, nonempty HEAP-SAFE result, no pending exception.
 * Error: return -1, Empty result, pending exception. No decline/retry result.
 */
typedef void *(*PySoacPinSourceCallV1)(
    void *metadata, const PySoacSourceCallInfoV1 *original);
typedef void (*PySoacReleaseSourceCallV1)(void *context);
typedef int (*PySoacExecuteSourceCallV1)(
    PySoacBoundCallViewV1 *view, void *context, PySoacRefV1 *result);

typedef struct {
    uint32_t abi_version;
    uint32_t reserved;
    uint64_t expected_function_id;
    uint64_t expected_strict_id;
    PyCodeObject *expected_code;
    PyObject *expected_owner;
    void *expected_metadata;
    vectorcallfunc expected_vectorcall;
    PySoacPinSourceCallV1 pin;
    PySoacReleaseSourceCallV1 release;
    PySoacExecuteSourceCallV1 execute;
} PySoacSourceEntrySpecV1;

/* Copy the typed spec into a function-owned native record. Identity operands
 * are borrowed from the actual function's already-live associations; no
 * Python reference pin is added. Reject stale size/version/owner/code/
 * metadata/vectorcall and non-source-signature compiler-private helpers.
 * Invalidate the record before metadata destructors, code/vectorcall writes,
 * or function/owner GC clearing can reenter. This revokes only future native
 * execution-hook admission, NEVER the required checked public boundary.
 */
PyAPI_FUNC(int) PyFunction_SetSoacSourceEntryV1(
    PyObject *function, const PySoacSourceEntrySpecV1 *spec, size_t spec_size);
PyAPI_FUNC(int) PyFunction_ClearSoacSourceEntryV1(PyObject *function);

/* Public C-vectorcall adapter: input array/function/kwnames are BORROWED.
 * Acquire references as native _PyEval_Vector does, then the same native
 * binding + BodyReady + checked Execute path. Returns an ordinary owned
 * PyObject result / NULL error. Never steals caller pointers, recursively
 * retries public vectorcall, or evaluates strict source bytecode.
 */
PyAPI_FUNC(PyObject *) PySoac_CallBorrowedVectorcallV1(
    PyObject *function, PyObject *const *args, size_t nargsf, PyObject *kwnames);

/* Native-reference counterpart to the existing lifetime-only frame finish.
 * Validate all rows first in original co_localsplus order. NULL row / Empty
 * is unbound; CELL/FREE rows must view an actual cell. Never cast the token
 * pointers to PyObject ** and never deduplicate equal objects.
 * If externally retained, copy object references into the native frame;
 * otherwise copy none. Valid Finish allocates nothing and calls no Python.
 * Primary tokens remain live and unchanged for the caller's ordered Close
 * transaction. Captured environment-map ownership/GC masks retain native19's
 * both-GC-orders behavior. Return 1 retained, 0 unique, -1 invalid untouched.
 */
PyAPI_FUNC(int) PyFrame_FinishSoacLifetimeWithReferencesV1(
    PyFrameObject *frame, PyObject *source_function,
    PyObject *captured_globals, PyObject *captured_builtins,
    PyObject *source_namespace,
    const PySoacRefV1 *const *source_owner_slots, Py_ssize_t count);

#ifdef __cplusplus
}
#endif
#endif
