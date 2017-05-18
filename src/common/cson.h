/*
 * Copyright (c) 2017, Krisztian Litkey
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Author nor the names of other contributors
 *     may be used to endorse or promote products derived from this software
 *     without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MURPHY_CSON_H__
#define __MURPHY_CSON_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>

#include <murphy/common/macros.h>
#include <murphy/common/refcnt.h>

MRP_CDECL_BEGIN

#ifndef __MRP_PTRBITS
#    if __WORDSIZE == 64
#        define __MRP_PTRBITS 64
#    else
#        define __MRP_PTRBITS 32
#    endif
#endif


/**
 * \addtogroup MurphCommonInfra
 * @{
 *
 * @file cson.h
 *
 * @brief Murphy JSON object implementation.
 *
 * An implementation of JSON objects for Murphy.
 */

/**
 * @brief Opaque type for a JSON object.
 */
typedef struct mrp_cson_s mrp_cson_t;

/**
 * JSON object types.
 */

typedef enum {
    /* these types can appear as the type of an mrp_cson_t (or a compact ptr) */
    MRP_CSON_TYPE_UNKNOWN   =   -1,
    MRP_CSON_TYPE_STRING    = 0x00,
    MRP_CSON_TYPE_INTEGER   = 0x01,
    MRP_CSON_TYPE_NUMBER    = 0x02,
    MRP_CSON_TYPE_OBJECT    = 0x03,
    MRP_CSON_TYPE_ARRAY     = 0x04,
    MRP_CSON_TYPE_NULL      = 0x05,
    MRP_CSON_TYPE_FALSE     = 0x06,
    MRP_CSON_TYPE_TRUE      = 0x07,
    MRP_CSON_TYPE_INT8      = 0x08,
    MRP_CSON_TYPE_UINT8     = 0x09,
    MRP_CSON_TYPE_INT16     = 0x0a,
    MRP_CSON_TYPE_UINT16    = 0x0b,
    MRP_CSON_TYPE_INT32     = 0x0c,
    MRP_CSON_TYPE_UINT32    = 0x0d,
    MRP_CSON_TYPE_INT64     = 0x0e,
    MRP_CSON_TYPE_UINT64    = 0x0f,
    MRP_CSON_TYPE_DOUBLE    = MRP_CSON_TYPE_NUMBER,

    /* type modifiers and pseudo-types */
    MRP_CSON_TYPE_DEFAULT   = 0x00, /* default format */
    MRP_CSON_TYPE_SHARABLE  = 0x10, /* forced sharable format */
    MRP_CSON_TYPE_COMPACT   = 0x20, /* forced compact format */
    MRP_CSON_TYPE_BOOLEAN   = 0x40, /* explicitly passed boolean with stdarg */
    MRP_CSON_TYPE_END       =   -1, /* stdarg terminator */
} mrp_cson_type_t;

/*
 * We have two possible representation for CSON objects, a compact and a
 * shareable one.
 *
 * The shareable representation is a dynamically allocated and reference-
 * counted straightforward implementation with separate fields for type,
 * type-specific value, and a reference count. This representation allows
 * one to construct objects with shared members. Changing the value of any
 * shared member is visible in all objects with that member. Any type of
 * JSON object can be constructed as a shareable one.
 *
 * In the compact representation the full object is packed into the value
 * of a single ptrdiff_t/mrp_cson_t pointer. The highest bit of the pointer,
 * which is otherwise always 0 in user space, is set to 1 to indicate a
 * compact object. The top 7 bits contain the object type, and the rest of
 * the lower bits contains the type-specific value, except for the lowest
 * bit which is again set to 0. Losing the lowest bit cuts the range of
 * representable integers in half (to 22 bits 54 bits), but it allows us to
 * represent strings as compact JSON objects too, by setting the lowest bit
 * for compact strings to 1 (which is otherwise 0 for dynamically allocated
 * memory is aligned to word-size or more).
 *
 * compact JSON object, other than strings:
 *                 22 or 54 bits
 *     +----------------------------------+
 *     |1|type|...type-specific value...|0|
 *     +----------------------------------+
 *
 * compact JSON string object:
 *                 30 or 62 bits
 *     +----------------------------------+
 *     |1|...original string pointer....|1|
 *     +----------------------------------+
 */

