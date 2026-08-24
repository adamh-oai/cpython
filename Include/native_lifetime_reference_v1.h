/* Native-reference code-primary transfer for SOAC lifetime frames.
 * Native reference operations and lifetime finish are declared in
 * native_reference_v1.h. Construction does not authorize execution.
 */
#ifndef PY_SOAC_LIFETIME_REFERENCE_V1_H
#define PY_SOAC_LIFETIME_REFERENCE_V1_H

#include "native_reference_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A successful native-reference ABI query is a prerequisite. code_owner must
 * address a live, aligned, nonempty HEAP-SAFE token for an actual code object.
 * The incoming native binder f_executable token satisfies this precondition.
 *
 * Validate and allocate the entire frame BEFORE changing code_owner. On every
 * recoverable failure the incoming token and debug handle remain unchanged.
 * Success MOVES that exact token into the lifetime frame's f_executable and
 * publishes native Empty in code_owner. No DUP, extra code INCREF, conversion
 * through an owning PyObject pointer, or debug-handle recreation occurs.
 * Native child borrows continue to depend on the same moved support handle.
 *
 * Allocation follows the existing lifetime-frame pending-error policy. Success
 * preserves a pending exception; allocation failure reports its MemoryError.
 * This does not enter a source-parent scope or authorize source execution.
 *
 * The caller's frame reference now IS the activation's code primary. After
 * Terminal Leave and Finish, retire locals/namespace/function in native order,
 * then release this frame reference LAST. An escaped frame may keep that same
 * code reference; unique frames must not release it before ordinary primaries.
 */
PyAPI_FUNC(PyFrameObject *) PyFrame_NewSoacLifetimeWithReferenceV1(
    PySoacRefV1 *code_owner);

#ifdef __cplusplus
}
#endif
#endif

