/*
 * Copyright 2015 CRS4
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* PyArg_ParseTuple "s#" needs this */
#define PY_SSIZE_T_CLEAN

#include "deserializer.h"
#include "convert.h"
#include "structmember.h"
#include "error.h"

static int
AvroDeserializer_init(AvroDeserializer *self, PyObject *args, PyObject *kwds)
{
    int rval;
    PyObject *types = NULL;
    const char *schema_json;
    static char *kwlist[] = {"schema", "types", NULL};

    self->flags = 0;
    self->iface = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|O", kwlist,
                                     &schema_json, &types)) {
        return -1;
    }

    rval = avro_schema_from_json(schema_json, 0, &self->schema, NULL);
    if (rval != 0 || self->schema == NULL) {
        PyErr_Format(PyExc_IOError, "Error reading schema: %s",
                     avro_strerror());
        return -1;
    }
    self->flags |= DESERIALIZER_SCHEMA_OK;

    size_t len = strlen(schema_json);
    self->schema_json = malloc(len + 1);
    strncpy(self->schema_json, schema_json, len + 1); // copy with \0 character

    self->iface = avro_generic_class_from_schema(self->schema);
    if (self->iface == NULL) {
        PyErr_SetString(PyExc_IOError,
                        "Error creating generic class interface");
        return -1;
    }

    self->datum_reader = avro_reader_memory(NULL, 0);
    if (!self->datum_reader) {
        PyErr_NoMemory();
        return -1;
    }
    self->flags |= DESERIALIZER_READER_OK;

    /* copied verbatim from filereader */
    if (types != NULL && PyObject_IsTrue(types)) {
        /* we still haven't incref'ed types here */
        if (Py_TYPE(types) == get_avro_types_type()) {
            Py_INCREF(types);
            self->info.types = types;
        } else {
            self->info.types = PyObject_CallFunctionObjArgs(
                  (PyObject *) get_avro_types_type(), NULL);
            if (self->info.types == NULL) {
                return -1;
            }
            declare_types(&self->info, self->schema);
        }
    } else {
        self->info.types = NULL;
    }

    return 0;
}

static int
do_close(AvroDeserializer* self) {
    if (self->flags & DESERIALIZER_READER_OK) {
        avro_reader_free(self->datum_reader);
        self->flags &= ~DESERIALIZER_READER_OK;
    }
    if (self->flags & DESERIALIZER_SCHEMA_OK) {
        avro_schema_decref(self->schema);
        self->flags &= ~DESERIALIZER_SCHEMA_OK;
    }
    if (self->schema_json != NULL){
        free(self->schema_json);
        self->schema_json = NULL;
    }
    if (self->iface != NULL) {
        avro_value_iface_decref(self->iface);
        self->iface = NULL;
    }
    return 0;
}

static void
AvroDeserializer_dealloc(AvroDeserializer *self)
{
    do_close(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
AvroDeserializer_deserialize(AvroDeserializer *self, PyObject *args, PyObject *kwds)
{
    int rval;
    avro_value_t value;
    char *buffer = NULL;
    Py_ssize_t buffer_size;
    const char *wschema_json = NULL;
    avro_schema_t wschema;

    static char *kwlist[] = {"buffer", "writer_schema", NULL};
    PyObject *result;

    avro_reader_reset(self->datum_reader);
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s#|z", kwlist,
            &buffer, &buffer_size, &wschema_json)) {
        return NULL;
    }
    avro_reader_memory_set_source(self->datum_reader, buffer, buffer_size);

    avro_generic_value_new(self->iface, &value);

    if(wschema_json == NULL)
    {
        // read the readers schema, no resolution needed
        rval = avro_value_read(self->datum_reader, &value);
    }
    else
    {
        rval = avro_schema_from_json(wschema_json, 0, &wschema, NULL);
        avro_value_iface_t* wiface = avro_generic_class_from_schema(wschema);

        avro_value_t source;
        avro_generic_value_new(wiface, &source);

        rval = avro_value_read_with_resolution(self->datum_reader, &source, &value);

        avro_value_decref(&source);
        avro_schema_decref(wschema);
        avro_value_iface_decref(wiface);
    }
    if (rval) {
        avro_value_decref(&value);
        set_error_prefix("Unable to deserialize data: ");
        PyErr_Format(PyExc_IOError,
                        "Deserialization error: %s", avro_strerror());

        return NULL;
    }
    result = avro_to_python(&self->info, &value);
    avro_value_decref(&value);
    return result;
}

static PyObject *
AvroDeserializer_close(AvroDeserializer *self, PyObject *args)
{
    do_close(self);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* AvroDeserializer_reduce(AvroDeserializer *self, PyObject *args)
{
    PyObject* obj = (PyObject*)self;
    PyObject* attr = PyObject_GetAttrString(obj, "__class__");
    PyObject* tuple = Py_BuildValue("O(s)", attr, self->schema_json);
    return tuple;
}


static PyMethodDef AvroDeserializer_methods[] = {
    {"close", (PyCFunction)AvroDeserializer_close, METH_VARARGS,
     "Close Avro deserializer."
    },
    {"deserialize", (PyCFunction)AvroDeserializer_deserialize, METH_VARARGS | METH_KEYWORDS,
     "Deserialize a record."
    },
    {"__reduce__", (PyCFunction)AvroDeserializer_reduce, METH_VARARGS,
     "AvroDeserializer Pickling support."
    },
    {NULL}  /* Sentinel */
};

static PyMemberDef AvroDeserializer_members[] = {
    {"types", T_OBJECT, offsetof(AvroDeserializer, info.types), 0,
     "types info"},
    /*
    {"schema_json", T_OBJECT, offsetof(AvroDeserializer, schema_json), 0,
     "schema as json"},
    */
    {NULL}  /* Sentinel */
};

PyTypeObject avroDeserializerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pyavroc.AvroDeserializer",            /* tp_name */
    sizeof(AvroDeserializer),              /* tp_basicsize */
    0,                                     /* tp_itemsize */
    (destructor)AvroDeserializer_dealloc,  /* tp_dealloc */
    0,                                     /* tp_print */
    0,                                     /* tp_getattr */
    0,                                     /* tp_setattr */
    0,                                     /* tp_compare */
    0,                                     /* tp_repr */
    0,                                     /* tp_as_number */
    0,                                     /* tp_as_sequence */
    0,                                     /* tp_as_mapping */
    0,                                     /* tp_hash */
    0,                                     /* tp_call */
    0,                                     /* tp_str */
    0,                                     /* tp_getattro */
    0,                                     /* tp_setattro */
    0,                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                    /* tp_flags */
    "AvroDeserializer objects",            /* tp_doc */
    0,                                     /* tp_traverse */
    0,                                     /* tp_clear */
    0,                                     /* tp_richcompare */
    0,                                     /* tp_weaklistoffset */
    0,                                     /* tp_iter */
    0,                                     /* tp_iternext */
    AvroDeserializer_methods,              /* tp_methods */
    AvroDeserializer_members,              /* tp_members */
    0,                                     /* tp_getset */
    0,                                     /* tp_base */
    0,                                     /* tp_dict */
    0,                                     /* tp_descr_get */
    0,                                     /* tp_descr_set */
    0,                                     /* tp_dictoffset */
    (initproc)AvroDeserializer_init,       /* tp_init */
    0,                                     /* tp_alloc */
    0,                                     /* tp_new */
};
