#ifndef Py_INTERNAL_SOAC_TYPE_H
#define Py_INTERNAL_SOAC_TYPE_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* These operations consult native type-owned state, never Python attributes. */
extern int _PySOAC_ProtectedName(PyTypeObject *, PyObject *);
extern int _PySOAC_CheckInstanceWrite(PyObject *, PyObject *, PyObject *);
extern int _PySOAC_CheckClassWrite(PyTypeObject *, PyObject *, PyObject *);
extern int _PySOAC_CheckTypeBases(PyTypeObject *, PyObject *);
extern int _PySOAC_CheckClassAssignment(PyTypeObject *, PyTypeObject *);
extern int _PySOAC_CheckDictionaryReplacement(PyObject *);
extern int _PySOAC_ReadyTypeInheritance(PyTypeObject *);
extern int _PySOAC_TypeContractTraverse(PyTypeObject *, visitproc, void *);
extern void _PySOAC_TypeContractBeginTeardown(PyTypeObject *);
extern void _PySOAC_TypeContractClear(PyTypeObject *);
extern void _PySOAC_TypeContractDealloc(PyTypeObject *);
extern PyObject *_PySOAC_NewInstanceDictionary(PyObject *);
extern int _PySOAC_UsesInstanceDictionaryPolicy(PyTypeObject *);
extern int _PySOAC_PublishAnnotationCache(PyTypeObject *, PyObject *, PyObject *);
extern int _PySOAC_MatchesClassNamespacePolicy(
    PyObject *policy_owner, PyDict_SoacPolicyCallback validate,
    PyObject *dict, PyObject *expected_owner);

#endif
