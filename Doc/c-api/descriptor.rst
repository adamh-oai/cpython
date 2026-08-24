.. highlight:: c

.. _descriptor-objects:

Descriptor Objects
------------------

"Descriptors" are objects that describe some attribute of an object. They are
found in the dictionary of type objects.

SOAC native descriptor births
=============================

The following interpreter-private C APIs support explicit compiler-owned
``staticmethod``, ``classmethod``, and getter-only ``property`` construction.
They are not a Stable ABI, a Python-level admission protocol, or a source-code
authentication mechanism. Ordinary public constructors are unchanged and do
not create a birth record. Property accessor copies and descriptor subclasses
also receive no birth authority.

.. c:function:: PyObject *PySoac_NewBuiltinDescriptor(PyObject *factory, PyObject *function, PyObject *expected_function_owner, PyObject *verified_code, PyObject *namespace_witness)

   Return a new exact builtin descriptor, initially mutable. ``factory`` must
   be the native ``staticmethod``, ``classmethod``, or ``property`` type itself.
   ``function`` must be an exact function whose attached native owner and
   current code are the two supplied identities. The native caller must first
   authenticate the original MakeFunction weak witness, the source owner/code,
   and the current namespace execution. Looking up the owner of an arbitrary
   replacement function is not a substitute for that original witness.

   ``namespace_witness`` is a caller-owned, GC-visible identity payload with no
   outgoing Python graph edges. In particular it must not retain a class,
   prepared namespace, globals, function, or namespace handle with such edges.
   The descriptor's opaque birth record owns this payload and a callback-free
   weak code reference. It owns no additional callable, function owner, class,
   or globals references beyond the descriptor's normal component reference.

   The record is attached before the descriptor is GC tracked. Construction
   then uses ordinary builtin metadata initialization and rechecks the actual
   component, function owner, and code after allocation/callback boundaries.
   A partially constructed record cannot match or be adopted. An invalidated
   record cannot become pending again. Failure preserves the native exception
   and releases the unpublished construction references normally.

.. c:function:: PyObject *PySoac_GetDescriptorBirthOwner(PyObject *descriptor)

   Return the borrowed namespace witness for a current completed birth. Return
   ``NULL`` without an exception for an ordinary, copied, reinitialized, or
   otherwise mismatched descriptor. A recognized terminal function owner keeps
   its runtime-unavailable exception. This getter does not authenticate the
   caller's namespace and grants no source or optimization capability.

.. c:function:: int PySoac_MatchesDescriptorBirth(PyObject *descriptor, PyObject *namespace_witness, PyObject *function, PyObject *expected_function_owner, PyObject *verified_code)

   Return ``1`` only when all actual native identities still match the birth,
   ``0`` for a miss, or ``-1`` for invalid NULL operands or a recognized terminal
   owner. Matching performs no allocation, Python attribute lookup, equality,
   weakref callback, or code evaluation. Stored raw coordinates are only
   compared against the independently live actual descriptor component; they
   are never dereferenced as objects.

.. c:function:: int PySoac_AdoptBuiltinDescriptor(PyObject *descriptor, PyObject *namespace_witness, PyObject *function, PyObject *expected_function_owner, PyObject *verified_code)

   Validate the same identities and permanently seal the descriptor component
   slots, returning ``0`` or ``-1`` with an exception. No callback or allocation
   separates successful validation from sealing. Repeating adoption with the
   same identities succeeds. A mismatched attempt never revokes an existing
   seal. The caller remains responsible for separately adopting the actual
   function metadata and required checked-entry boundaries before class Ready;
   this operation does not select a vectorcall or grant a JIT capability.

Before adoption, valid ``__init__`` calls invalidate birth provenance before
releasing any displaced component, even if the requested component is identical
or later restored. The descriptor keeps ordinary mutable behavior. Argument
errors before initialization do not invalidate it. After adoption,
reinitialization is rejected by the permanent component seal; non-dispatch
metadata such as documentation remains ordinary. GC clear/deallocation first
tombstones the nonowning coordinates, then preserves the ordinary component
release order and clears the inert record payload. Retaining an opaque record
through GC introspection cannot extend the descriptor or callable lifetime.

