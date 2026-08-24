#include "parts.h"
#include "util.h"

/* Deliberately test-only native ownership.  There is no Python-accessible
   policy replacement or arbitrary-dictionary terminal cleanup method. */
typedef struct {
    PyObject_HEAD
    PyObject *dict;
    PyObject *schema;
    PyObject *finals;
    PyObject *callback;
    PyObject *keepalive;
    unsigned int flags;
    int terminal;
} SoacTestOwner;

static int
soac_test_owner_traverse(PyObject *op, visitproc visit, void *arg)
{
    SoacTestOwner *owner = (SoacTestOwner *)op;
    Py_VISIT(Py_TYPE(op));
    Py_VISIT(owner->dict);
    Py_VISIT(owner->schema);
    Py_VISIT(owner->finals);
    Py_VISIT(owner->callback);
    Py_VISIT(owner->keepalive);
    return 0;
}

static int
soac_test_owner_clear(PyObject *op)
{
    SoacTestOwner *owner = (SoacTestOwner *)op;
    Py_CLEAR(owner->dict);
    Py_CLEAR(owner->schema);
    Py_CLEAR(owner->finals);
    Py_CLEAR(owner->callback);
    Py_CLEAR(owner->keepalive);
    return 0;
}

static void
soac_test_owner_dealloc(PyObject *op)
{
    PyTypeObject *type = Py_TYPE(op);
    PyObject_GC_UnTrack(op);
    soac_test_owner_clear(op);
    type->tp_free(op);
    Py_DECREF(type);
}

static PyObject *
soac_test_owner_terminal(PyObject *op, void *context)
{
    return PyBool_FromLong(((SoacTestOwner *)op)->terminal);
}

static PyObject *
soac_test_owner_clear_for_test(PyObject *op, PyObject *Py_UNUSED(args))
{
    SoacTestOwner *owner = (SoacTestOwner *)op;
    assert(owner->dict != NULL);
    /* Exercise the actual unreachable-GC slot, only for this fixture's
       permanently bound dictionary.  Not an unseal or a general C wrapper. */
    if (Py_TYPE(owner->dict)->tp_clear(owner->dict) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyGetSetDef soac_test_owner_getset[] = {
    {"terminal", soac_test_owner_terminal, NULL, NULL, NULL},
    {NULL}
};

static PyMethodDef soac_test_owner_methods[] = {
    {"clear_for_test", soac_test_owner_clear_for_test, METH_NOARGS, NULL},
    {NULL}
};

static PyType_Slot soac_test_owner_slots[] = {
    {Py_tp_traverse, soac_test_owner_traverse},
    {Py_tp_clear, soac_test_owner_clear},
    {Py_tp_dealloc, soac_test_owner_dealloc},
    {Py_tp_getset, soac_test_owner_getset},
    {Py_tp_methods, soac_test_owner_methods},
    {0, NULL}
};

static PyType_Spec soac_test_owner_spec = {
    .name = "_testcapi._SoacDictOwner",
    .basicsize = sizeof(SoacTestOwner),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
             Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots = soac_test_owner_slots,
};

static int
soac_test_validate_value(SoacTestOwner *owner, PyObject *key, PyObject *value)
{
    PyObject *expected = PyUnicode_CheckExact(key)
        ? PyDict_GetItemWithError(owner->schema, key) : NULL;
    if (expected == NULL && !PyErr_Occurred() &&
        (owner->flags & (PyDict_SOAC_ALLOW_NONSTRING_KEYS | PyDict_SOAC_READ_ONLY))) {
        expected = Py_None;
    }
    if (expected == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "undeclared SOAC test field");
        }
        return -1;
    }
    if (expected != Py_None && Py_TYPE(value) != (PyTypeObject *)expected) {
        PyErr_SetString(PyExc_TypeError, "incorrect SOAC test field value");
        return -1;
    }
    return 0;
}