#define MRP_CSON_TYPEBITS 2
#define MRP_CSON_PTRSHIFT (__MRP_PTRBITS - MRP_CSON_TYPEBITS)
#define MRP_CSON_PTRMASK  (0x11 << MRP_CSON_PTRSHIFT)

/* compact format value limits, masks, etc. */
#define MRP_CSON_MININT8   ((int8_t  )-0x7f - 1)
#define MRP_CSON_MAXINT8   ((int8_t  )+0x7f)
#define MRP_CSON_MAXUINT8  ((uint8_t )+0xff)
#define MRP_CSON_MININT16  ((int16_t )-0x7fff - 1)
#define MRP_CSON_MAXINT16  ((int16_t )+0x7fff)
#define MRP_CSON_MAXUINT16 ((uint16_t)+0xffff)

#define MRP_CSON_COMPACT_STR 0x1UL

#if __MRP_PTRBITS < 64
#    define MRP_CSON_COMPACT_BIT 0x80000000UL
#    define MRP_CSON_TYPE_SHIFT  27
#    define MRP_CSON_VALUE_MASK  0x00ffffffUL
#    define MRP_CSON_SIGN_BIT    0x00800000UL

#    define MRP_CSON_MININT32  ((int32_t )-0x3fffffff)
#    define MRP_CSON_MAXINT32  ((int32_t )+0x3fffffff)
#    define MRP_CSON_MAXUINT32 ((uint32_t)+0x7fffffff)
#    define MRP_CSON_MININT64  ((int64_t )MRP_CSON_MININT32)
#    define MRP_CSON_MAXINT64  ((int64_t )MRP_CSON_MAXINT32)
#    define MRP_CSON_MAXUINT64 ((uint64_t)MRP_CSON_MAXUINT32)
#    define MRP_CSON_MININT    ((signed  )MRP_CSON_MININT32)
#    define MRP_CSON_MAXINT    ((signed  )MRP_CSON_MAXINT32)
#    define MRP_CSON_MAXUINT   ((unsigned)MRP_CSON_MAXUINT32)
#else
#    define MRP_CSON_COMPACT_BIT 0x8000000000000000ULL
#    define MRP_CSON_TYPE_SHIFT  59
#    define MRP_CSON_VALUE_MASK  0x00ffffffffffffffULL
#    define MRP_CSON_SIGN_BIT    0x0080000000000000ULL

#    define MRP_CSON_MININT32  ((int32_t )-0x7fffffff)
#    define MRP_CSON_MAXINT32  ((int32_t )+0x7fffffff)
#    define MRP_CSON_MAXUINT32 ((uint32_t)+0xffffffff)
#    define MRP_CSON_MININT64  ((int64_t )-0x003fffffffffffffLL )
#    define MRP_CSON_MAXINT64  ((int64_t )+0x003fffffffffffffLL )
#    define MRP_CSON_MAXUINT64 ((uint64_t)+0x007fffffffffffffULL)
#    define MRP_CSON_MININT    ((signed  )MRP_CSON_MININT32)
#    define MRP_CSON_MAXINT    ((signed  )MRP_CSON_MAXINT32)
#    define MRP_CSON_MAXUINT   ((unsigned)MRP_CSON_MAXUINT32)
#endif

#define MRP_CSON_MIN(_t)                                                \
    MRP_CHOOSE(MRP_COMPATIBLE(_t, int8_t), MRP_CSON_MININT8,            \
      MRP_CHOOSE(MRP_COMPATIBLE(_t, int16_t), MRP_CSON_MININT16,        \
        MRP_CHOOSE(MRP_COMPATIBLE(_t, int32_t), MRP_CSON_MININT32,      \
          MRP_CHOOSE(MRP_COMPATIBLE(_t, int64_t), MRP_CSON_MININT64,    \
                     (void)0))))

