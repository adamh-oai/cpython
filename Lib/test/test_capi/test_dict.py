import unittest
import gc
import sys
import weakref
from collections import OrderedDict, UserDict
from types import MappingProxyType
from test import support
from test.support import import_helper
from test.support.script_helper import assert_python_failure, assert_python_ok


_testcapi = import_helper.import_module("_testcapi")
_testinternalcapi = import_helper.import_module("_testinternalcapi")
_testlimitedcapi = import_helper.import_module("_testlimitedcapi")


NULL = None
INVALID_UTF8 = b'\xff'

class DictSubclass(dict):
    def __getitem__(self, key):
        raise RuntimeError('do not get evil')
    def __setitem__(self, key, value):
        raise RuntimeError('do not set evil')
    def __delitem__(self, key):
        raise RuntimeError('do not del evil')

def gen():
    yield 'a'
    yield 'b'
    yield 'c'


@unittest.skipIf(support.Py_GIL_DISABLED, "SOAC policies require the GIL")
class SoacDictPolicyTests(unittest.TestCase):

    def tearDown(self):
        gc.collect()

    def protect(self, dictionary, schema, finals=(), callback=None, keepalive=None):
        return _testcapi.dict_set_soac_policy(
            dictionary, schema, finals, callback, keepalive)

    def test_single_item_python_and_c_mutators(self):
        d = {"x": 1}
        self.protect(d, {"x": int, "y": int})
        for setter in (dict.__setitem__, _testlimitedcapi.dict_setitem):
            with self.subTest(setter=setter):
                setter(d, "x", 2)
                with self.assertRaises(TypeError):
                    setter(d, "x", "bad")
                with self.assertRaises(TypeError):
                    setter(d, "undeclared", 3)
                self.assertEqual(d, {"x": 2})
        self.assertEqual(d.setdefault("x", "unused invalid default"), 2)
        self.assertEqual(_testcapi.dict_setdefaultref(d, "y", 3), 3)
        with self.assertRaises(TypeError):
            _testcapi.dict_setdefault(d, "undeclared", 1)
        self.assertEqual(_testcapi.dict_pop(d, "y"), (1, 3))
        self.assertEqual(d.popitem(), ("x", 2))
        d["x"] = 4
        _testlimitedcapi.dict_delitem(d, "x")
        self.assertEqual(d, {})
        self.assertTrue(_testcapi.dict_has_soac_policy(d))
        with self.assertRaises(TypeError):
            d["x"] = "still checked"
        d["x"] = 5
        _testlimitedcapi.dict_clear(d)
        self.assertEqual(d, {})
        self.assertTrue(_testcapi.dict_has_soac_policy(d))

    def test_namespace_seal_is_permanent_but_mutable_bindings_work(self):
        d = {"fixed": 1, "mutable": 2}
        schema = {"fixed": int, "mutable": int, "late": int}
        self.protect(d, schema, ("fixed",))
        _testcapi.dict_seal_soac_namespace(d)
        _testcapi.dict_seal_soac_namespace(d)
        d["mutable"] = 3
        d["late"] = 4
        with self.assertRaises(TypeError):
            d["fixed"] = 1
        with self.assertRaises(TypeError):
            del d["fixed"]
        with self.assertRaises(TypeError):
            d.clear()
        with self.assertRaises(TypeError):
            self.protect(d, schema)
        d.update(d)
        self.assertEqual(d, {"fixed": 1, "mutable": 3, "late": 4})
        copied = d.copy()
        self.assertFalse(_testcapi.dict_has_soac_policy(copied))
        copied.clear()
        self.assertIn("fixed", d)

    def test_registration_validates_contents_and_refuses_shared_namespaces(self):
        d = {"x": "bad"}
        with self.assertRaises(TypeError):
            self.protect(d, {"x": int})
        self.assertFalse(_testcapi.dict_has_soac_policy(d))
        d["x"] = 1
        self.protect(d, {"x": int})
        for shared in (sys.__dict__, vars(__import__("builtins")), sys.modules):
            with self.subTest(shared_type=type(shared)):
                with self.assertRaises(TypeError):
                    self.protect(shared, {})
                self.assertFalse(_testcapi.dict_has_soac_policy(shared))
        with self.assertRaises(TypeError):
            self.protect(DictSubclass(), {})
        with self.assertRaises(TypeError):
            _testcapi.dict_seal_soac_namespace({})

    def test_non_exact_keys_are_rejected_before_hash_or_equality(self):
        called = []

        class Alias:
            def __hash__(self):
                called.append("hash")
                return hash("x")

            def __eq__(self, other):
                called.append("eq")
                return True

        class StringAlias(str):
            __hash__ = Alias.__hash__
            __eq__ = Alias.__eq__

        d = {"x": 1}
        self.protect(d, {"x": int})
        mutators = (
            lambda key: d.__setitem__(key, 2),
            lambda key: d.__delitem__(key),
            lambda key: d.setdefault(key, 2),
            lambda key: d.pop(key),
            lambda key: _testlimitedcapi.dict_setitem(d, key, 2),
            lambda key: _testlimitedcapi.dict_delitem(d, key),
            lambda key: _testcapi.dict_pop(d, key),
        )
        for key in (Alias(), StringAlias("x")):
            for mutate in mutators:
                with self.assertRaises(TypeError):
                    mutate(key)
        self.assertEqual(called, [])
        self.assertEqual(d, {"x": 1})

    def test_valid_exact_bulk_writes_and_prevalidation(self):
        d = {"x": 1}
        self.protect(d, {"x": int, "y": int, "z": int})
        d.update({"x": 2, "y": 3})
        d |= {"z": 4}
        d.update([["x", 5], ("y", 6)])
        _testlimitedcapi.dict_update(d, {"z": 7})
        _testlimitedcapi.dict_merge(d, {"x": 999}, 0)
        _testlimitedcapi.dict_merge(d, {"y": 8}, 1)
        _testlimitedcapi.dict_mergefromseq2(d, (("z", 9),), 1)
        self.assertEqual(d, {"x": 5, "y": 8, "z": 9})
        for update in (
            lambda: d.update({"x": 100, "y": "bad"}),
            lambda: d.__ior__({"x": 100, "y": "bad"}),
            lambda: dict.__init__(d, {"x": 100, "y": "bad"}),
            lambda: _testlimitedcapi.dict_merge(d, {"x": 100, "y": "bad"}, 1),
            lambda: _testlimitedcapi.dict_mergefromseq2(d, [["x", 100], ["y", "bad"]], 1),
        ):
            with self.assertRaises(TypeError):
                update()
            self.assertEqual(d, {"x": 5, "y": 8, "z": 9})
        d.clear()
        _testlimitedcapi.dict_mergefromseq2(d, [("x", 1), ("x", "skipped")], 0)
        self.assertEqual(d, {"x": 1})

    def test_arbitrary_bulk_protocols_are_rejected_without_execution(self):
        called = []

        class Mapping:
            def keys(self):
                called.append("keys")
                return ["x"]

            def __getitem__(self, key):
                called.append("getitem")
                return 2

            def __iter__(self):
                called.append("iter")
                return iter([("x", 2)])

        d = {"x": 1}
        self.protect(d, {"x": int})
        source = Mapping()
        for update in (
            lambda: d.update(source),
            lambda: d.__ior__(source),
            lambda: _testlimitedcapi.dict_merge(d, source, 1),
            lambda: _testlimitedcapi.dict_mergefromseq2(d, source, 1),
            lambda: d.update([source]),
        ):
            with self.assertRaises(TypeError):
                update()
        self.assertEqual(called, [])
        self.assertEqual(d, {"x": 1})

    def test_duplicate_bulk_writes_validate_projected_binding_presence(self):
        d = {"x": 1}
        self.protect(d, {"x": int, "late": int}, ("late",))
        with self.assertRaisesRegex(TypeError, "immutable"):
            d.update([("x", 2), ("late", 3), ("late", 4)])
        self.assertEqual(d, {"x": 1})
        d.update([("x", 2), ("x", 3)])
        self.assertEqual(d, {"x": 3})
        with self.assertRaises(TypeError):
            d.update([("x", "invalid intermediate"), ("x", 4)])
        self.assertEqual(d, {"x": 3})

    def test_validation_reentry_cannot_mutate_authoritative_dictionary(self):
        observed = []

        def validate(d, key, value, operation):
            with self.assertRaisesRegex(RuntimeError, "reentrant"):
                d["y"] = 9
            observed.append(dict(d))

        d = {"x": 1}
        self.protect(d, {"x": int, "y": int}, callback=validate)
        d["x"] = 2
        self.assertEqual(observed, [{"x": 1}])
        self.assertEqual(d, {"x": 2})

    def test_guard_covers_watcher_notification_through_commit(self):
        d = {"x": 1}
        states = []
        self.protect(d, {"x": None, "y": int})
        test = self

        class Value:
            def __str__(self):
                # The test watcher formats values, deliberately violating the
                # native no-Python watcher rule to probe the commit guard.
                with test.assertRaisesRegex(RuntimeError, "reentrant"):
                    d["y"] = 3
                states.append(dict(d))
                return "value"

        watcher = _testcapi.add_dict_watcher(0)
        try:
            _testcapi.watch_dict(watcher, d)
            value = Value()
            d["x"] = value
        finally:
            _testcapi.unwatch_dict(watcher, d)
            _testcapi.clear_dict_watcher(watcher)
        self.assertEqual(states, [{"x": 1}])
        self.assertIs(d["x"], value)
        self.assertNotIn("y", d)

    def test_native_conditional_delete_uses_policy(self):
        import _weakref

        class Token:
            pass

        token = Token()
        ref = weakref.ref(token)
        d = {"fixed": ref, "mutable": ref}
        self.protect(d, {"fixed": weakref.ReferenceType,
                         "mutable": weakref.ReferenceType}, ("fixed",))
        del token
        with self.assertRaises(TypeError):
            _weakref._remove_dead_weakref(d, "fixed")
        _weakref._remove_dead_weakref(d, "mutable")
        self.assertEqual(d, {"fixed": ref})

    def test_post_commit_finalizers_can_write_other_checked_fields(self):
        d = {}
        states = []
        self.protect(d, {"x": None, "y": int})

        class Finalizer:
            def __del__(self):
                states.append(dict(d))
                d["y"] = 7

        d["x"] = Finalizer()
        d["x"] = None
        self.assertEqual(states, [{"x": None}])
        self.assertEqual(d, {"x": None, "y": 7})
        d["x"] = Finalizer()
        d.clear()
        self.assertEqual(states[-1], {})
        self.assertEqual(d, {"y": 7})
        d["x"] = Finalizer()
        _testcapi.dict_pop_null(d, "x")
        self.assertEqual(states[-1], {"y": 7})
        d.clear()
        d["x"] = Finalizer()
        d.update({"x": None, "y": 8})
        self.assertEqual(states[-1], {"x": None})
        self.assertEqual(d, {"x": None, "y": 8})

    def test_explicit_clear_preserves_underlying_finalizer_order(self):
        class Item:
            pass

        def run(protected, split):
            events = []

            class Value:
                def __init__(self, name):
                    self.name = name

                def __del__(self):
                    events.append(self.name)

            if split:
                obj = Item()
                d = obj.__dict__
            else:
                d = {}
            for name in ("z", "x", "y"):
                d[name] = Value(name)
            del d["x"]
            d["x"] = Value("x_reinserted")
            events.clear()
            if protected:
                self.protect(d, dict.fromkeys(("x", "y", "z"), None))
            d.clear()
            return events

        for split in (False, True):
            with self.subTest(split=split):
                self.assertEqual(run(True, split), run(False, split))

    def test_indexed_writes_and_clear_preserve_policy_and_schema(self):
        d = _testinternalcapi.dict_new_indexed(("x", "y"))
        self.protect(d, {"x": int, "y": int})
        _testinternalcapi.dict_set_indexed_item(d, 0, 1)
        with self.assertRaises(TypeError):
            _testinternalcapi.dict_set_indexed_item(d, 0, "bad")
        d.setdefault("y", 2)
        d.clear()
        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(d))
        self.assertEqual(_testinternalcapi.dict_indexed_key_index(d, "y"), 1)
        _testinternalcapi.dict_set_indexed_item(d, 1, 3)
        self.assertEqual(d, {"y": 3})
        self.assertTrue(_testcapi.dict_has_soac_policy(d))

    def test_specialized_instance_stores_and_dictionary_replacement(self):
        class Item:
            pass

        def write(obj, value):
            obj.x = value

        for combined in (False, True):
            with self.subTest(combined=combined):
                obj = Item()
                obj.x = 1
                if combined:
                    for index in range(50):
                        setattr(obj, f"extra_{index}", index)
                d = obj.__dict__
                for index in range(1000):
                    write(obj, index)
                self.protect(d, dict.fromkeys(d, int))
                write(obj, 3)
                with self.assertRaises(TypeError):
                    write(obj, "bad")
                with self.assertRaises(TypeError):
                    obj.__dict__ = {}
                with self.assertRaises(TypeError):
                    del obj.__dict__
                self.assertIs(obj.__dict__, d)
                self.assertEqual(obj.x, 3)
                del obj.x
                self.assertNotIn("x", d)
                obj.x = 4
                self.assertEqual(d["x"], 4)

    def test_native_owner_edge_is_visible_to_gc(self):
        class Token:
            pass

        token = Token()
        ref = weakref.ref(token)
        d = {"x": 1}
        owner = self.protect(d, {"x": int}, keepalive=token)
        del token, owner
        gc.collect()
        self.assertIsNotNone(ref())
        del d
        gc.collect()
        self.assertIsNone(ref())

        obj = Token()
        obj.x = 1
        ref = weakref.ref(obj)
        owner = self.protect(obj.__dict__, {"x": int}, keepalive=obj)
        del obj, owner
        gc.collect()
        self.assertIsNone(ref())

    def test_terminal_gc_clear_never_unseals_or_reenables_writes(self):
        d = {"x": 1}
        owner = self.protect(d, {"x": int})
        self.assertTrue(_testcapi.dict_matches_soac_policy(d, owner))
        self.assertFalse(_testcapi.dict_matches_soac_policy(d, object()))
        _testcapi.dict_seal_soac_namespace(d)
        owner.clear_for_test()
        self.assertTrue(owner.terminal)
        self.assertTrue(_testcapi.dict_has_soac_policy(d))
        self.assertFalse(_testcapi.dict_matches_soac_policy(d, owner))
        self.assertEqual(d, {})
        with self.assertRaises(RuntimeError):
            d["x"] = 2
        with self.assertRaises(RuntimeError):
            d.update({"x": 2})
        with self.assertRaises(RuntimeError):
            _testcapi.dict_seal_soac_namespace(d)

    @support.requires_subprocess()
    def test_void_clear_has_explicit_fatal_boundary(self):
        _, _, stderr = assert_python_failure("-c", """
import _testcapi, _testlimitedcapi
d = {"x": 1}
owner = _testcapi.dict_set_soac_policy(d, {"x": int})
_testcapi.dict_seal_soac_namespace(d)
_testlimitedcapi.dict_clear(d)
""")
        self.assertIn(b"PyDict_Clear cannot report a SOAC policy violation", stderr)

    @support.requires_subprocess()
    def test_sealed_module_shutdown_runs_finalizers_without_fatal_clear(self):
        _, stdout, _ = assert_python_ok("-c", """
import _testcapi, sys, types
class Finalizer:
    def __del__(self, write=sys.stdout.write):
        write("soac finalizer ran\\n")
module = types.ModuleType("soac_policy_shutdown_fixture")
module.payload = Finalizer()
owner = _testcapi.dict_set_soac_policy(
    module.__dict__, dict.fromkeys(module.__dict__, None))
_testcapi.dict_seal_soac_namespace(module.__dict__)
sys.modules[module.__name__] = module
""")
        self.assertIn(b"soac finalizer ran", stdout)


