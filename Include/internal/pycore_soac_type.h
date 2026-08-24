#ifndef Py_INTERNAL_SOAC_TYPE_H
#define Py_INTERNAL_SOAC_TYPE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* These operations consult native type-owned state, never Python attributes. */
extern int _PySOAC_CheckTypeAllocation(PyTypeObject *);
extern int _PySOAC_CheckPendingBases(PyObject *);
extern int _PySOAC_CheckPendingTypeWrite(PyTypeObject *);
extern void _PySOAC_FailPendingType(PyTypeObject *);
extern PyObject *_PySOAC_GetTypeConstructionOwner(PyObject *);
extern int _PySOAC_LinkPendingTypeHandle(PyObject *, PyObject *, PyObject *);
/* Operation-owned actual type supports the returned scalar destination.
 * This is selected only for an open pending dataclass adapter; it does not
 * authenticate the dictionary's mutating guard or grant a permanent policy. */
extern int _PySOAC_PendingTypeMemberTarget(
    PyObject *actual_type, PyObject *expected_owner, PyObject *expected_root,
    PyObject *actual_dict, PyObject *incoming_exact_name,
    unsigned int role, uint64_t **birth_out);
/* The actual dictionary kernel supplies its own live policy fields and owns
 * the busy guard. This pure identity test deliberately does not read mutating. */
extern int _PySOAC_MatchesPendingMemberPolicy(
    PyObject *policy_owner, PyDict_SoacPolicyCallback validate,
    PyObject *actual_dict, PyObject *expected_class_owner);
/* Pending-only callback-free stored-key scan plus the recorded publication
 * birth. Returns 1 pending, 0 legacy, -1 refusal; no borrowed hook is retained. */
extern int _PySOAC_PendingTypeCopiedHook(
    PyObject *original, PyObject *expected_root, PyObject *exact_name,
    PyObject *function, uint64_t *birth_out);
extern int _PySOAC_BeginPendingTypeAdapter(PyObject *, PyObject **);
extern void _PySOAC_EndPendingTypeAdapter(PyObject *, int);
extern int _PySOAC_ProtectedName(PyTypeObject *, PyObject *);
extern int _PySOAC_CheckInstanceWrite(PyObject *, PyObject *, PyObject *);
extern int _PySOAC_CheckClassWrite(PyTypeObject *, PyObject *, PyObject *);
extern int _PySOAC_CheckTypeBases(PyTypeObject *, PyObject *);
extern int _PySOAC_CheckTypeMro(PyTypeObject *, PyObject *, int);
extern int _PySOAC_CheckClassAssignment(PyTypeObject *, PyTypeObject *);
extern int _PySOAC_CheckDictionaryReplacement(PyObject *);
extern int _PySOAC_ReadyTypeInheritance(PyTypeObject *);
extern int _PySOAC_TypeContractTraverse(PyTypeObject *, visitproc, void *);
extern void _PySOAC_TypeContractBeginTeardown(PyTypeObject *);
extern void _PySOAC_TypeContractClear(PyTypeObject *);
extern void _PySOAC_TypeContractDealloc(PyTypeObject *);
extern PyObject *_PySOAC_NewInstanceDictionary(PyObject *);
extern int _PySOAC_UsesInstanceDictionaryPolicy(PyTypeObject *);
/* Actual generated _testinternalcapi executor consumes these guard/check
 * helpers; export only these private implementation symbols. */
PyAPI_FUNC(int) _PySOAC_HasOrdinaryInstanceWrites(PyTypeObject *);
PyAPI_FUNC(int) _PySOAC_CheckInlineInstanceWrite(PyObject *, PyObject *, PyObject *);
extern int _PySOAC_PrepareInstanceDictPolicy(
    PyObject *, PyObject *, const PySoacInstanceDictPolicy *, PySoacInstanceDictPolicy *);
/* Also consumed by the generated _testinternalcapi executor. */
PyAPI_FUNC(int) _PySOAC_UsesObjectSlotPolicy(PyTypeObject *);
extern int _PySOAC_CheckObjectSlotAccess(PyObject *, const PyMemberDef *);
extern int _PySOAC_CheckObjectSlotWrite(PyObject *, Py_ssize_t, PyObject *);
extern int _PySOAC_PublishAnnotationCache(PyTypeObject *, PyObject *, PyObject *);
extern int _PySOAC_MatchesClassNamespacePolicy(
    PyObject *policy_owner, PyDict_SoacPolicyCallback validate,
    PyObject *dict, PyObject *expected_owner);

/* Only the actual builtin __build_class__ may consume with these C-supported
 * operands. No public mode or second type-construction implementation. */
extern PyObject *_PySOAC_TypeFromInterpreterConstructionHandle(
    PyObject *handle, PyObject *namespace_function, PyObject *metaclass,
    PyObject *name, PyObject *bases, PyObject *namespace_dict, PyObject *keywords);

#endif
