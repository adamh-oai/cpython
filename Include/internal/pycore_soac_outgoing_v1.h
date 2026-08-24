/* IGNORED native22 implementation dependency. No Rust interpreter-frame ABI. */
#ifndef Py_INTERNAL_SOAC_OUTGOING_V1_H
#define Py_INTERNAL_SOAC_OUTGOING_V1_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "native_outgoing_v1.h"
#include "pycore_interpframe.h"

/* Read-only validation of the one existing scope representation. No new
 * owner, current-frame publication, pseudo position or source capability. */
extern _PyInterpreterFrame *_PyFrame_BorrowSoacLifetimeScopeV1(
    const PySoacLifetimeScopeV1 *scope, size_t scope_size,
    PyCodeObject *expected_code);

/* This validates a supplied, already authenticated emission association; it
 * never discovers a source operation from code/name/range/first-match. Only
 * CALL/C_RETURN/C_RAISE observation is implemented here. Other entitled source
 * observers, or an unprovable unknown-site EX form, fail before commit. */
extern int _PySoac_OutgoingCallSiteV1(
    PyThreadState *thread, PyCodeObject *code, Py_ssize_t byte_offset,
    uint32_t kind, Py_ssize_t argument_count,
    int *instrumented, _Py_CODEUNIT **instruction);

#endif
