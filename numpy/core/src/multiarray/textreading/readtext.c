#include <stdio.h>
#include <stdbool.h>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#include "numpy/arrayobject.h"
#include "npy_argparse.h"
#include "common.h"
#include "conversion_utils.h"

#include "textreading/parser_config.h"
#include "textreading/stream_pyobject.h"
#include "textreading/field_types.h"
#include "textreading/rows.h"
#include "textreading/str_to_int.h"


//
// `usecols` must point to a Python object that is Py_None or a 1-d contiguous
// numpy array with data type int32.
//
// `dtype` must point to a Python object that is Py_None or a numpy dtype
// instance.  If the latter, code and sizes must be arrays of length
// num_dtype_fields, holding the flattened data field type codes and byte
// sizes. (num_dtype_fields, codes, and sizes can be inferred from dtype,
// but we do that in Python code.)
//
// If both `usecols` and `dtype` are not None, and the data type is compound,
// then len(usecols) must equal num_dtype_fields.
//
// If `dtype` is given and it is compound, and `usecols` is None, then the
// number of columns in the file must match the number of fields in `dtype`.
//
static PyObject *
_readtext_from_stream(stream *s,
        parser_config *pc, Py_ssize_t num_usecols, Py_ssize_t usecols[],
        Py_ssize_t skiprows, Py_ssize_t max_rows,
        PyObject *converters, PyObject *dtype)
{
    PyArrayObject *arr = NULL;
    PyArray_Descr *out_dtype = NULL;
    field_type *ft = NULL;

    /*
     * If dtypes[0] is dtype the input was not structured and the result
     * is considered "homogeneous" and we have to discover the number of
     * columns/
     */
    out_dtype = (PyArray_Descr *)dtype;
    Py_INCREF(out_dtype);

    Py_ssize_t num_fields = field_types_create(out_dtype, &ft);
    if (num_fields < 0) {
        goto finish;
    }
    bool homogeneous = num_fields == 1 && ft[0].descr == out_dtype;

    if (!homogeneous && usecols != NULL && num_usecols != num_fields) {
        PyErr_Format(PyExc_TypeError,
                "If a structured dtype is used, the number of columns in "
                "`usecols` must match the effective number of fields. "
                "But %zd usecols were given and the number of fields is %zd.",
                num_usecols, num_fields);
        goto finish;
    }

    arr = read_rows(
            s, max_rows, num_fields, ft, pc,
            num_usecols, usecols, skiprows, converters,
            NULL, out_dtype, homogeneous);
    if (arr == NULL) {
        goto finish;
    }

  finish:
    Py_XDECREF(out_dtype);
    field_types_xclear(num_fields, ft);
    return (PyObject *)arr;
}


static int
parse_control_character(PyObject *obj, Py_UCS4 *character)
{
    if (obj == Py_None) {
        *character = (Py_UCS4)-1;  /* character beyond unicode range */
        return 1;
    }
    if (!PyUnicode_Check(obj) || PyUnicode_GetLength(obj) != 1) {
        PyErr_Format(PyExc_TypeError,
                "Text reading control character must be a single unicode "
                "character or None; but got: %.100R", obj);
        return 0;
    }
    *character = PyUnicode_READ_CHAR(obj, 0);
    return 1;
}


NPY_NO_EXPORT PyObject *
_load_from_filelike(PyObject *NPY_UNUSED(mod),
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyObject *file;
    Py_ssize_t skiprows = 0;
    Py_ssize_t max_rows = -1;
    PyObject *usecols_obj = Py_None;
    PyObject *converters = Py_None;

    PyObject *dtype = Py_None;
    PyObject *encoding_obj = Py_None;
    const char *encoding = NULL;

    parser_config pc = {
        .delimiter = ',',
        .comment = '#',
        .quote = '"',
        .imaginary_unit = 'j',
        .allow_embedded_newline = true,
        .delimiter_is_whitespace = false,
        .ignore_leading_whitespace = false,
        .python_byte_converters = false,
        .c_byte_converters = false,
    };
    bool filelike = true;

    PyObject *arr = NULL;

    NPY_PREPARE_ARGPARSER;
    if (npy_parse_arguments("_load_from_filelike", args, len_args, kwnames,
            "file", NULL, &file,
            "|delimiter", &parse_control_character, &pc.delimiter,
            "|comment", &parse_control_character, &pc.comment,
            "|quote", &parse_control_character, &pc.quote,
            "|imaginary_unit", &parse_control_character, &pc.imaginary_unit,
            "|usecols", NULL, &usecols_obj,
            "|skiprows", &PyArray_IntpFromPyIntConverter, &skiprows,
            "|max_rows", &PyArray_IntpFromPyIntConverter, &max_rows,
            "|converters", NULL, &converters,
            "|dtype", NULL, &dtype,
            "|encoding", NULL, &encoding_obj,
            "|filelike", &PyArray_BoolConverter, &filelike,
            "|byte_converters", &PyArray_BoolConverter, &pc.python_byte_converters,
            "|c_byte_converters", PyArray_BoolConverter, &pc.c_byte_converters,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    if (pc.delimiter == (Py_UCS4)-1) {
        pc.delimiter_is_whitespace = true;
        /* Ignore leading whitespace to match `string.split(None)` */
        pc.ignore_leading_whitespace = true;
    }

    if (!PyArray_DescrCheck(dtype) ) {
        PyErr_SetString(PyExc_TypeError,
                "internal error: dtype must be provided and be a NumPy dtype");
        return NULL;
    }

    if (encoding_obj != Py_None) {
        if (!PyUnicode_Check(encoding_obj)) {
            PyErr_SetString(PyExc_TypeError,
                    "encoding must be a unicode string.");
            return NULL;
        }
        encoding = PyUnicode_AsUTF8(encoding_obj);
        if (encoding == NULL) {
            return NULL;
        }
    }

    /*
     * Parse usecols, the rest of NumPy has no clear helper for this, so do
     * it here manually.
     */
    Py_ssize_t num_usecols = -1;
    Py_ssize_t *usecols = NULL;
    if (usecols_obj != Py_None) {
        num_usecols = PySequence_Length(usecols_obj);
        if (num_usecols < 0) {
            return NULL;
        }
        /* Calloc just to not worry about overflow */
        usecols = PyMem_Calloc(num_usecols, sizeof(Py_ssize_t));
        for (Py_ssize_t i = 0; i < num_usecols; i++) {
            PyObject *tmp = PySequence_GetItem(usecols_obj, i);
            if (tmp == NULL) {
                return NULL;
            }
            usecols[i] = PyNumber_AsSsize_t(tmp, PyExc_OverflowError);
            if (error_converting(usecols[i])) {
                if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                    PyErr_Format(PyExc_TypeError,
                            "usecols must be an int or a sequence of ints but "
                            "it contains at least one element of type '%s'",
                            tmp->ob_type->tp_name);
                }
                Py_DECREF(tmp);
                return NULL;
            }
            Py_DECREF(tmp);
        }
    }

    stream *s;
    if (filelike) {
        s = stream_python_file(file, encoding);
    }
    else {
        s = stream_python_iterable(file, encoding);
    }
    if (s == NULL) {
        PyErr_Format(PyExc_RuntimeError, "Unable to access the file.");
        PyMem_FREE(usecols);
        return NULL;
    }

    arr = _readtext_from_stream(
            s, &pc, num_usecols, usecols, skiprows, max_rows, converters, dtype);
    stream_close(s);
    PyMem_FREE(usecols);
    return arr;
}