#define MRP_CSON_MAX(_t)                                                \
    MRP_CHOOSE(MRP_COMPATIBLE(_t, int8_t), MRP_CSON_MAXINT8,            \
      MRP_CHOOSE(MRP_COMPATIBLE(_t, int16_t), MRP_CSON_MAXINT16,        \
        MRP_CHOOSE(MRP_COMPATIBLE(_t, int32_t), MRP_CSON_MAXINT32,      \
          MRP_CHOOSE(MRP_COMPATIBLE(_t, int64_t), MRP_CSON_MAXINT64,    \
    MRP_CHOOSE(MRP_COMPATIBLE(_t, uint8_t), MRP_CSON_MAXUINT8,          \
      MRP_CHOOSE(MRP_COMPATIBLE(_t, uint16_t), MRP_CSON_MAXUINT16,      \
        MRP_CHOOSE(MRP_COMPATIBLE(_t, uint32_t), MRP_CSON_MAXUINT32,    \
          MRP_CHOOSE(MRP_COMPATIBLE(_t, uint64_t), MRP_CSON_MAXUINT64,  \
                     (void)0))))))))

static inline int mrp_cson_is_compact(mrp_cson_t *obj)
{
    ptrdiff_t o = (ptrdiff_t)obj;

    return ((o & MRP_CSON_COMPACT_BIT) == MRP_CSON_COMPACT_BIT);
}


static inline mrp_cson_type_t mrp_cson_compact_type(mrp_cson_t *obj)
{
    ptrdiff_t o = (ptrdiff_t)obj;

    if (MRP_UNLIKELY(!mrp_cson_is_compact(obj))) {
        errno = EINVAL;
        return -1;
    }

    if (o & MRP_CSON_COMPACT_STR)
        return MRP_CSON_TYPE_STRING;
    else
        return (o >> MRP_CSON_TYPE_SHIFT) & 0xf;
}


static inline ptrdiff_t mrp_cson_compact_value(mrp_cson_t *obj)
{
    ptrdiff_t       v = (ptrdiff_t)obj;
    uint64_t        u = (ptrdiff_t)obj;
    mrp_cson_type_t type;
    int             neg;

    type = mrp_cson_compact_type(obj);
    switch (type) {
    case MRP_CSON_TYPE_NULL:
    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
        return (type == MRP_CSON_TYPE_TRUE);

    case MRP_CSON_TYPE_BOOLEAN:
        return (v & MRP_CSON_VALUE_MASK) != 0;

    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        neg = (v & MRP_CSON_SIGN_BIT) != 0;
        v   = (v & (MRP_CSON_VALUE_MASK >> 1)) >> 1;
        v   = neg ? -v : v;

        return v;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        return (v & MRP_CSON_VALUE_MASK) >> 1;

    case MRP_CSON_TYPE_STRING:
        return v & ~(MRP_CSON_COMPACT_BIT | MRP_CSON_COMPACT_STR);

    default:
        errno = EINVAL;
        return (ptrdiff_t)-1;
    }
}



