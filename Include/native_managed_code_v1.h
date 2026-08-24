/* ADDITIVE REVIEW DRAFT ONLY. No source body is registered or enabled.
 * Includes the immutable lifetime22/native-reference headers; does not edit
 * their ABI or the existing PySoacGeneratorSpec/legacy constructors.
 */
#ifndef PY_SOAC_MANAGED_CODE_V1_DRAFT_H
#define PY_SOAC_MANAGED_CODE_V1_DRAFT_H

#include "native_lifetime_reference_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caller holds the matching native-reference ABI, GIL, live function/owner,
 * and an exclusive aligned already-heap-safe code token. Select the exact
 * generator/coroutine/asyncgen family from that captured code, not func_code.
 *
 * Validation/allocation failure BEFORE bind observation leaves the exact
 * input handle unchanged. Once the fully initialized native object can be
 * observed by spec.bind, its executable owns the moved token and code_owner
 * is Empty, including on a subsequent error return. Such failure uses the
 * existing fixed clear/destructor path; an escaped failed-bind object keeps
 * its captured code. There is no rollback, extra code INCREF, or new grant.
 */
PyAPI_FUNC(PyObject *) PySoac_NewManagedWithReferenceV1(
    PyObject *function, PySoacRefV1 *code_owner,
    PyObject *name, PyObject *qualname, PyObject *owner,
    const PySoacGeneratorSpec *spec);

/* First actual EXECUTING managed step, exact live owner and transferred-code
 * mode only. Allocate before creating a native Borrow of the gen executable.
 * No code/gen owner is acquired. The caller must publish at most one lifetime
 * view per activation and Finish/drop it before closing its code support.
 * Created.throw is a real step; an incomplete factory is not. Scope entry is
 * separate. Existing optimized-frame introspection refusals remain unchanged.
 */
PyAPI_FUNC(PyFrameObject *) PyFrame_NewSoacManagedLifetimeV1(
    PyObject *generator, PyObject *owner);

#define PySoac_GENERATOR_CLEANUP_ABI_VERSION 1
#define PySoac_GENERATOR_CLEANUP_V1_WORDS 8
#define PySoac_GENERATOR_RETIREMENT_V1_SLOTS 3
enum {
    PySoac_GENERATOR_RETIRE_CODE = 0,
    PySoac_GENERATOR_RETIRE_NAME = 1,
    PySoac_GENERATOR_RETIRE_QUALNAME = 2,
};

typedef struct {
    uintptr_t _opaque[PySoac_GENERATOR_CLEANUP_V1_WORDS];
} PySoacGeneratorCleanupV1;

typedef struct {
    uint32_t version;
    uint32_t word_count;
    uint32_t retirement_count;
    uint32_t reserved;  /* zero */
    size_t size;
    size_t alignment;
} PySoacGeneratorCleanupLayoutV1;

/* Exact successful reply only; failure leaves output unchanged. Query before
 * activation/cleanup, not after owner cleanup has become non-recoverable. */
PyAPI_FUNC(int) PyGen_GetSoacGeneratorCleanupLayoutV1(
    uint32_t version, size_t layout_size,
    PySoacGeneratorCleanupLayoutV1 *out);

/* All-zero, one-use, stable/address-pinned scope; same interpreter/thread/GIL
 * until End. The three live native-token slots are separately initialized by
 * PySoacRef_InitSlotsV1, all Empty. Count must be exactly three. Sink/scope
 * storage must not overlap each other, the live gen, or its native metadata.
 *
 * The caller must establish live/retiring association BEFORE dereferencing a
 * remembered generator pointer: use its existing capsule state or the exact
 * clear callback's generator argument. A terminal escaped capsule must not
 * attempt Begin using stale Binding.generator. Native validates that live
 * association, transferred mode and absence of another cleanup link.
 *
 * Success does not change execution phase or any object refcount. If gen
 * deallocation subsequently survives finalization without resurrection, the
 * destructor moves its executable plus name/qualname into these exact slots,
 * empties all original fields, unlinks the gen, then frees native storage.
 * Code's native debug handle is unchanged. Names adopt their original owned
 * PyObject edges via FromOwned, without INCREF, at this actual transfer only.
 * No other path fills any slot; failed Begin and surviving-gen End leave all
 * three Empty. The sink's owner must traverse every nonempty token natively.
 *
 * Public invalid input returns -1 before partial publication; validated
 * internal callers must not unwind through owner cleanup on invariant error.
 */
PyAPI_FUNC(int) PyGen_BeginSoacGeneratorCleanupV1(
    PySoacGeneratorCleanupV1 *cleanup, size_t cleanup_size,
    PyObject *generator, PyObject *owner,
    PySoacRefV1 *retirement_slots, Py_ssize_t retirement_count);

/* 0: unlinked a still-live gen, all slots Empty. 1: gen was destroyed and the
 * exact three original owners were transferred. -1: invalid/copied/wrong-
 * thread/reused scope, unchanged. After gen death, End uses only the scope's
 * own transferred state; it must not inspect an old Binding.generator.
 *
 * End NEVER closes or removes a sink token. After all ordinary source cleanup
 * and borrowed-frame Finish/drop, caller closes CODE, then NAME, then QUALNAME,
 * preserving its existing PyErr policy. Code weakref callbacks see both names
 * still live, just as in native gen_dealloc. No linked scope or nonempty sink
 * may escape terminal capsule cleanup. A reentrant close that leaves gen alive
 * does not unlink the scope: a later finalizer may still destroy that gen.
 */
PyAPI_FUNC(int) PyGen_EndSoacGeneratorCleanupV1(
    PySoacGeneratorCleanupV1 *cleanup, size_t cleanup_size);

#ifdef __cplusplus
}
#endif
#endif
