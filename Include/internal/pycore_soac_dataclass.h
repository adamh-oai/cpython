#ifndef Py_INTERNAL_SOAC_DATACLASS_H
#define Py_INTERNAL_SOAC_DATACLASS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_typedefs.h"

/* Only an immediate real Python parent can transmit context implicitly.
 * C entry/trampoline frames are barriers. Explicit root/exec bridges supply
 * their owned invocation instead; no thread-local or Python attribute state. */
PyAPI_FUNC(int) _PySOAC_DataclassEnterFrame(_PyInterpreterFrame *frame);
extern int _PySOAC_DataclassEnterExplicit(
    PyObject *invocation, unsigned int stage, _PyInterpreterFrame *parent,
    _PyInterpreterFrame *frame);
extern PyObject *_PySOAC_DataclassEvalVector(
    PyThreadState *tstate, PyFunctionObject *function, PyObject *locals,
    PyObject *const *args, size_t argcount, PyObject *kwnames,
    PyObject *invocation, unsigned int stage, _PyInterpreterFrame *parent);
/* These calls are only for opcode dispatch. The supplied parent is the
 * actual caller of this exact operand; public/C vectorcall remains a
 * provenance barrier, even when it forwards to the same Python function. */
PyAPI_FUNC(PyObject *) _PySOAC_DataclassVectorcallFromFrame(
    _PyInterpreterFrame *parent, PyObject *callable, PyObject *const *args,
    size_t nargsf, PyObject *kwnames);
PyAPI_FUNC(PyObject *) _PySOAC_DataclassObjectCallFromFrame(
    _PyInterpreterFrame *parent, PyObject *callable, PyObject *args,
    PyObject *kwargs);
PyAPI_FUNC(PyObject *) _PySOAC_FunctionFromFrame(
    PyObject *code, PyObject *globals, _PyInterpreterFrame *producer);
/* Native creation-boundary registration only; these do not read late Python
 * bindings. Helpers' ordinary C entry is always context-free. */
extern int _PySOAC_DataclassCaptureBuiltin(
    PyInterpreterState *interp, unsigned int kind, PyObject *builtin);
PyAPI_FUNC(int) _PySOAC_DataclassAddHelpers(PyObject *module);
PyAPI_FUNC(int) _PySOAC_DataclassIsBridgeImplementation(PyObject *callable);

/* Begin consumes the exact fresh record before allocating the operation.
 * The returned opaque GC owner pins this operation's operands. Both the
 * type setter and dictionary policy check that SAME operation, not a
 * reusable permission on the invocation or interpreter. */
extern PyObject *_PySOAC_DataclassBeginMember(
    PyObject *invocation, PyObject *actual_type, PyObject *expected_class_owner,
    PyObject *name, PyObject *function);

/* Zero means exact match; -1 preserves the native strict error. Successful
 * validation performs no allocation or Python callback. */
extern int _PySOAC_DataclassCheckMember(
    PyObject *operation, PyObject *actual_type, PyObject *expected_class_owner,
    PyObject *name, PyObject *function);

/* Mark completion/failure before releasing any references. The caller then
 * decrefs the operation. Failure leaves previous restrictions installed. */
extern void _PySOAC_DataclassFinishMember(PyObject *operation, int succeeded);

#endif