static inline mrp_cson_t *mrp_cson_compact(mrp_cson_type_t type, ...)
{
    va_list        ap;
    char          *str;
    ptrdiff_t      i, u, neg, o;
    int64_t        i64, smin, smax;
    uint64_t       u64, umax;

    va_start(ap, type);

    switch (type) {
    case MRP_CSON_TYPE_NULL:
    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
        o = (type == MRP_CSON_TYPE_TRUE) << 1;
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        i = va_arg(ap, int);
        o = (!!i) << 1;
        break;

    case MRP_CSON_TYPE_INT8:
        smin = MRP_CSON_MIN(int8_t) , smax = MRP_CSON_MAX(int8_t);
        goto check_signed;
    case MRP_CSON_TYPE_INT16:
        smin = MRP_CSON_MIN(int16_t), smax = MRP_CSON_MAX(int16_t);
        goto check_signed;
    case MRP_CSON_TYPE_INT32:
        smin = MRP_CSON_MIN(int32_t), smax = MRP_CSON_MAX(int32_t);
    check_signed:
        i = va_arg(ap, int);

        if ((neg = (i < 0)))
            u = -i;
        else
            u = i;

        if (u & ~(MRP_CSON_VALUE_MASK >> 1))
            goto range_error;

        o = (neg ? MRP_CSON_SIGN_BIT : 0) | (u << 1);
        break;

    case MRP_CSON_TYPE_UINT8:
        umax = MRP_CSON_MAX(uint8_t);
        goto check_unsigned;
    case MRP_CSON_TYPE_UINT16:
        umax = MRP_CSON_MAX(uint16_t);
        goto check_unsigned;
    case MRP_CSON_TYPE_UINT32:
        umax = MRP_CSON_MAX(uint32_t);
    check_unsigned:
        u = va_arg(ap, unsigned int);

        if (u & ~MRP_CSON_VALUE_MASK)
            goto range_error;

        o = (u << 1);
        break;

    case MRP_CSON_TYPE_INT64:
        smin = MRP_CSON_MIN(int64_t), smax = MRP_CSON_MAX(int64_t);
        i64 = va_arg(ap, int64_t);

        if ((neg = (i64 < 0)))
            u64 = -i64;
        else
            u64 = i64;

        if (u64 & ~(MRP_CSON_VALUE_MASK >> 1))
            goto range_error;

        o = (neg ? MRP_CSON_SIGN_BIT : 0) | (u64 << 1);
        break;


    case MRP_CSON_TYPE_UINT64:
        umax = MRP_CSON_MAX(uint64_t);
        u64 = va_arg(ap, uint64_t);

        if (u64 & ~MRP_CSON_VALUE_MASK)
            goto range_error;

        o = (u64 << 1);
        break;

    case MRP_CSON_TYPE_STRING:
        o = mrp_strdup(str = va_arg(ap, char *));

        if (!o && str != NULL)
            goto nomem_error;
        break;

    default:
        goto type_error;
        break; /* stupid gcc... */
    }

    va_end(ap);

    MRP_ASSERT(!(o & (MRP_CSON_COMPACT_BIT | MRP_CSON_COMPACT_STR)),
               "compact value has lowest or highest bit already set");

    if (type == MRP_CSON_TYPE_STRING)
        o |= MRP_CSON_COMPACT_BIT | MRP_CSON_COMPACT_STR;
    else {
        o |= MRP_CSON_COMPACT_BIT | (((ptrdiff_t)type) << MRP_CSON_TYPE_SHIFT);
    }

    return o;

 range_error:
    errno = ERANGE;
    goto fail;
 type_error:
    errno = EINVAL;
 nomem_error:
 fail:
    va_end(ap);
    return -1;
}


/*
 * macros, function prototypes
 */

#define MRP_CSON_TYPE(_t, ...) MRP_CSON_TYPE_##_t, __VA_ARGS__
#define MRP_CSON_STRING(_s)  MRP_CSON_TYPE(STRING , _s)
#define MRP_CSON_INTEGER(_i) MRP_CSON_TYPE(INTEGER, _i)
#define MRP_CSON_BOOLEAN(_b) MRP_CSON_TYPE(BOOLEAN, _b)
#define MRP_CSON_DOUBLE(_d)  MRP_CSON_TYPE(DOUBLE , _d)
#define MRP_CSON_OBJECT(...) MRP_CSON_TYPE(OBJECT,__VA_ARGS__,MRP_CSON_TYPE_END)
#define MRP_CSON_ARRAY(...)  MRP_CSON_TYPE(ARRAY ,__VA_ARGS__,MRP_CSON_TYPE_END)


#if 0
#define MRP_CSON_COMPACT(_t, _v) MRP_CSON_MAKE_COMPACT(MRP_CSON_TYPE_##_t, _v)

#define MRP_CSON_MAKE_COMPACT(_type, _v) ({                                \
            ptrdiff_t _val = (ptrdiff_t)_v;                                \
            switch (_type) {                                               \
            case MRP_CSON_TYPE_STRING:                                     \
                MRP_ASSERT(builtin_constant_p(_v), "string not constant"); \
                _val |= MRP_CSON_STRING;                                   \
                break;                                                     \
            case MRP_CSON_INTEGER:                                         \

            })
#endif



/**
 * @brief Macro to refer to all member names.
 */
#define MRP_CSON_ALL_NAMES ((const char *)1)

