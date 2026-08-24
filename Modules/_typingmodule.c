/* typing accelerator C extension: _typing module. */

#ifndef Py_BUILD_CORE
#define Py_BUILD_CORE
#endif

#include "Python.h"
#include "internal/pycore_code.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_typevarobject.h"
#include "internal/pycore_unionobject.h"  // _PyUnion_Type
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "clinic/_typingmodule.c.h"

/*[clinic input]
module _typing

[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=1db35baf1c72942b]*/

/* helper function to make typing.NewType.__call__ method faster */

/*[clinic input]
_typing._idfunc -> object

    x: object
    /

[clinic start generated code]*/

static PyObject *
_typing__idfunc(PyObject *module, PyObject *x)
/*[clinic end generated code: output=63c38be4a6ec5f2c input=49f17284b43de451]*/
{
    return Py_NewRef(x);
}

static PyObject *
soac_annotation_replay_code(PyObject *module, PyObject *args)
{
    PyObject *provider, *owner, *format;
    if (!PyArg_ParseTuple(args, "OOO:_soac_annotation_replay_code",
                          &provider, &owner, &format)) {
        return NULL;
    }
    if (!PyFunction_Check(provider)) {
        return PyObject_GetAttrString(provider, "__code__");
    }
    PyObject *native_owner = PyFunction_GetSoacStrictOwner(provider);
    if (native_owner == NULL && PyErr_Occurred()) {
        return NULL;
    }
    PyObject *code = PyFunction_GET_CODE(provider);
    if (native_owner == NULL &&
        (code == NULL || !PyCode_Check(code) ||
         (!(((PyCodeObject *)code)->co_flags & CO_FUTURE_STRICT) &&
          ((PyCodeObject *)code)->_co_soac_strict_source_id == 0))) {
        /* Keep stock callable/descriptor behavior outside strict ownership. */
        return PyObject_GetAttrString(provider, "__code__");
    }
    if (!PyLong_Check(format)) {
        PyErr_SetString(PyExc_ValueError, "annotation replay requires FORWARDREF or STRING");
        return NULL;
    }
    long requested = PyLong_AsLong(format);
    if (requested == -1 && PyErr_Occurred()) {
        return NULL;
    }
    if (requested != 3 && requested != 4) {
        PyErr_SetString(PyExc_ValueError, "annotation replay requires FORWARDREF or STRING");
        return NULL;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    PySoacAnnotationReplayResolver resolver = interp->soac.annotation_replay_resolver;
    if (resolver == NULL || interp->soac.annotation_replay_closed) {
        PyObject *exception = PySoac_GetStrictRuntimeUnavailableError();
        if (exception != NULL) {
            PyErr_SetString(exception, "strict annotation replay runtime is unavailable");
        }
        return NULL;
    }
    PyObject *result = resolver(provider, owner, (int)requested);
    if (result == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "annotation replay resolver failed without an exception");
        }
        return NULL;
    }
    if (_PyCode_CheckSoacAnnotationReplay(result) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}


static PyMethodDef typing_methods[] = {
    _TYPING__IDFUNC_METHODDEF
    {"_soac_annotation_replay_code", soac_annotation_replay_code, METH_VARARGS,
     "Return ordinary replay code through the interpreter-owned native resolver."},
    {NULL, NULL, 0, NULL}
};

PyDoc_STRVAR(typing_doc,
"Primitives and accelerators for the typing module.\n");

static int
_typing_exec(PyObject *m)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();

#define EXPORT_TYPE(name, typename) \
    if (PyModule_AddObjectRef(m, name, \
                              (PyObject *)interp->cached_objects.typename) < 0) { \
        return -1; \
    }

    EXPORT_TYPE("TypeVar", typevar_type);
    EXPORT_TYPE("TypeVarTuple", typevartuple_type);
    EXPORT_TYPE("ParamSpec", paramspec_type);
    EXPORT_TYPE("ParamSpecArgs", paramspecargs_type);
    EXPORT_TYPE("ParamSpecKwargs", paramspeckwargs_type);
    EXPORT_TYPE("Generic", generic_type);
#undef EXPORT_TYPE
    if (PyModule_AddObjectRef(m, "TypeAliasType", (PyObject *)&_PyTypeAlias_Type) < 0) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "Union", (PyObject *)&_PyUnion_Type) < 0) {
        return -1;
    }
    if (PyModule_AddObjectRef(m, "NoDefault", (PyObject *)&_Py_NoDefaultStruct) < 0) {
        return -1;
    }
    return 0;
}

static struct PyModuleDef_Slot _typingmodule_slots[] = {
    {Py_mod_exec, _typing_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef typingmodule = {
        PyModuleDef_HEAD_INIT,
        "_typing",
        typing_doc,
        0,
        typing_methods,
        _typingmodule_slots,
        NULL,
        NULL,
        NULL
};

PyMODINIT_FUNC
PyInit__typing(void)
{
    return PyModuleDef_Init(&typingmodule);
}
