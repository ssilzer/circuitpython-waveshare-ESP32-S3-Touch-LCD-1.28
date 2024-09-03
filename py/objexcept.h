/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MICROPY_INCLUDED_PY_OBJEXCEPT_H
#define MICROPY_INCLUDED_PY_OBJEXCEPT_H

#include "py/obj.h"
#include "py/objtuple.h"
// CIRCUITPY-CHANGE: changes here and below for traceback.
#include "py/objtraceback.h"

typedef struct _mp_obj_exception_t {
    mp_obj_base_t base;
    mp_obj_tuple_t *args;
    // CIRCUITPY-CHANGE
    mp_obj_traceback_t *traceback;
    #if MICROPY_CPYTHON_EXCEPTION_CHAIN
    struct _mp_obj_exception_t *cause, *context;
    bool suppress_context;
    bool marked;
    #endif
} mp_obj_exception_t;

void mp_obj_exception_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind);
void mp_obj_exception_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);
// CIRCUITPY-CHANGE: new routines
void mp_obj_exception_initialize0(mp_obj_exception_t *o_exc, const mp_obj_type_t *type);
mp_obj_exception_t *mp_obj_exception_get_native(mp_obj_t self_in);

#define MP_DEFINE_EXCEPTION(exc_name, base_name) \
    MP_DEFINE_CONST_OBJ_TYPE(mp_type_##exc_name, MP_QSTR_##exc_name, MP_TYPE_FLAG_NONE, \
    make_new, mp_obj_exception_make_new, \
    print, mp_obj_exception_print, \
    attr, mp_obj_exception_attr, \
    parent, &mp_type_##base_name \
    );

#endif // MICROPY_INCLUDED_PY_OBJEXCEPT_H