/**
 * @brief Tell the library to expect a member name,
 *
 * Let the library know that a given name is expected as a member name
 * within in objects. If @name is @MRP_CSON_ALL_NAMES, all member names
 * will be expected to be used by multiple object instances.
 *
 * @param [in] name  the member name to expect
 *
 * @return Returns 0 if the library successfully preallocated the given
 *         member name, -1 otherwise.
 */
int mrp_cson_expect_name(const char *name);

/**
 * @brief Tell the library to stop expecting a member name.
 *
 * Let the library know that a name advertised earlier to be expected as
 * a member in multiple objects is not to be expected any more. If @name
 * is @MRP_CSON_ALL_NAMES, no particular member names are expected any
 * more.
 *
 * Note that calls to @mrp_cson_expect_name and @mrp_cson_forget_name should
 * either be balanced for each name (including @MRP_CSON_ALL_NAMES), or have
 * fewer @mrp_cson_forget_name calls. In particular it is an error to have
 * more @mrp_cson_forget_name than @mrp_cson_expect_name calls for any name.
 *
 * @param [in] name  the name to stop expecting
 */
void mrp_cson_forget_name(const char *name);

/**
 * @brief Create a new JSON object.
 *
 * Create a JSON object of the specified type, and initialize it with the
 * given type-specific parameter. You should use one the * MRP_CSON_* macros
 * to pass the type together with the type-specific initializer.
 *
 * @param [in] type  type of the JSON object to create
 * @param [in] ...   type-specfic initializer
 *
 * @return Returns the newly created JSON object with its reference count
 *         initialized to 1.
 */
mrp_cson_t *mrp_cson_create(mrp_cson_type_t type, ...);

/**
 * @brief Get the type of a JSON object.
 *
 * Get the specific type of a given JSON object.
 *
 * @param [in] o  object to get the specific type of
 *
 * @return Returns the type of the given object, or @MRP_CSON_TYPE_NULL if
 *         @o is @NULL.
 */
mrp_cson_type_t mrp_cson_get_type(mrp_cson_t *o);

/**
 * @brief Increase the reference count of a JSON object.
 *
 * Increase the reference count of the given JSON object, taking shared
 * ownership of it.
 *
 * @param [in] o  the object to inrease the reference count of
 *
 * @return Returns @o.
 */
mrp_cson_t *mrp_cson_ref(mrp_cson_t *o);

/**
 * @brief Decrease the reference count of a JSON object.
 *
 * Decrease the reference count of te given JSON object, freeing it if
 * the last reference is gone.
 *
 * @param [in] o  the JSON object to decrease the reference count of
 *
 * @return Return non-zero if this was the last reference and the object
 *         has been freed.
 */
int mrp_cson_unref(mrp_cson_t *o);

/**
 * @brief Set a member of a JSON object.
 */
int mrp_cson_set(mrp_cson_t *o, const char *name, mrp_cson_t *v);

/**
 * @brief Get a ember of a JSON object.
 */
mrp_cson_t *mrp_cson_get(mrp_cson_t *o, const char *name);

/**
 * @brief JSON object printing and conversion to strings.
 *
 * We register a printf modifier flag ("JSON") and we override
 * the printf handler function for "%p". Our handler tells the
 * caller it cannot handle the call if the JSON modifier was not
 * specified, letting the default implementation take over.
 *
 * This allows us to print or pretty print JSON objects using the
 * stock C library printf family of functions like this:
 *
 * void print_cson(FILE *fp, mrp_cson_t *o, int pretty)
 * {
 *     if (pretty)
 *         fprintf(fp, "%p: pretty-CSON %#CSONp\n", o, o);
 *     else
 *         fprintf(fp, "%p: CSON: %CSONp\n", o, o);
 * }
 */

#define MRP_CSON_PRINTF_MOD  "CSON"
#define MRP_CSON_PRINTF_SPEC 'p'

/**
 * @brief Supported printing styles.
 */

typedef enum {
    MRP_CSON_PRINT_DEFAULT,
    MRP_CSON_PRINT_COMPACT = MRP_CSON_PRINT_DEFAULT,
    MRP_CSON_PRINT_PRETTY,
} mrp_cson_print_t;


/**
 * @}
 */

MRP_CDECL_END

#endif /* __MURPHY_CSON_H__ */