class IndexedDictTests(unittest.TestCase):

    def new_dict(self, keys):
        return _testinternalcapi.dict_new_indexed(keys)

    def test_indexed_dict_direct_access_and_shared_keys(self):
        left = self.new_dict(("first", "second", "third"))
        right = self.new_dict(("first", "second", "third"))

        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(left))
        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(right))
        self.assertEqual(
            _testinternalcapi.dict_indexed_key_index(left, "second"),
            1,
        )
        self.assertEqual(
            _testinternalcapi.dict_indexed_key_index(right, "second"),
            1,
        )

        _testinternalcapi.dict_set_indexed_item(left, 1, 20)
        _testinternalcapi.dict_set_indexed_item(left, 0, 10)
        _testinternalcapi.dict_set_indexed_item(left, 1, 21)

        self.assertEqual(
            _testinternalcapi.dict_get_indexed_item(left, 0),
            10,
        )
        self.assertEqual(
            _testinternalcapi.dict_get_indexed_item(left, 1),
            21,
        )
        with self.assertRaises(KeyError):
            _testinternalcapi.dict_get_indexed_item(left, 2)
        self.assertEqual(list(left), ["second", "first"])
        self.assertEqual(right, {})

    def test_indexed_dict_deletion_and_reinsertion_convert(self):
        dct = self.new_dict(("first", "second", "third"))
        dct["first"] = 1
        dct["second"] = 2
        dct["third"] = 3

        del dct["second"]
        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(dct))
        self.assertEqual(list(dct), ["first", "third"])
        with self.assertRaises(KeyError):
            _testinternalcapi.dict_get_indexed_item(dct, 1)
        with self.assertRaises(RuntimeError):
            _testinternalcapi.dict_set_indexed_item(dct, 1, 20)

        dct["second"] = 20
        self.assertFalse(_testinternalcapi.dict_has_indexed_keys(dct))
        self.assertEqual(list(dct), ["first", "third", "second"])
        self.assertEqual(dct, {"first": 1, "third": 3, "second": 20})

    def test_indexed_dict_unknown_key_converts(self):
        dct = self.new_dict(("first", "second"))
        dct["second"] = 2
        dct["other"] = 3

        self.assertFalse(_testinternalcapi.dict_has_indexed_keys(dct))
        self.assertEqual(list(dct), ["second", "other"])
        self.assertEqual(dct, {"second": 2, "other": 3})

    def test_indexed_dict_copy_clear_and_iteration(self):
        dct = self.new_dict(("first", "second", "third"))
        dct["third"] = 3
        dct["first"] = 1

        copied = dct.copy()
        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(copied))
        self.assertEqual(list(copied), ["third", "first"])
        self.assertEqual(list(reversed(copied)), ["first", "third"])
        self.assertEqual(list(copied.values()), [3, 1])
        self.assertEqual(list(copied.items()), [("third", 3), ("first", 1)])
        self.assertEqual(repr(copied), "{'third': 3, 'first': 1}")

        copied.clear()
        self.assertTrue(_testinternalcapi.dict_has_indexed_keys(copied))
        self.assertEqual(copied, {})
        _testinternalcapi.dict_set_indexed_item(copied, 0, 10)
        self.assertEqual(copied, {"first": 10})

    def test_indexed_dict_large_unicode_keyset(self):
        keys = tuple(f"key_{index}" for index in range(300))
        dct = self.new_dict(keys)
        for index in (0, 127, 255, 299):
            _testinternalcapi.dict_set_indexed_item(dct, index, index)

        self.assertEqual(list(dct), [keys[0], keys[127], keys[255], keys[299]])
        for index in (0, 127, 255, 299):
            self.assertEqual(
                _testinternalcapi.dict_get_indexed_item(dct, index),
                index,
            )

    def test_indexed_dict_rejects_invalid_keysets(self):
        with self.assertRaises(TypeError):
            self.new_dict(("valid", 1))
        with self.assertRaises(ValueError):
            self.new_dict(("duplicate", "duplicate"))

    def test_split_key_layout_events(self):
        class Point:
            pass

        _testinternalcapi.dict_get_key_layout_events()
        _testinternalcapi.dict_watch_split_keys_for_type(Point)

        point = Point()
        point.x = 1
        point.y = 2

        events = _testinternalcapi.dict_get_key_layout_events()
        point_events = [
            (key, index)
            for owner, key, index in events
            if owner is Point
        ]
        self.assertEqual(point_events, [("x", 0), ("y", 1)])
        self.assertEqual(_testinternalcapi.dict_get_key_layout_events(), [])

        with self.assertRaises(TypeError):
            _testinternalcapi.dict_watch_split_keys_for_type(int)