.. XXX document these!

.. c:function:: PyObject* PyDescr_NewGetSet(PyTypeObject *type, struct PyGetSetDef *getset)


.. c:function:: PyObject* PyDescr_NewMember(PyTypeObject *type, struct PyMemberDef *meth)


.. c:var:: PyTypeObject PyMemberDescr_Type

   The type object for member descriptor objects created from
   :c:type:`PyMemberDef` structures. These descriptors expose fields of a
   C struct as attributes on a type, and correspond
   to :class:`types.MemberDescriptorType` objects in Python.



.. c:var:: PyTypeObject PyGetSetDescr_Type

   The type object for get/set descriptor objects created from
   :c:type:`PyGetSetDef` structures. These descriptors implement attributes
   whose value is computed by C getter and setter functions, and are used
   for many built-in type attributes.


.. c:function:: PyObject* PyDescr_NewMethod(PyTypeObject *type, struct PyMethodDef *meth)


.. c:var:: PyTypeObject PyMethodDescr_Type

   The type object for method descriptor objects created from
   :c:type:`PyMethodDef` structures. These descriptors expose C functions as
   methods on a type, and correspond to :class:`types.MemberDescriptorType`
   objects in Python.


.. c:function:: PyObject* PyDescr_NewWrapper(PyTypeObject *type, struct wrapperbase *wrapper, void *wrapped)


.. c:var:: PyTypeObject PyWrapperDescr_Type

   The type object for wrapper descriptor objects created by
   :c:func:`PyDescr_NewWrapper` and :c:func:`PyWrapper_New`. Wrapper
   descriptors are used internally to expose special methods implemented
   via wrapper structures, and appear in Python as
   :class:`types.WrapperDescriptorType` objects.


.. c:function:: PyObject* PyDescr_NewClassMethod(PyTypeObject *type, PyMethodDef *method)


.. c:function:: int PyDescr_IsData(PyObject *descr)

   Return non-zero if the descriptor object *descr* describes a data attribute, or
   ``0`` if it describes a method.  *descr* must be a descriptor object; there is
   no error checking.


.. c:function:: PyObject* PyWrapper_New(PyObject *, PyObject *)


.. c:macro:: PyDescr_COMMON

   This is a :term:`soft deprecated` macro including the common fields for a
   descriptor object.

   This was included in Python's C API by mistake; do not use it in extensions.
   For creating custom descriptor objects, create a class implementing the
   descriptor protocol (:c:member:`~PyTypeObject.tp_descr_get` and
   :c:member:`~PyTypeObject.tp_descr_set`).


Built-in descriptors
^^^^^^^^^^^^^^^^^^^^

.. c:var:: PyTypeObject PyProperty_Type

   The type object for property objects. This is the same object as
   :class:`property` in the Python layer.


.. c:var:: PyTypeObject PySuper_Type

   The type object for super objects. This is the same object as
   :class:`super` in the Python layer.


.. c:var:: PyTypeObject PyClassMethod_Type

   The type of class method objects. This is the same object as
   :class:`classmethod` in the Python layer.


.. c:var:: PyTypeObject PyClassMethodDescr_Type

   The type object for C-level class method descriptor objects.
   This is the type of the descriptors created for :func:`classmethod` defined in
   C extension types, and is the same object as :class:`classmethod`
   in Python.


.. c:function:: PyObject *PyClassMethod_New(PyObject *callable)

   Create a new :class:`classmethod` object wrapping *callable*.
   *callable* must be a callable object and must not be ``NULL``.

   On success, this function returns a :term:`strong reference` to a new class
   method descriptor. On failure, this function returns ``NULL`` with an
   exception set.


.. c:var:: PyTypeObject PyStaticMethod_Type

   The type of static method objects. This is the same object as
   :class:`staticmethod` in the Python layer.


.. c:function:: PyObject *PyStaticMethod_New(PyObject *callable)

   Create a new :class:`staticmethod` object wrapping *callable*.
   *callable* must be a callable object and must not be ``NULL``.

   On success, this function returns a :term:`strong reference` to a new static
   method descriptor. On failure, this function returns ``NULL`` with an
   exception set.