static int
soac_test_validate(PyObject *op, PyObject *dict, PyObject *key,
                   PyObject *value, int operation, PyObject *provenance)
{
    SoacTestOwner *owner = (SoacTestOwner *)op;
    if (operation == PyDict_SOAC_CACHE_INSERT || operation == PyDict_SOAC_CACHE_REPLACE) {
        PyErr_SetString(PyExc_TypeError, "SOAC test owner has no cache provider");
        return -1;
    }
    int attribute = operation == PyDict_SOAC_ATTRIBUTE_SET ||
                    operation == PyDict_SOAC_ATTRIBUTE_SET_EXISTING;
    assert(attribute ? (provenance != NULL && PyUnicode_Check(provenance))
                     : provenance == NULL);
    if (operation == PyDict_SOAC_VALIDATE_INITIAL) {
        assert(!PyDict_MatchesSoacPolicy(dict, op, soac_test_validate, owner->flags));
    }
    if (operation == PyDict_SOAC_TERMINAL_TEARDOWN) {
        owner->terminal = 1;
        return 0;
    }
    assert(!owner->terminal);
    if (operation == PyDict_SOAC_CLEAR) {
        Py_ssize_t pos = 0;
        PyObject *stored_key;
        while (PyDict_Next(dict, &pos, &stored_key, NULL)) {
            if (!PyUnicode_CheckExact(stored_key)) {
                continue;
            }
            int final = PySet_Contains(owner->finals, stored_key);
            if (final < 0) {
                return -1;
            }
            if (final) {
                PyErr_SetString(PyExc_TypeError, "immutable SOAC test binding");
                return -1;
            }
        }
    }
    if (operation == PyDict_SOAC_SET || operation == PyDict_SOAC_SET_EXISTING ||
        operation == PyDict_SOAC_VALIDATE_INITIAL || attribute) {
        if (soac_test_validate_value(owner, key, value) < 0) {
            return -1;
        }
        if (attribute) {
            PyObject *name = PyUnicode_FromObject(provenance);
            if (name == NULL) {
                return -1;
            }
            int result = soac_test_validate_value(owner, name, value);
            Py_DECREF(name);
            if (result < 0) {
                return -1;
            }
        }
    }
    if (key != NULL && PyUnicode_CheckExact(key) && operation != PyDict_SOAC_VALIDATE_INITIAL) {
        int final = PySet_Contains(owner->finals, key);
        if (final < 0) {
            return -1;
        }
        if (final && (operation == PyDict_SOAC_SET_EXISTING ||
                      operation == PyDict_SOAC_ATTRIBUTE_SET_EXISTING ||
                      operation == PyDict_SOAC_DELETE)) {
            PyErr_SetString(PyExc_TypeError, "immutable SOAC test binding");
            return -1;
        }
    }
    if (operation != PyDict_SOAC_VALIDATE_INITIAL && owner->callback != Py_None) {
        PyObject *result = PyObject_CallFunction(
            owner->callback, "OOOi", dict, key == NULL ? Py_None : key,
            value == NULL ? Py_None : value, operation);
        if (result == NULL) {
            return -1;
        }
        Py_DECREF(result);
    }
    return 0;
}

static PyObject *
dict_set_soac_policy(PyObject *self, PyObject *args)
{
    PyObject *dict, *schema, *finals = NULL;
    PyObject *callback = Py_None, *keepalive = Py_None;
    unsigned int flags = 0;
    if (!PyArg_ParseTuple(args, "OO|OOOI", &dict, &schema, &finals,
                          &callback, &keepalive, &flags)) {
        return NULL;
    }
    if (!PyDict_CheckExact(schema)) {
        PyErr_SetString(PyExc_TypeError, "expected an exact schema dictionary");
        return NULL;
    }
    Py_ssize_t pos = 0;
    PyObject *key, *expected;
    while (PyDict_Next(schema, &pos, &key, &expected)) {
        if (!PyUnicode_CheckExact(key) ||
            (expected != Py_None && !PyType_Check(expected))) {
            PyErr_SetString(PyExc_TypeError, "invalid SOAC test schema");
            return NULL;
        }
    }
    PyObject *type = PyType_FromSpec(&soac_test_owner_spec);
    if (type == NULL) {
        return NULL;
    }
    SoacTestOwner *owner = (SoacTestOwner *)((PyTypeObject *)type)->tp_alloc(
        (PyTypeObject *)type, 0);
    Py_DECREF(type);
    if (owner == NULL) {
        return NULL;
    }
    owner->dict = Py_NewRef(dict);
    owner->schema = PyDict_Copy(schema);
    owner->finals = PyFrozenSet_New(finals);
    owner->callback = Py_NewRef(callback);
    owner->keepalive = Py_NewRef(keepalive);
    owner->flags = flags;
    if (owner->schema == NULL || owner->finals == NULL ||
        PyDict_SetSoacPolicy(dict, (PyObject *)owner, soac_test_validate, flags) < 0) {
        Py_DECREF(owner);
        return NULL;
    }
    return (PyObject *)owner;
}