class CAPITest(unittest.TestCase):

    def test_dict_check(self):
        check = _testlimitedcapi.dict_check
        self.assertTrue(check({1: 2}))
        self.assertTrue(check(OrderedDict({1: 2})))
        self.assertFalse(check(UserDict({1: 2})))
        self.assertFalse(check([1, 2]))
        self.assertFalse(check(object()))
        # CRASHES check(NULL)

    def test_dict_checkexact(self):
        check = _testlimitedcapi.dict_checkexact
        self.assertTrue(check({1: 2}))
        self.assertFalse(check(OrderedDict({1: 2})))
        self.assertFalse(check(UserDict({1: 2})))
        self.assertFalse(check([1, 2]))
        self.assertFalse(check(object()))
        # CRASHES check(NULL)

    def test_dict_new(self):
        dict_new = _testlimitedcapi.dict_new
        dct = dict_new()
        self.assertEqual(dct, {})
        self.assertIs(type(dct), dict)
        dct2 = dict_new()
        self.assertIsNot(dct2, dct)

    def test_dictproxy_new(self):
        dictproxy_new = _testlimitedcapi.dictproxy_new
        for dct in {1: 2}, OrderedDict({1: 2}), UserDict({1: 2}):
            proxy = dictproxy_new(dct)
            self.assertIs(type(proxy), MappingProxyType)
            self.assertEqual(proxy, dct)
            with self.assertRaises(TypeError):
                proxy[1] = 3
            self.assertEqual(proxy[1], 2)
            dct[1] = 4
            self.assertEqual(proxy[1], 4)

        self.assertRaises(TypeError, dictproxy_new, [])
        self.assertRaises(TypeError, dictproxy_new, 42)
        # CRASHES dictproxy_new(NULL)

    def test_dict_copy(self):
        copy = _testlimitedcapi.dict_copy
        for dct in {1: 2}, OrderedDict({1: 2}):
            dct_copy = copy(dct)
            self.assertIs(type(dct_copy), dict)
            self.assertEqual(dct_copy, dct)

        self.assertRaises(SystemError, copy, UserDict())
        self.assertRaises(SystemError, copy, [])
        self.assertRaises(SystemError, copy, 42)
        self.assertRaises(SystemError, copy, NULL)

    def test_dict_clear(self):
        clear = _testlimitedcapi.dict_clear
        dct = {1: 2}
        clear(dct)
        self.assertEqual(dct, {})

        # NOTE: It is not safe to call it with OrderedDict.

        # Has no effect for non-dicts.
        dct = UserDict({1: 2})
        clear(dct)
        self.assertEqual(dct, {1: 2})
        lst = [1, 2]
        clear(lst)
        self.assertEqual(lst, [1, 2])
        clear(object())

        # CRASHES? clear(NULL)

    def test_dict_size(self):
        size = _testlimitedcapi.dict_size
        self.assertEqual(size({1: 2}), 1)
        self.assertEqual(size(OrderedDict({1: 2})), 1)

        self.assertRaises(SystemError, size, UserDict())
        self.assertRaises(SystemError, size, [])
        self.assertRaises(SystemError, size, 42)
        self.assertRaises(SystemError, size, object())
        self.assertRaises(SystemError, size, NULL)

    def test_dict_getitem(self):
        getitem = _testlimitedcapi.dict_getitem
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertEqual(getitem(dct, 'a'), 1)
        self.assertIs(getitem(dct, 'b'), KeyError)
        self.assertEqual(getitem(dct, '\U0001f40d'), 2)

        dct2 = DictSubclass(dct)
        self.assertEqual(getitem(dct2, 'a'), 1)
        self.assertIs(getitem(dct2, 'b'), KeyError)

        with support.catch_unraisable_exception() as cm:
            self.assertIs(getitem({}, []), KeyError)  # unhashable
            self.assertEqual(cm.unraisable.exc_type, TypeError)
            self.assertEqual(str(cm.unraisable.exc_value),
                             "unhashable type: 'list'")

        self.assertIs(getitem(42, 'a'), KeyError)
        self.assertIs(getitem([1], 0), KeyError)
        # CRASHES getitem({}, NULL)
        # CRASHES getitem(NULL, 'a')

    def test_dict_getitemstring(self):
        getitemstring = _testlimitedcapi.dict_getitemstring
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertEqual(getitemstring(dct, b'a'), 1)
        self.assertIs(getitemstring(dct, b'b'), KeyError)
        self.assertEqual(getitemstring(dct, '\U0001f40d'.encode()), 2)

        dct2 = DictSubclass(dct)
        self.assertEqual(getitemstring(dct2, b'a'), 1)
        self.assertIs(getitemstring(dct2, b'b'), KeyError)

        with support.catch_unraisable_exception() as cm:
            self.assertIs(getitemstring({}, INVALID_UTF8), KeyError)
            self.assertEqual(cm.unraisable.exc_type, UnicodeDecodeError)
            self.assertRegex(str(cm.unraisable.exc_value),
                             "'utf-8' codec can't decode")

        self.assertIs(getitemstring(42, b'a'), KeyError)
        self.assertIs(getitemstring([], b'a'), KeyError)
        # CRASHES getitemstring({}, NULL)
        # CRASHES getitemstring(NULL, b'a')

    def test_dict_getitemref(self):
        getitem = _testcapi.dict_getitemref
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertEqual(getitem(dct, 'a'), 1)
        self.assertIs(getitem(dct, 'b'), KeyError)
        self.assertEqual(getitem(dct, '\U0001f40d'), 2)

        dct2 = DictSubclass(dct)
        self.assertEqual(getitem(dct2, 'a'), 1)
        self.assertIs(getitem(dct2, 'b'), KeyError)

        self.assertRaises(SystemError, getitem, 42, 'a')
        self.assertRaises(TypeError, getitem, {}, [])  # unhashable
        self.assertRaises(SystemError, getitem, [], 1)
        self.assertRaises(SystemError, getitem, [], 'a')
        # CRASHES getitem({}, NULL)
        # CRASHES getitem(NULL, 'a')

    def test_dict_getitemstringref(self):
        getitemstring = _testcapi.dict_getitemstringref
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertEqual(getitemstring(dct, b'a'), 1)
        self.assertIs(getitemstring(dct, b'b'), KeyError)
        self.assertEqual(getitemstring(dct, '\U0001f40d'.encode()), 2)

        dct2 = DictSubclass(dct)
        self.assertEqual(getitemstring(dct2, b'a'), 1)
        self.assertIs(getitemstring(dct2, b'b'), KeyError)

        self.assertRaises(SystemError, getitemstring, 42, b'a')
        self.assertRaises(UnicodeDecodeError, getitemstring, {}, INVALID_UTF8)
        self.assertRaises(SystemError, getitemstring, [], b'a')
        # CRASHES getitemstring({}, NULL)
        # CRASHES getitemstring(NULL, b'a')

    def test_dict_getitemwitherror(self):
        getitem = _testlimitedcapi.dict_getitemwitherror
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertEqual(getitem(dct, 'a'), 1)
        self.assertIs(getitem(dct, 'b'), KeyError)
        self.assertEqual(getitem(dct, '\U0001f40d'), 2)

        dct2 = DictSubclass(dct)
        self.assertEqual(getitem(dct2, 'a'), 1)
        self.assertIs(getitem(dct2, 'b'), KeyError)

        self.assertRaises(SystemError, getitem, 42, 'a')
        self.assertRaises(TypeError, getitem, {}, [])  # unhashable
        self.assertRaises(SystemError, getitem, [], 1)
        self.assertRaises(SystemError, getitem, [], 'a')
        # CRASHES getitem({}, NULL)
        # CRASHES getitem(NULL, 'a')

    def test_dict_contains(self):
        contains = _testlimitedcapi.dict_contains
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertTrue(contains(dct, 'a'))
        self.assertFalse(contains(dct, 'b'))
        self.assertTrue(contains(dct, '\U0001f40d'))

        dct2 = DictSubclass(dct)
        self.assertTrue(contains(dct2, 'a'))
        self.assertFalse(contains(dct2, 'b'))

        self.assertRaises(TypeError, contains, {}, [])  # unhashable
        # CRASHES contains({}, NULL)
        # CRASHES contains(UserDict(), 'a')
        # CRASHES contains(42, 'a')
        # CRASHES contains(NULL, 'a')

    def test_dict_contains_string(self):
        contains_string = _testcapi.dict_containsstring
        dct = {'a': 1, '\U0001f40d': 2}
        self.assertTrue(contains_string(dct, b'a'))
        self.assertFalse(contains_string(dct, b'b'))
        self.assertTrue(contains_string(dct, '\U0001f40d'.encode()))
        self.assertRaises(UnicodeDecodeError, contains_string, dct, INVALID_UTF8)

        dct2 = DictSubclass(dct)
        self.assertTrue(contains_string(dct2, b'a'))
        self.assertFalse(contains_string(dct2, b'b'))

        # CRASHES contains({}, NULL)
        # CRASHES contains(NULL, b'a')

    def test_dict_setitem(self):
        setitem = _testlimitedcapi.dict_setitem
        dct = {}
        setitem(dct, 'a', 5)
        self.assertEqual(dct, {'a': 5})
        setitem(dct, '\U0001f40d', 8)
        self.assertEqual(dct, {'a': 5, '\U0001f40d': 8})

        dct2 = DictSubclass()
        setitem(dct2, 'a', 5)
        self.assertEqual(dct2, {'a': 5})

        self.assertRaises(TypeError, setitem, {}, [], 5)  # unhashable
        self.assertRaises(SystemError, setitem, UserDict(), 'a', 5)
        self.assertRaises(SystemError, setitem, [1], 0, 5)
        self.assertRaises(SystemError, setitem, 42, 'a', 5)
        # CRASHES setitem({}, NULL, 5)
        # CRASHES setitem({}, 'a', NULL)
        # CRASHES setitem(NULL, 'a', 5)

    def test_dict_setitemstring(self):
        setitemstring = _testlimitedcapi.dict_setitemstring
        dct = {}
        setitemstring(dct, b'a', 5)
        self.assertEqual(dct, {'a': 5})
        setitemstring(dct, '\U0001f40d'.encode(), 8)
        self.assertEqual(dct, {'a': 5, '\U0001f40d': 8})

        dct2 = DictSubclass()
        setitemstring(dct2, b'a', 5)
        self.assertEqual(dct2, {'a': 5})

        self.assertRaises(UnicodeDecodeError, setitemstring, {}, INVALID_UTF8, 5)
        self.assertRaises(SystemError, setitemstring, UserDict(), b'a', 5)
        self.assertRaises(SystemError, setitemstring, 42, b'a', 5)
        # CRASHES setitemstring({}, NULL, 5)
        # CRASHES setitemstring({}, b'a', NULL)
        # CRASHES setitemstring(NULL, b'a', 5)

    def test_dict_delitem(self):
        delitem = _testlimitedcapi.dict_delitem
        dct = {'a': 1, 'c': 2, '\U0001f40d': 3}
        delitem(dct, 'a')
        self.assertEqual(dct, {'c': 2, '\U0001f40d': 3})
        self.assertRaises(KeyError, delitem, dct, 'b')
        delitem(dct, '\U0001f40d')
        self.assertEqual(dct, {'c': 2})

        dct2 = DictSubclass({'a': 1, 'c': 2})
        delitem(dct2, 'a')
        self.assertEqual(dct2, {'c': 2})
        self.assertRaises(KeyError, delitem, dct2, 'b')

        self.assertRaises(TypeError, delitem, {}, [])  # unhashable
        self.assertRaises(SystemError, delitem, UserDict({'a': 1}), 'a')
        self.assertRaises(SystemError, delitem, [1], 0)
        self.assertRaises(SystemError, delitem, 42, 'a')
        # CRASHES delitem({}, NULL)
        # CRASHES delitem(NULL, 'a')

    def test_dict_delitemstring(self):
        delitemstring = _testlimitedcapi.dict_delitemstring
        dct = {'a': 1, 'c': 2, '\U0001f40d': 3}
        delitemstring(dct, b'a')
        self.assertEqual(dct, {'c': 2, '\U0001f40d': 3})
        self.assertRaises(KeyError, delitemstring, dct, b'b')
        delitemstring(dct, '\U0001f40d'.encode())
        self.assertEqual(dct, {'c': 2})

        dct2 = DictSubclass({'a': 1, 'c': 2})
        delitemstring(dct2, b'a')
        self.assertEqual(dct2, {'c': 2})
        self.assertRaises(KeyError, delitemstring, dct2, b'b')

        self.assertRaises(UnicodeDecodeError, delitemstring, {}, INVALID_UTF8)
        self.assertRaises(SystemError, delitemstring, UserDict({'a': 1}), b'a')
        self.assertRaises(SystemError, delitemstring, 42, b'a')
        # CRASHES delitemstring({}, NULL)
        # CRASHES delitemstring(NULL, b'a')

    def test_dict_setdefault(self):
        setdefault = _testcapi.dict_setdefault
        dct = {}
        self.assertEqual(setdefault(dct, 'a', 5), 5)
        self.assertEqual(dct, {'a': 5})
        self.assertEqual(setdefault(dct, 'a', 8), 5)
        self.assertEqual(dct, {'a': 5})

        dct2 = DictSubclass()
        self.assertEqual(setdefault(dct2, 'a', 5), 5)
        self.assertEqual(dct2, {'a': 5})
        self.assertEqual(setdefault(dct2, 'a', 8), 5)
        self.assertEqual(dct2, {'a': 5})

        self.assertRaises(TypeError, setdefault, {}, [], 5)  # unhashable
        self.assertRaises(SystemError, setdefault, UserDict(), 'a', 5)
        self.assertRaises(SystemError, setdefault, [1], 0, 5)
        self.assertRaises(SystemError, setdefault, 42, 'a', 5)
        # CRASHES setdefault({}, NULL, 5)
        # CRASHES setdefault({}, 'a', NULL)
        # CRASHES setdefault(NULL, 'a', 5)

    def test_dict_setdefaultref(self):
        setdefault = _testcapi.dict_setdefaultref
        dct = {}
        self.assertEqual(setdefault(dct, 'a', 5), 5)
        self.assertEqual(dct, {'a': 5})
        self.assertEqual(setdefault(dct, 'a', 8), 5)
        self.assertEqual(dct, {'a': 5})

        dct2 = DictSubclass()
        self.assertEqual(setdefault(dct2, 'a', 5), 5)
        self.assertEqual(dct2, {'a': 5})
        self.assertEqual(setdefault(dct2, 'a', 8), 5)
        self.assertEqual(dct2, {'a': 5})

        self.assertRaises(TypeError, setdefault, {}, [], 5)  # unhashable
        self.assertRaises(SystemError, setdefault, UserDict(), 'a', 5)
        self.assertRaises(SystemError, setdefault, [1], 0, 5)
        self.assertRaises(SystemError, setdefault, 42, 'a', 5)
        # CRASHES setdefault({}, NULL, 5)
        # CRASHES setdefault({}, 'a', NULL)
        # CRASHES setdefault(NULL, 'a', 5)

    def test_mapping_keys_valuesitems(self):
        class BadMapping(dict):
            def keys(self):
                return None
            def values(self):
                return None
            def items(self):
                return None
        dict_obj = {'foo': 1, 'bar': 2, 'spam': 3}
        for mapping in [dict_obj, DictSubclass(dict_obj), BadMapping(dict_obj)]:
            self.assertListEqual(_testlimitedcapi.dict_keys(mapping),
                                 list(dict_obj.keys()))
            self.assertListEqual(_testlimitedcapi.dict_values(mapping),
                                 list(dict_obj.values()))
            self.assertListEqual(_testlimitedcapi.dict_items(mapping),
                                 list(dict_obj.items()))

    def test_dict_keys_valuesitems_bad_arg(self):
        for mapping in UserDict(), [], object():
            self.assertRaises(SystemError, _testlimitedcapi.dict_keys, mapping)
            self.assertRaises(SystemError, _testlimitedcapi.dict_values, mapping)
            self.assertRaises(SystemError, _testlimitedcapi.dict_items, mapping)

    def test_dict_next(self):
        dict_next = _testlimitedcapi.dict_next
        self.assertIsNone(dict_next({}, 0))
        dct = {'a': 1, 'b': 2, 'c': 3}
        pos = 0
        pairs = []
        while True:
            res = dict_next(dct, pos)
            if res is None:
                break
            rc, pos, key, value = res
            self.assertEqual(rc, 1)
            pairs.append((key, value))
        self.assertEqual(pairs, list(dct.items()))

        # CRASHES dict_next(NULL, 0)

    def test_dict_update(self):
        update = _testlimitedcapi.dict_update
        for cls1 in dict, DictSubclass:
            for cls2 in dict, DictSubclass, UserDict:
                dct = cls1({'a': 1, 'b': 2})
                update(dct, cls2({'b': 3, 'c': 4}))
                self.assertEqual(dct, {'a': 1, 'b': 3, 'c': 4})

        self.assertRaises(AttributeError, update, {}, [])
        self.assertRaises(AttributeError, update, {}, 42)
        self.assertRaises(SystemError, update, UserDict(), {})
        self.assertRaises(SystemError, update, 42, {})
        self.assertRaises(SystemError, update, {}, NULL)
        self.assertRaises(SystemError, update, NULL, {})

    def test_dict_merge(self):
        merge = _testlimitedcapi.dict_merge
        for cls1 in dict, DictSubclass:
            for cls2 in dict, DictSubclass, UserDict:
                dct = cls1({'a': 1, 'b': 2})
                merge(dct, cls2({'b': 3, 'c': 4}), 0)
                self.assertEqual(dct, {'a': 1, 'b': 2, 'c': 4})
                dct = cls1({'a': 1, 'b': 2})
                merge(dct, cls2({'b': 3, 'c': 4}), 1)
                self.assertEqual(dct, {'a': 1, 'b': 3, 'c': 4})

        self.assertRaises(AttributeError, merge, {}, [], 0)
        self.assertRaises(AttributeError, merge, {}, 42, 0)
        self.assertRaises(SystemError, merge, UserDict(), {}, 0)
        self.assertRaises(SystemError, merge, 42, {}, 0)
        self.assertRaises(SystemError, merge, {}, NULL, 0)
        self.assertRaises(SystemError, merge, NULL, {}, 0)

    def test_dict_mergefromseq2(self):
        mergefromseq2 = _testlimitedcapi.dict_mergefromseq2
        for cls1 in dict, DictSubclass:
            for cls2 in list, iter:
                dct = cls1({'a': 1, 'b': 2})
                mergefromseq2(dct, cls2([('b', 3), ('c', 4)]), 0)
                self.assertEqual(dct, {'a': 1, 'b': 2, 'c': 4})
                dct = cls1({'a': 1, 'b': 2})
                mergefromseq2(dct, cls2([('b', 3), ('c', 4)]), 1)
                self.assertEqual(dct, {'a': 1, 'b': 3, 'c': 4})

        self.assertRaises(ValueError, mergefromseq2, {}, [(1,)], 0)
        self.assertRaises(ValueError, mergefromseq2, {}, [(1, 2, 3)], 0)
        self.assertRaises(TypeError, mergefromseq2, {}, [1], 0)
        self.assertRaises(TypeError, mergefromseq2, {}, 42, 0)
        # CRASHES mergefromseq2(UserDict(), [], 0)
        # CRASHES mergefromseq2(42, [], 0)
        # CRASHES mergefromseq2({}, NULL, 0)
        # CRASHES mergefromseq2(NULL, {}, 0)

    def test_dict_pop(self):
        # Test PyDict_Pop()
        dict_pop = _testcapi.dict_pop
        dict_pop_null = _testcapi.dict_pop_null

        # key present, get removed value
        mydict = {"key": "value", "key2": "value2"}
        self.assertEqual(dict_pop(mydict, "key"), (1, "value"))
        self.assertEqual(mydict, {"key2": "value2"})
        self.assertEqual(dict_pop(mydict, "key2"), (1, "value2"))
        self.assertEqual(mydict, {})

        # key present, ignore removed value
        mydict = {"key": "value", "key2": "value2"}
        self.assertEqual(dict_pop_null(mydict, "key"), 1)
        self.assertEqual(mydict, {"key2": "value2"})
        self.assertEqual(dict_pop_null(mydict, "key2"), 1)
        self.assertEqual(mydict, {})

        # key missing, expect removed value; empty dict has a fast path
        self.assertEqual(dict_pop({}, "key"), (0, NULL))
        self.assertEqual(dict_pop({"a": 1}, "key"), (0, NULL))

        # key missing, ignored removed value; empty dict has a fast path
        self.assertEqual(dict_pop_null({}, "key"), 0)
        self.assertEqual(dict_pop_null({"a": 1}, "key"), 0)

        # dict error
        not_dict = UserDict({1: 2})
        self.assertRaises(SystemError, dict_pop, not_dict, "key")
        self.assertRaises(SystemError, dict_pop_null, not_dict, "key")

        # key error; don't hash key if dict is empty
        not_hashable_key = ["list"]
        self.assertEqual(dict_pop({}, not_hashable_key), (0, NULL))
        with self.assertRaises(TypeError):
            dict_pop({'key': 1}, not_hashable_key)
        dict_pop({}, NULL)  # key is not checked if dict is empty

        # CRASHES dict_pop(NULL, "key")
        # CRASHES dict_pop({"a": 1}, NULL)

    def test_dict_popstring(self):
        # Test PyDict_PopString()
        dict_popstring = _testcapi.dict_popstring
        dict_popstring_null = _testcapi.dict_popstring_null

        # key present, get removed value
        mydict = {"key": "value", "key2": "value2"}
        self.assertEqual(dict_popstring(mydict, "key"), (1, "value"))
        self.assertEqual(mydict, {"key2": "value2"})
        self.assertEqual(dict_popstring(mydict, "key2"), (1, "value2"))
        self.assertEqual(mydict, {})

        # key present, ignore removed value
        mydict = {"key": "value", "key2": "value2"}
        self.assertEqual(dict_popstring_null(mydict, "key"), 1)
        self.assertEqual(mydict, {"key2": "value2"})
        self.assertEqual(dict_popstring_null(mydict, "key2"), 1)
        self.assertEqual(mydict, {})

        # key missing; empty dict has a fast path
        self.assertEqual(dict_popstring({}, "key"), (0, NULL))
        self.assertEqual(dict_popstring_null({}, "key"), 0)
        self.assertEqual(dict_popstring({"a": 1}, "key"), (0, NULL))
        self.assertEqual(dict_popstring_null({"a": 1}, "key"), 0)

        # non-ASCII key
        non_ascii = '\U0001f40d'
        dct = {'\U0001f40d': 123}
        self.assertEqual(dict_popstring(dct, '\U0001f40d'.encode()), (1, 123))
        dct = {'\U0001f40d': 123}
        self.assertEqual(dict_popstring_null(dct, '\U0001f40d'.encode()), 1)

        # dict error
        not_dict = UserDict({1: 2})
        self.assertRaises(SystemError, dict_popstring, not_dict, "key")
        self.assertRaises(SystemError, dict_popstring_null, not_dict, "key")

        # key error
        self.assertRaises(UnicodeDecodeError, dict_popstring, {1: 2}, INVALID_UTF8)
        self.assertRaises(UnicodeDecodeError, dict_popstring_null, {1: 2}, INVALID_UTF8)

        # CRASHES dict_popstring(NULL, "key")
        # CRASHES dict_popstring({}, NULL)
        # CRASHES dict_popstring({"a": 1}, NULL)


if __name__ == "__main__":
    unittest.main()
