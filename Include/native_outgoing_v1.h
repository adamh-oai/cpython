/* Native-reference outgoing calls with explicit source context.
 * These declarations do not establish source-body or call-site admission. */
#ifndef PY_SOAC_OUTGOING_REFERENCE_V1_H
#define PY_SOAC_OUTGOING_REFERENCE_V1_H

#include "native_reference_v1.h"
#include <frameobject.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Py_SOAC_OUTGOING_CONTEXT_ABI_V1 1u

typedef struct {
    uint32_t abi_version;
    uint32_t reserved;
    const PySoacLifetimeScopeV1 *source_scope;
    size_t source_scope_size;
    PyCodeObject *source_code;       /* borrowed exact scope code primary */
    Py_ssize_t instruction_offset;  /* proven original CALL emission, or -1 */
    PyObject *source_namespace;     /* existing borrowed FrameNamespace, or NULL */
} PySoacOutgoingCallContextV1;

/* Use only within this exact linked source scope and native thread. The
 * context owns nothing. Globals come from the scope; namespace is the existing
 * resolved call-context borrow, not a second namespace selection or owner.
 * A nonnegative offset requires a compiler-owned original native code/emission
 * association. Native boundary/arity validation does not establish that proof.
 * No line/range/display-name join or source-error-site offset is accepted as
 * a substitute. The lifetime frame's unavailable position is never changed.
 *
 * Unknown sites refuse if an entitled observer needs the omitted information,
 * or if native instrumented EX retirement is unprovable, even while tracing
 * suppresses callback delivery. Proven CALL/C_RETURN/C_RAISE form is snapshotted
 * before effects. Other source observers explicitly refuse. This is not a
 * whole-body monitoring contract or a trusted dataclass invocation protocol.
 *
 * Vector region: [callable, self-or-Empty, argument_count values, kwnames].
 * kind is Py_SOAC_CALL_VM_POSITIONAL_V1 or Py_SOAC_CALL_VM_KEYWORDS_V1.
 * POSITIONAL requires Empty kwnames; KEYWORDS requires its original exact tuple.
 * Method expansion occurs at the native phase. Equal objects are not deduped.
 */
PyAPI_FUNC(int) PySoac_CallVectorWithReferencesV1(
    const PySoacOutgoingCallContextV1 *context, size_t context_size,
    uint32_t kind, PySoacRefV1 *operands, Py_ssize_t argument_count,
    PySoacRefV1 *result);

/* Prepared EX region: [callable, Empty, exact tuple, exact dict-or-Empty].
 * Original expression evaluation, star normalization, tuple conversion and
 * keyword merging have already occurred in explicit source IR. Never replay.
 * Instrumented EX and bound-method EX retain their real borrowed-call supports;
 * only the selected uninstrumented exact-Python-function route consumes into
 * native callee primaries before the body.
 */
PyAPI_FUNC(int) PySoac_CallPreparedWithReferencesV1(
    const PySoacOutgoingCallContextV1 *context, size_t context_size,
    PySoacRefV1 *operands, PySoacRefV1 *result);

/* Result is Empty and disjoint from context/scope/operand storage on entry.
 * An already pending PyErr returns -1 unchanged, with no input consumption.
 * Invalid ABI/shape/site, unsupported observer, count overflow or pre-commit
 * native scratch OOM also returns -1/PyErr with all inputs unchanged.
 * Large vectors use one checked PyMem allocation before commit; its added
 * performance/MemoryError boundary remains subject to measurement/admission.
 *
 * After commit every input slot is Empty before any callback or reentrant
 * Close. Native handles move, never alias-cast/DUP/promote for transport.
 * Success is 0 + one heap-safe result + no PyErr. Committed error is -1/PyErr
 * with result Empty and all inputs consumed. There is no retry/decline result.
 * Caller cleanup closes only remaining nonempty slots in its selected order.
 * C/custom calls keep the original supporting tokens through return and C
 * completion monitoring, then Close in native order, never arbitrary DECREF.
 */

#ifdef __cplusplus
}
#endif
#endif
