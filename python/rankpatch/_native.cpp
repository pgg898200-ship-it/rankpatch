#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "rankpatch.hpp"
#include "rankpatch_steady.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
constexpr const char *capsule_name = "rankpatch.Index";
using State = std::variant<rankpatch::Index, rankpatch::SteadyIndex>;

void destroy_index(PyObject *capsule) {
    delete static_cast<State *>(PyCapsule_GetPointer(capsule, capsule_name));
}

PyObject *translate_exception() {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return PyErr_NoMemory();
    } catch (const std::out_of_range &e) {
        PyErr_SetString(PyExc_IndexError, e.what());
    } catch (const std::invalid_argument &e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    } catch (const std::length_error &e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    } catch (const std::overflow_error &e) {
        PyErr_SetString(PyExc_OverflowError, e.what());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "unexpected native exception");
    }
    return nullptr;
}

template <typename T>
bool unsigned_integer(PyObject *obj, const char *label, T &out) {
    if (PyBool_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be an integer, got bool", label);
        return false;
    }
    PyObject *number = PyNumber_Index(obj);
    if (!number) return false;
    const auto value = PyLong_AsUnsignedLongLong(number);
    Py_DECREF(number);
    if (PyErr_Occurred()) return false;
    if (value > std::numeric_limits<T>::max()) {
        PyErr_Format(PyExc_OverflowError, "%s exceeds its supported range", label);
        return false;
    }
    out = static_cast<T>(value);
    return true;
}

bool read_vector(PyObject *obj, const char *label, std::vector<uint32_t> &out) {
    // Materialize an immutable snapshot before calling arbitrary __index__ methods.
    // A mutable input list may be edited from one of those methods.
    std::unique_ptr<PyObject, decltype(&Py_DecRef)> items(PySequence_Tuple(obj), Py_DecRef);
    if (!items) return false;
    const Py_ssize_t count = PyTuple_GET_SIZE(items.get());
    out.resize(static_cast<size_t>(count));
    for (Py_ssize_t i = 0; i < count; ++i) {
        if (!unsigned_integer(PyTuple_GET_ITEM(items.get(), i), label,
                              out[static_cast<size_t>(i)])) {
            return false;
        }
    }
    return true;
}

PyObject *create(PyObject *, PyObject *args) {
    PyObject *sources, *universe, *points, *count;
    const char *strategy = "buffered";
    if (!PyArg_ParseTuple(args, "OOOO|s:create", &sources, &universe, &points, &count, &strategy)) {
        return nullptr;
    }
    try {
        size_t s, u, k;
        std::vector<uint32_t> weights;
        if (!unsigned_integer(sources, "sources", s) ||
            !unsigned_integer(universe, "universe", u) ||
            !unsigned_integer(count, "k", k) ||
            !read_vector(points, "rank point", weights)) return nullptr;
        std::unique_ptr<State> index;
        if (std::string_view(strategy) == "buffered") {
            index = std::make_unique<State>(std::in_place_type<rankpatch::Index>,
                                            s, u, std::move(weights), k);
        } else if (std::string_view(strategy) == "steady") {
            index = std::make_unique<State>(std::in_place_type<rankpatch::SteadyIndex>,
                                            s, u, std::move(weights), k);
        } else {
            throw std::invalid_argument("strategy must be 'buffered' or 'steady'");
        }
        PyObject *capsule = PyCapsule_New(index.get(), capsule_name, destroy_index);
        if (capsule) index.release();
        return capsule;
    } catch (...) {
        return translate_exception();
    }
}

PyObject *replace(PyObject *, PyObject *args) {
    PyObject *capsule, *source, *revision, *items;
    if (!PyArg_ParseTuple(args, "OOOO:replace", &capsule, &source, &revision, &items)) {
        return nullptr;
    }
    auto *state = static_cast<State *>(PyCapsule_GetPointer(capsule, capsule_name));
    if (!state) return nullptr;
    try {
        size_t s;
        uint64_t rev;
        std::vector<uint32_t> ids;
        if (!unsigned_integer(source, "source", s) ||
            !unsigned_integer(revision, "revision", rev)) return nullptr;
        return std::visit([&](auto &index) -> PyObject * {
            if (!index.is_fresh(s, rev)) Py_RETURN_FALSE;
            if (!read_vector(items, "document ID", ids)) return nullptr;
            return PyBool_FromLong(index.replace(s, rev, ids));
        }, *state);
    } catch (...) {
        return translate_exception();
    }
}

PyObject *top(PyObject *, PyObject *capsule) {
    auto *state = static_cast<State *>(PyCapsule_GetPointer(capsule, capsule_name));
    if (!state) return nullptr;
    try {
        // Copy the rows before allocating Python objects: GC finalizers can reenter.
        const auto snapshot = std::visit([](auto &index) { return index.top(); }, *state);
        if (snapshot.size() > static_cast<size_t>(PY_SSIZE_T_MAX)) {
            PyErr_SetString(PyExc_OverflowError, "too many results");
            return nullptr;
        }
        PyObject *list = PyList_New(static_cast<Py_ssize_t>(snapshot.size()));
        if (!list) return nullptr;
        Py_ssize_t pos = 0;
        for (const auto &row : snapshot) {
            PyObject *pair = Py_BuildValue("(KK)",
                static_cast<unsigned long long>(row.first),
                static_cast<unsigned long long>(row.second));
            if (!pair) {
                Py_DECREF(list);
                return nullptr;
            }
            PyList_SET_ITEM(list, pos++, pair);
        }
        return list;
    } catch (...) {
        return translate_exception();
    }
}

PyMethodDef methods[] = {
    {"create", create, METH_VARARGS, "Construct the native ranked-list index."},
    {"replace", replace, METH_VARARGS, "Apply a source revision."},
    {"top", top, METH_O, "Read a top-k snapshot."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_native", nullptr, -1, methods,
    nullptr, nullptr, nullptr, nullptr
};
} // namespace

PyMODINIT_FUNC PyInit__native() {
    return PyModule_Create(&module);
}