static PyObject *
dict_seal_soac_namespace(PyObject *self, PyObject *dict)
{
    RETURN_INT(PyDict_SealSoacNamespace(dict));
}

static PyObject *
dict_has_soac_policy(PyObject *self, PyObject *dict)
{
    return PyBool_FromLong(PyDict_HasSoacPolicy(dict));
}

static PyObject *
dict_matches_soac_policy(PyObject *self, PyObject *args)
{
    PyObject *dict, *owner;
    unsigned int flags = 0;
    if (!PyArg_ParseTuple(args, "OO|I", &dict, &owner, &flags)) {
        return NULL;
    }
    return PyBool_FromLong(PyDict_MatchesSoacPolicy(dict, owner, soac_test_validate, flags));
}

static PyObject *
dict_containsstring(PyObject *self, PyObject *args)
{
    PyObject *obj;
    const char *key;
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "Oz#", &obj, &key, &size)) {
        return NULL;
    }
    NULLABLE(obj);
    RETURN_INT(PyDict_ContainsString(obj, key));
}

static PyObject *
dict_getitemref(PyObject *self, PyObject *args)
{
    PyObject *obj, *attr_name, *value = UNINITIALIZED_PTR;
    if (!PyArg_ParseTuple(args, "OO", &obj, &attr_name)) {
        return NULL;
    }
    NULLABLE(obj);
    NULLABLE(attr_name);

    switch (PyDict_GetItemRef(obj, attr_name, &value)) {
        case -1:
            assert(value == NULL);
            return NULL;
        case 0:
            assert(value == NULL);
            return Py_NewRef(PyExc_KeyError);
        case 1:
            return value;
        default:
            Py_FatalError("PyMapping_GetItemRef() returned invalid code");
            Py_UNREACHABLE();
    }
}

static PyObject *
dict_getitemstringref(PyObject *self, PyObject *args)
{
    PyObject *obj, *value = UNINITIALIZED_PTR;
    const char *attr_name;
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "Oz#", &obj, &attr_name, &size)) {
        return NULL;
    }
    NULLABLE(obj);

    switch (PyDict_GetItemStringRef(obj, attr_name, &value)) {
        case -1:
            assert(value == NULL);
            return NULL;
        case 0:
            assert(value == NULL);
            return Py_NewRef(PyExc_KeyError);
        case 1:
            return value;
        default:
            Py_FatalError("PyDict_GetItemStringRef() returned invalid code");
            Py_UNREACHABLE();
    }
}

static PyObject *
dict_setdefault(PyObject *self, PyObject *args)
{
    PyObject *mapping, *key, *defaultobj;
    if (!PyArg_ParseTuple(args, "OOO", &mapping, &key, &defaultobj)) {
        return NULL;
    }
    NULLABLE(mapping);
    NULLABLE(key);
    NULLABLE(defaultobj);
    return PyDict_SetDefault(mapping, key, defaultobj);
}

static PyObject *
dict_setdefaultref(PyObject *self, PyObject *args)
{
    PyObject *obj, *key, *default_value, *result = UNINITIALIZED_PTR;
    if (!PyArg_ParseTuple(args, "OOO", &obj, &key, &default_value)) {
        return NULL;
    }
    NULLABLE(obj);
    NULLABLE(key);
    NULLABLE(default_value);
    switch (PyDict_SetDefaultRef(obj, key, default_value, &result)) {
        case -1:
            assert(result == NULL);
            return NULL;
        case 0:
            assert(result == default_value);
            return result;
        case 1:
            return result;
        default:
            Py_FatalError("PyDict_SetDefaultRef() returned invalid code");
            Py_UNREACHABLE();
    }
}

static PyObject *
dict_pop(PyObject *self, PyObject *args)
{
    // Test PyDict_Pop(dict, key, &value)
    PyObject *dict, *key;
    if (!PyArg_ParseTuple(args, "OO", &dict, &key)) {
        return NULL;
    }
    NULLABLE(dict);
    NULLABLE(key);
    PyObject *result = UNINITIALIZED_PTR;
    int res = PyDict_Pop(dict, key,  &result);
    if (res < 0) {
        assert(result == NULL);
        return NULL;
    }
    if (res == 0) {
        assert(result == NULL);
        result = Py_NewRef(Py_None);
    }
    else {
        assert(result != NULL);
    }
    return Py_BuildValue("iN", res, result);
}

