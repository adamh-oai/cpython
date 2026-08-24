#ifndef Py_INTERNAL_SOAC_DESCRIPTOR_H
#define Py_INTERNAL_SOAC_DESCRIPTOR_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* The opaque GC record never owns the descriptor or its callable. Bind and
 * invalidate only change inert coordinates; neither can allocate or decref. */
extern void _PySoac_DescriptorBirth_Bind(PyObject *, PyObject *);
extern void _PySoac_DescriptorBirth_Invalidate(PyObject *);
/* Called after ordinary component cleanup. Detaches and clears the record,
 * including when an observer has retained the otherwise opaque record. */
extern void _PySoac_DescriptorBirth_Clear(PyObject **);
/* Exact property construction, with a borrowed preallocated birth record. */
extern PyObject *_PySoac_NewProperty(PyObject *, PyObject *);

#ifdef __cplusplus
}
#endif
/* Exact original builtin constructor dispatch, callback-free observations.
 * These are not descriptor/source authority and expose no operand capability. */
extern int _PySOAC_TypeCallIsOriginal(void);
extern int _PySOAC_DescriptorFactoryIsOriginal(PyObject *factory);
extern int _PySOAC_PropertyFactoryIsOriginal(void);

#endif /* Py_INTERNAL_SOAC_DESCRIPTOR_H */
