#ifndef Py_INTERNAL_SOAC_DATACLASS_H
#define Py_INTERNAL_SOAC_DATACLASS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_typedefs.h"

/* Only an immediate real Python parent can transmit context implicitly.
 * C entry/trampoline frames are barriers. Explicit root/exec bridges supply
 * their owned invocation instead; no thread-local or Python attribute state. */
struct _PySoacInterpreterCallV1;
extern int _PySOAC_DataclassBeginRoot(PyObject *, unsigned int, PyObject *);
extern int _PySOAC_DataclassAttachRoot(
    PyObject **, unsigned int, _PyInterpreterFrame *,
    _PyInterpreterFrame *, const _Py_CODEUNIT *, struct _PySoacInterpreterCallV1 *);
extern void _PySOAC_DataclassTakeRoot(
    _PyInterpreterFrame *, PyObject **, unsigned int *,
    _PyInterpreterFrame **, const _Py_CODEUNIT **, struct _PySoacInterpreterCallV1 **);
extern int _PySOAC_DataclassFinishRoot(PyObject *, unsigned int, PyObject *);
extern PyObject *_PySOAC_DataclassRootOwner(PyObject *);
extern void _PySOAC_DataclassReleaseRoot(PyObject *);
extern void _PySOAC_DataclassCreationInfo(
    PyObject *, uint32_t *, uint32_t *, uint64_t *, PyObject **, PyObject **);

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
    PyObject *code, PyObject *globals, _PyInterpreterFrame *producer,
    const _Py_CODEUNIT *this_instr);
/* Native creation-boundary registration only; these do not read late Python
 * bindings. Helpers' ordinary C entry is always context-free. */
extern int _PySOAC_DataclassCaptureBuiltin(
    PyInterpreterState *interp, unsigned int kind, PyObject *builtin);
PyAPI_FUNC(int) _PySOAC_DataclassAddHelpers(PyObject *module);
PyAPI_FUNC(int) _PySOAC_DataclassIsBridgeImplementation(PyObject *callable);

/* A single opcode-owned stack context, valid only throughout this replacement
 * construction. It is never stored in a Python object or thread state. */
typedef struct _PySoacDataclassSlotsContext _PySoacDataclassSlotsContext;
extern _PySoacDataclassSlotsContext *_PySOAC_DataclassBeginSlotsHandle(
    const PySoacDataclassFrameView *, const PySoacTypeConstructionSpec *);
extern int _PySOAC_DataclassRecordSlotsHandle(
    _PySoacDataclassSlotsContext *, PyObject *, const PySoacTypeConstructionSpec *);
extern int _PySOAC_DataclassValidateSlotsHandle(
    _PySoacDataclassSlotsContext *, PyObject *, const PySoacTypeConstructionSpec *);
extern int _PySOAC_DataclassBindSlotsType(
    _PySoacDataclassSlotsContext *, PyObject *, PyObject *);
extern int _PySOAC_DataclassValidateCopiedHook(
    _PySoacDataclassSlotsContext *, PyObject *, PyObject *);
extern int _PySOAC_DataclassCopiedHookBirth(
    _PySoacDataclassSlotsContext *, PyObject *, PyObject *, uint64_t *);
extern int _PySOAC_DataclassMatchesInstalledHook(
    PyObject *function, uint64_t birth, unsigned int role);
extern int _PySOAC_DataclassFailSlots(_PySoacDataclassSlotsContext *, const char *);
extern PyObject *_PySOAC_TypeFromDataclassSlotsHandle(
    _PySoacDataclassSlotsContext *, PyObject *);
extern void _PySOAC_ClearDataclassSlotsHandle(PyObject *);

/* Structural safety for a generated function exposed before its closure
 * attributes have been populated. No argument or return type predicates. */
extern int _PySOAC_DataclassCheckFrameConstruction(_PyInterpreterFrame *frame);

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

/* The selected dictionary kernel calls Check before lookup and again after
 * its last equality/watcher callback, immediately before physical effect.
 * incoming_exact_name is the original native member name, NOT a canonical
 * stored key. Returns 1 pending, 0 legacy, -1 refusal. Success is callback- and
 * allocation/reference-free. The registered validate_member callback is
 * confined to BeginMember/the initial type write, before native dictionary
 * resolution; neither call here invokes it. A first check is never a reusable
 * mutation permit. Failure may set an error/terminalize, but never commits. */
extern int _PySOAC_DataclassPendingMemberCheck(
    PyObject *operation, PyObject *actual_dict, PyObject *incoming_exact_name,
    PyObject *value, PyObject *expected_class_owner);
/* Physical dictionary effect and this scalar birth publication are adjacent:
 * no callback or failure is allowed between them. No operand/owner is added. */
extern void _PySOAC_DataclassPendingMemberCommit(PyObject *operation);

/* Mark completion/failure before releasing any references. The caller then
 * decrefs the operation. Failure leaves previous restrictions installed. */
extern void _PySOAC_DataclassFinishMember(PyObject *operation, int succeeded);

#endif