static PyObject *
dict_pop_null(PyObject *self, PyObject *args)
{
    // Test PyDict_Pop(dict, key, NULL)
    PyObject *dict, *key;
    if (!PyArg_ParseTuple(args, "OO", &dict, &key)) {
        return NULL;
    }
    NULLABLE(dict);
    NULLABLE(key);
    RETURN_INT(PyDict_Pop(dict, key,  NULL));
}

static PyObject *
dict_popstring(PyObject *self, PyObject *args)
{
    PyObject *dict;
    const char *key;
    Py_ssize_t key_size;
    if (!PyArg_ParseTuple(args, "Oz#", &dict, &key, &key_size)) {
        return NULL;
    }
    NULLABLE(dict);
    PyObject *result = UNINITIALIZED_PTR;
    int res = PyDict_PopString(dict, key,  &result);
    if (res < 0) {
        assert(result == NULL);
        return NULL;
    }
    if (res == 0) {
        assert(result == NULL);
        result = Py_NewRef(Py_None);
    }
    else {
        assert(result != NULL);
    }
    return Py_BuildValue("iN", res, result);
}

static PyObject *
dict_popstring_null(PyObject *self, PyObject *args)
{
    PyObject *dict;
    const char *key;
    Py_ssize_t key_size;
    if (!PyArg_ParseTuple(args, "Oz#", &dict, &key, &key_size)) {
        return NULL;
    }
    NULLABLE(dict);
    RETURN_INT(PyDict_PopString(dict, key,  NULL));
}


static int
test_dict_inner(PyObject *self, int count)
{
    Py_ssize_t pos = 0, iterations = 0;
    int i;
    PyObject *dict = PyDict_New();
    PyObject *v, *k;

    if (dict == NULL)
        return -1;

    for (i = 0; i < count; i++) {
        v = PyLong_FromLong(i);
        if (v == NULL) {
            goto error;
        }
        if (PyDict_SetItem(dict, v, v) < 0) {
            Py_DECREF(v);
            goto error;
        }
        Py_DECREF(v);
    }

    k = v = UNINITIALIZED_PTR;
    while (PyDict_Next(dict, &pos, &k, &v)) {
        PyObject *o;
        iterations++;

        assert(k != UNINITIALIZED_PTR);
        assert(v != UNINITIALIZED_PTR);
        i = PyLong_AS_LONG(v) + 1;
        o = PyLong_FromLong(i);
        if (o == NULL) {
            goto error;
        }
        if (PyDict_SetItem(dict, k, o) < 0) {
            Py_DECREF(o);
            goto error;
        }
        Py_DECREF(o);
        k = v = UNINITIALIZED_PTR;
    }
    assert(k == UNINITIALIZED_PTR);
    assert(v == UNINITIALIZED_PTR);

    Py_DECREF(dict);

    if (iterations != count) {
        PyErr_SetString(
            PyExc_AssertionError,
            "test_dict_iteration: dict iteration went wrong ");
        return -1;
    } else {
        return 0;
    }
error:
    Py_DECREF(dict);
    return -1;
}


static PyObject*
test_dict_iteration(PyObject* self, PyObject *Py_UNUSED(ignored))
{
    int i;

    for (i = 0; i < 200; i++) {
        if (test_dict_inner(self, i) < 0) {
            return NULL;
        }
    }

    Py_RETURN_NONE;
}


static PyMethodDef test_methods[] = {
    {"dict_set_soac_policy", dict_set_soac_policy, METH_VARARGS},
    {"dict_seal_soac_namespace", dict_seal_soac_namespace, METH_O},
    {"dict_has_soac_policy", dict_has_soac_policy, METH_O},
    {"dict_matches_soac_policy", dict_matches_soac_policy, METH_VARARGS},
    {"dict_containsstring", dict_containsstring, METH_VARARGS},
    {"dict_getitemref", dict_getitemref, METH_VARARGS},
    {"dict_getitemstringref", dict_getitemstringref, METH_VARARGS},
    {"dict_setdefault", dict_setdefault, METH_VARARGS},
    {"dict_setdefaultref", dict_setdefaultref, METH_VARARGS},
    {"dict_pop", dict_pop, METH_VARARGS},
    {"dict_pop_null", dict_pop_null, METH_VARARGS},
    {"dict_popstring", dict_popstring, METH_VARARGS},
    {"dict_popstring_null", dict_popstring_null, METH_VARARGS},
    {"test_dict_iteration",     test_dict_iteration,             METH_NOARGS},
    {NULL},
};

int
_PyTestCapi_Init_Dict(PyObject *m)
{
    if (PyModule_AddFunctions(m, test_methods) < 0) {
        return -1;
    }

    return 0;
}
