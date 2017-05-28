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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <printf.h>


#include <murphy/common/macros.h>
#include <murphy/common/mm.h>
#include <murphy/common/debug.h>
#include <murphy/common/list.h>
#include <murphy/common/refcnt.h>
#include <murphy/common/hash-table.h>
#include <murphy/common/cson.h>


/* this will probably need to go to the header, for iterating over members */
typedef struct mrp_cson_member_s mrp_cson_member_t;

/* a JSON object member */
struct mrp_cson_member_s {
    uint32_t         id;                     /* member name (symbol id) */
    mrp_cson_t      *value;                  /* member value */
    mrp_list_hook_t  hook;                   /* to parent object */
};

/* a JSON object */
struct mrp_cson_s {
    mrp_cson_type_t type;                    /* MRP_CSON_TYPE_* */
    mrp_refcnt_t    refcnt;                  /* reference count */
    union {                                  /* type-specific value */
        char     *str;                       /* _STRING */
        int64_t   i;                         /* _INTEGER, _INT* */
        uint64_t  u;                         /* _UINT* */
        double    dbl;                       /* _NUMBER, _DOUBLE */

        struct {                             /* _OBJECT */
            uint32_t        blmmask;         /*   member bloom mask */
            mrp_list_hook_t members;         /*   members */
        } object;

        struct {                             /* _ARRAY */
            size_t       size;               /* allocated array elements */
            size_t       nitem;              /* number of array elements */
            mrp_cson_t **items;              /* array element values */
        } array;
    };
};


static int default_mode;                     /* compact vs. sparse default */
static int mod_cson;                         /* printf CSON %p modifier flag */


/*
 * symbol-table implementation
 *
 * The assumption made in the implementation of this library is that
 * typically we will be dealing with a relatively large number of
 * instances of a relatively small number of JSON object types. IOW,
 * we expect to repeatedly encounter a fair number of structurally
 * identical objects with slightly different values.
 *
 * Based on this assumption, instead of storing object member names as
 * strings directly in the object instances, we enumerate member names
 * and store a numeric ID instead in the object instances. A member name
 * is thus shared among all object instances having a member with the
 * same enumerated name.
 *
 * It is possible pre-declare names as being expected by the library.
 * Expected names will be readily enumerated and kept around even without
 * any active objects referencing them.
 */

/* an enumerated JSON member name */
typedef struct {
    char         *name;                      /* string (member name) */
    uint32_t      id;                        /* string id (cookie) */
    uint32_t      hash;                      /* string hash value */
    mrp_refcnt_t  refcnt;                    /* active references */
} cson_sym_t;


static mrp_hashtbl_t *symtbl;                /* symbol (member name) table */
static int            expect_all;            /* expect/lock all member names */


static void symbol_free(void *key, void *obj);


static mrp_hashtbl_t *symtbl_create(void)
{
    mrp_hashtbl_config_t cfg;

    mrp_clear(&cfg);
    cfg.hash = mrp_hash_string;
    cfg.comp = mrp_comp_string;
    cfg.free = symbol_free;

    return (symtbl = mrp_hashtbl_create(&cfg));
}


static inline cson_sym_t *symbol_ref(cson_sym_t *sym)
{
    return mrp_ref_obj(sym, refcnt);
}


static inline int symbol_unref(cson_sym_t *sym)
{
    return mrp_unref_obj(sym, refcnt);
}


static cson_sym_t *symbol_get(const char *str, int create)
{
    uint32_t    cookie;
    cson_sym_t *sym;

    mrp_debug("looking up symbol '%s' (create: %s)", str, create ? "yes" : "no");

    cookie = MRP_HASH_COOKIE_NONE;
    sym    = mrp_hashtbl_lookup(symtbl, str, cookie);

    if (sym != NULL)
        return create ? symbol_ref(sym) : sym;
    else if (!create)
        return NULL;

    if ((sym = mrp_allocz(sizeof(*sym))) == NULL)
        goto nomem;
    if ((sym->name = mrp_strdup(str)) == NULL)
        goto nomem;

    if (mrp_hashtbl_add(symtbl, sym->name, sym, &cookie) < 0)
        goto add_fail;

    mrp_refcnt_init(&sym->refcnt);
    sym->id   = cookie;
    sym->hash = (1 << ((cookie - 1) % (8 * sizeof(sym->hash))));
    symbol_ref(sym);

    if (expect_all)
        symbol_ref(sym);

    mrp_debug("created symbol '%s' (0x%x, hash: %u)", str, sym->id, sym->hash);

    return sym;

 add_fail:
    mrp_free(sym->name);
 nomem:
    mrp_free(sym);
    return NULL;
}


#define symbol_create(_s) symbol_get(_s, TRUE)


static void symbol_free(void *key, void *obj)
{
    cson_sym_t *sym = obj;

    MRP_UNUSED(key);

    if (sym != NULL) {
        mrp_debug("destroying symbol '%s' (0x%x)", sym->name, sym->id);
        mrp_free(sym->name);
        mrp_free(sym);
    }
}


static cson_sym_t *symbol_lookup(uint32_t id)
{
    return mrp_hashtbl_fetch(symtbl, id);
}


static inline uint32_t symbol_id(const char *name, int add)
{
    cson_sym_t *sym = symbol_get(name, add);

    return sym ? sym->id : MRP_HASH_COOKIE_NONE;
}


static const char *symbol_name(uint32_t id)
{
    cson_sym_t *sym = mrp_hashtbl_fetch(symtbl, id);

    return sym ? sym->name : "<unknow-symbol-id>";
}


int mrp_cson_expect_name(const char *name)
{
    if (name != MRP_CSON_ALL_NAMES) {
        cson_sym_t *sym = symbol_create(name);
        return sym ? 0 : -1;
    }
    else {
        /* XXX hmm... should we extra ref all existing symbols here ? */
        expect_all++;
        return 0;
    }
}


void mrp_cson_forget_name(const char *name)
{
    if (name != MRP_CSON_ALL_NAMES) {
        symbol_unref(symbol_get(name, FALSE));
    }
    else {
        expect_all--;

        if (expect_all < 0) {
            mrp_log_error("imbalanced mrp_cson_{expect,forget}_name calls");
            expect_all = 0;
            /* XXX hmm... should we unref all existing symbols here ? */;
        }
    }
}


int mrp_cson_set_default_mode(mrp_cson_type_t mode)
{
    default_mode = mode;

    return 0;
}


/*
 * compact mode implementation
 */

static inline int compactable_type(mrp_cson_type_t type)
{
    switch (type) {
    case MRP_CSON_TYPE_NULL:
    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
    case MRP_CSON_TYPE_BOOLEAN:
    case MRP_CSON_TYPE_STRING:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_UINT16:
        return TRUE;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_INT64:
    case MRP_CSON_TYPE_UINT64:
        /* XXX TODO: should check range first... */
        return TRUE;

    default:
        return FALSE;
    }
}


static inline int compact_object(const mrp_cson_t *obj)
{
    ptrdiff_t o = (ptrdiff_t)obj;

    return ((o & MRP_CSON_COMPACT_BIT) == MRP_CSON_COMPACT_BIT);
}


static inline mrp_cson_type_t compact_type(mrp_cson_t *obj)
{
    ptrdiff_t o = (ptrdiff_t)obj;

    if (MRP_UNLIKELY(!compact_object(obj))) {
        errno = EINVAL;
        return -1;
    }

    if (o & MRP_CSON_COMPACT_STR)
        return MRP_CSON_TYPE_STRING;
    else
        return (o >> MRP_CSON_TYPE_SHIFT) & 0xf;
}


static inline ptrdiff_t compact_value(mrp_cson_t *obj)
{
    ptrdiff_t       v = (ptrdiff_t)obj;
    mrp_cson_type_t type;
    int             neg;

    type = compact_type(obj);

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
    case MRP_CSON_TYPE_INTEGER:
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


static inline mrp_cson_t *compact_createv(mrp_cson_type_t type, va_list *ap)
{
    int        neg;
    ptrdiff_t  o, i, u;
    int64_t    i64;
    uint64_t   u64;
    char      *str;

    switch (type) {
    case MRP_CSON_TYPE_NULL:
    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
        o = (type == MRP_CSON_TYPE_TRUE) << 1;
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        i = va_arg(*ap, int);
        type = MRP_CSON_TYPE_FALSE + (va_arg(*ap, int) ? 1 : 0);
        o = 0;
        break;

    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INTEGER:
        i = va_arg(*ap, int);

        if ((neg = (i < 0)))
            i = -i;

        if (i & ~(MRP_CSON_VALUE_MASK >> 1))
            goto range_error;

        o = (neg ? MRP_CSON_SIGN_BIT : 0) | (i << 1);
        break;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
        u = va_arg(*ap, unsigned int);

        if (u & ~MRP_CSON_VALUE_MASK)
            goto range_error;

        o = (u << 1);
        break;

    case MRP_CSON_TYPE_INT64:
        i64 = va_arg(*ap, int64_t);

        if ((neg = (i64 < 0)))
            u64 = -i64;
        else
            u64 = i64;

        if (u64 & ~(MRP_CSON_VALUE_MASK >> 1))
            goto range_error;

        o = (neg ? MRP_CSON_SIGN_BIT : 0) | (u64 << 1);
        break;


    case MRP_CSON_TYPE_UINT64:
        u64 = va_arg(*ap, uint64_t);

        if (u64 & ~MRP_CSON_VALUE_MASK)
            goto range_error;

        o = (u64 << 1);
        break;

    case MRP_CSON_TYPE_STRING:
        o = (ptrdiff_t)mrp_strdup(str = va_arg(*ap, char *));

        if (!o && str != NULL)
            goto nomem_error;
        break;

    default:
        goto type_error;
        break; /* stupid gcc... */
    }

    MRP_ASSERT(!(o & (MRP_CSON_COMPACT_BIT | MRP_CSON_COMPACT_STR)),
               "compact value has lowest or highest bit already set");

    if (type == MRP_CSON_TYPE_STRING)
        o |= MRP_CSON_COMPACT_BIT | MRP_CSON_COMPACT_STR;
    else
        o |= MRP_CSON_COMPACT_BIT | (((ptrdiff_t)type) << MRP_CSON_TYPE_SHIFT);

    return (mrp_cson_t *)o;

 range_error:
    errno = ERANGE;
    goto fail;
 type_error:
    errno = EINVAL;
 nomem_error:
 fail:
    return (mrp_cson_t *)(ptrdiff_t)-1;
}


static inline mrp_cson_t *compact_create(mrp_cson_type_t type, ...)
{
    mrp_cson_t *o;
    va_list     ap;

    va_start(ap, type);
    o = compact_createv(type, &ap);
    va_end(ap);

    return o;
}


static inline mrp_cson_t *compact_ref(mrp_cson_t *obj)
{
    if (!compact_object(obj))
        return NULL;

    if (compact_type(obj) == MRP_CSON_TYPE_STRING)
        return compact_create(MRP_CSON_TYPE_STRING, compact_value(obj));
    else
        return obj;
}


static inline int compact_unref(mrp_cson_t *obj)
{
    if (!compact_object(obj))
        return -1;

    if (compact_type(obj) == MRP_CSON_TYPE_STRING)
        mrp_free((char *)compact_value(obj));

    return 1;
}


static int compact_print(char *buf, size_t size, mrp_cson_t *o)
{
    ptrdiff_t v;
    int       l;

    v = compact_value(o);

    switch (compact_type(o)) {
    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "false");
        break;
    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "true");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "NULL");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "'%s'", (char *)v);
        break;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%lld", (long long)v);
        break;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%llu", (unsigned long long)v);
        break;

    default:
        l = snprintf(buf, size, "<invalid-comapct-CSON-type>");
        break;
    }

    return l;

}


static int compact_print_pretty(char *buf, size_t size, mrp_cson_t *o, int lvl)
{
    ptrdiff_t v;
    int       l;

    MRP_UNUSED(lvl);

    v = compact_value(o);

    switch (compact_type(o)) {
    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "false");
        break;
    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "true");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "NULL");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "'%s'", (char *)v);
        break;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%lld", (long long)v);
        break;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%llu", (unsigned long long)v);
        break;

    default:
        l = snprintf(buf, size, "<invalid-comapct-CSON-type>\n");
        break;
    }

    return l;
}


ptrdiff_t mrp_cson_compact_value(mrp_cson_t *o)
{
    return compact_value(o);
}



/*
 * full/shareable mode implementation
 */

static mrp_cson_t *shareable_createv(mrp_cson_type_t type, va_list *ap)
{
    mrp_cson_t *o;
    const char *str;

    o = mrp_allocz(sizeof(*o));

    if (o == NULL)
        return NULL;

    o->type = type;
    mrp_refcnt_init(&o->refcnt);

    switch (type) {
    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
    case MRP_CSON_TYPE_NULL:
        break;

    case MRP_CSON_TYPE_STRING:
        o->str = mrp_strdup(str = va_arg(*ap, const char *));

        if (o->str == NULL && str != NULL)
            goto nomem;
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        o->type = MRP_CSON_TYPE_FALSE + (va_arg(*ap, int) ? 1 : 0);
        break;

    case MRP_CSON_TYPE_INTEGER:
        o->i = va_arg(*ap, int);
        break;

    case MRP_CSON_TYPE_INT8:
        o->i = (int8_t)va_arg(*ap, int);
        break;
    case MRP_CSON_TYPE_UINT8:
        o->u = (uint8_t)va_arg(*ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT16:
        o->i = (int16_t)va_arg(*ap, int);
        break;
    case MRP_CSON_TYPE_UINT16:
        o->u = (uint16_t)va_arg(*ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT32:
        o->i = (int32_t)va_arg(*ap, int);
        break;
    case MRP_CSON_TYPE_UINT32:
        o->u = (uint32_t)va_arg(*ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT64:
        o->i = (int64_t)va_arg(*ap, int64_t);
        break;
    case MRP_CSON_TYPE_UINT64:
        o->u = (uint64_t)va_arg(*ap, uint64_t);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        o->dbl = va_arg(*ap, double);
        break;

    case MRP_CSON_TYPE_OBJECT:
        mrp_list_init(&o->object.members);
        break;

    case MRP_CSON_TYPE_ARRAY:
        type = va_arg(*ap, int);
        break;

    default:
        goto invalid_type;
        break;
    }

    return o;

 invalid_type:
    errno = EINVAL;
 nomem:
    mrp_free(o);

    return NULL;
}


static mrp_cson_t *shareable_create(mrp_cson_type_t type, ...)
{
    mrp_cson_t *o;
    va_list     ap;

    va_start(ap, type);
    o = shareable_createv(type, &ap);
    va_end(ap);

    return o;
}


static void shareable_free(mrp_cson_t *o)
{
    mrp_list_hook_t   *p, *n;
    mrp_cson_member_t *m;
    cson_sym_t        *sym;
    size_t             i;

    if (o == NULL)
        return;

    switch (o->type) {
    case MRP_CSON_TYPE_STRING:
        mrp_free(o->str);
        break;

    case MRP_CSON_TYPE_OBJECT:
        mrp_list_foreach(&o->object.members, p, n) {
            m = mrp_list_entry(p, typeof(*m), hook);
            mrp_list_delete(&m->hook);
            sym = symbol_lookup(m->id);
            symbol_unref(sym);
            mrp_cson_unref(m->value);
        }
        break;

    case MRP_CSON_TYPE_ARRAY:
        for (i = 0; i < o->array.nitem; i++)
            mrp_cson_unref(o->array.items[i]);
        mrp_free(o->array.items);
        break;

    default:
        break;
    }

    o->type = MRP_CSON_TYPE_UNKNOWN;
    mrp_free(o);
}


static inline int shareable_object(mrp_cson_t *obj)
{
    return !compact_object(obj);
}


static inline mrp_cson_type_t shareable_type(mrp_cson_t *o)
{
    return o ? o->type : MRP_CSON_TYPE_NULL;
}


static mrp_cson_t *shareable_ref(mrp_cson_t *o)
{
    return mrp_ref_obj(o, refcnt);
}


static int shareable_unref(mrp_cson_t *o)
{
    int last = mrp_unref_obj(o, refcnt);

    if (last)
        shareable_free(o);

    return last;
}


static mrp_cson_member_t *shareable_get(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m;
    mrp_list_hook_t   *p, *n;
    cson_sym_t        *sym;

    if (o == NULL)
        goto notfound;

    if (!shareable_object(o) || o->type != MRP_CSON_TYPE_OBJECT)
        goto invalid_type;

    if ((sym = symbol_get(name, FALSE)) == NULL)
        goto notfound;

    if ((o->object.blmmask & sym->hash) != sym->hash)
        goto notfound;

    mrp_list_foreach(&o->object.members, p, n) {
        m = mrp_list_entry(p, mrp_cson_member_t, hook);

        if (m->id == sym->id)
            return m;
    }

notfound:
    errno = ENOENT;
    return NULL;

invalid_type:
    errno = EINVAL;
    return NULL;
}


static int shareable_set(mrp_cson_t *o, const char *name, mrp_cson_t *v)
{
    mrp_cson_member_t *m = NULL;
    cson_sym_t        *sym;

    if (o == NULL)
        goto invalid_type;

    if (!shareable_object(o) || o->type != MRP_CSON_TYPE_OBJECT)
        goto invalid_type;

    if ((m = shareable_get(o, name)) != NULL)
        mrp_cson_unref(m->value);
    else {
        m   = mrp_allocz(sizeof(*m));
        sym = symbol_get(name, TRUE);

        if (m == NULL || sym == NULL)
            goto nomem;

        mrp_list_init(&m->hook);
        m->id = sym->id;

        mrp_list_append(&o->object.members, &m->hook);
        o->object.blmmask |= sym->hash;
    }

    m->value = v;

    return 0;

 invalid_type:
    errno = EINVAL;
 nomem:
    mrp_free(m);
    return -1;
}


static int shareable_del(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m = shareable_get(o, name);
    cson_sym_t        *sym;

    if ((m = shareable_get(o, name)) == NULL)
        return -1;

    mrp_list_delete(&m->hook);

    sym = symbol_lookup(m->id);
    symbol_unref(sym);
    mrp_cson_unref(m->value);

    mrp_free(m);

    return 0;
}


static int shareable_print_pretty(char *buf, size_t size, mrp_cson_t *o, int lvl)
{
    int l, c;

    MRP_UNUSED(lvl);

    if (o == NULL)
        return 0;

    l = 0;

    switch (o->type) {
    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "false");
        break;
    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "true");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "NULL");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "'%s'", o->str);
        break;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%lld", (long long)o->i);
        break;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%llu", (unsigned long long)o->u);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        l = snprintf(buf, size, "%f", o->dbl);
        break;

    case MRP_CSON_TYPE_OBJECT: {
        mrp_cson_member_t *m;
        mrp_list_hook_t   *p, *n;
        const char        *t;

        c    = snprintf(buf, size, "{");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);

        t = "";
        mrp_list_foreach(&o->object.members, p, n) {
            m = mrp_list_entry(p, mrp_cson_member_t, hook);

            c    = snprintf(buf, size, "%s%s: %#CSONp", t,
                            symbol_name(m->id), m->value);
            l   += c;
            buf += c;
            size = (c > (int)size ? 0 : size - c);
            t    = ", ";
        }

        c    = snprintf(buf, size, "}");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);
    }
        break;

    case MRP_CSON_TYPE_ARRAY: {
        mrp_cson_t *e;
        int         i;
        const char *t;

        c    = snprintf(buf, size, "[");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);

        t = "";
        for (i = 0; i < (int)o->array.nitem; i++) {
            e = o->array.items[i];
            c    = snprintf(buf, size, "%s%#CSONp", t, e);
            l   += c;
            buf += c;
            size = (c > (int)size ? 0 : size - c);
            t    = ",";
        }

        c    = snprintf(buf, size, "]");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);
    }
        break;

    default:
        l = snprintf(buf, size, "<unknown CSON type>\n");
        break;

    }

    return l;
}


static int shareable_print(char *buf, size_t size, mrp_cson_t *o)
{
    int l, c;

    if (o == NULL)
        return 0;

    l = 0;

    switch (o->type) {
    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "false");
        break;
    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "true");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "NULL");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "'%s'", o->str);
        break;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%lld", (long long)o->i);
        break;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%llu", (unsigned long long)o->u);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        l = snprintf(buf, size, "%f", o->dbl);
        break;

    case MRP_CSON_TYPE_OBJECT: {
        mrp_cson_member_t *m;
        mrp_list_hook_t   *p, *n;
        const char        *t;

        c    = snprintf(buf, size, "{");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);

        t = "";
        mrp_list_foreach(&o->object.members, p, n) {
            m = mrp_list_entry(p, mrp_cson_member_t, hook);
            c    = snprintf(buf, size, "%s%s:%#CSONp", t,
                            symbol_name(m->id), m->value);
            l   += c;
            buf += c;
            size = (c > (int)size ? 0 : size - c);
            t    = ",";
        }

        c    = snprintf(buf, size, "}");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);
    }
        break;

    case MRP_CSON_TYPE_ARRAY: {
        mrp_cson_t *e;
        int         i;
        const char *t;

        c    = snprintf(buf, size, "[");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);

        t = "";
        for (i = 0; i < (int)o->array.nitem; i++) {
            e = o->array.items[i];
            c    = snprintf(buf, size, "%s%#CSONp,", t, e);
            l   += c;
            buf += c;
            size = (c > (int)size ? 0 : size - c);
            t    = ",";
        }

        c    = snprintf(buf, size, "]");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);
    }
        break;

    default:
        l = snprintf(buf, size, "<unknown-CSON-type>\n");
        break;
    }

    return l;
}


/*
 * representation-agnostic functions
 */

mrp_cson_t *mrp_cson_create(mrp_cson_type_t t, ...)
{
    mrp_cson_t      *o;
    mrp_cson_type_t  type, mode, bln;
    va_list          ap;

    MRP_UNUSED(shareable_create);

    type = t & MRP_CSON_TYPE_MASK;
    bln  = t & MRP_CSON_TYPE_BOOLEAN;
    mode = t & (MRP_CSON_TYPE_COMPACT | MRP_CSON_TYPE_SHARABLE);

    if (!mode)
        mode = default_mode;

    if (bln) {
        if (type)
            goto invalid_type;
        else
            type = MRP_CSON_TYPE_BOOLEAN;
    }

    va_start(ap, t);

    if (compactable_type(type) && mode != MRP_CSON_TYPE_SHARABLE)
        o = compact_createv(type, &ap);
    else
        o = shareable_createv(type, &ap);

    va_end(ap);

    return o;

 invalid_type:
    errno = EINVAL;
    return NULL;
}


mrp_cson_type_t mrp_cson_get_type(mrp_cson_t *o)
{
    return compact_object(o) ? compact_type(o) : shareable_type(o);
}


mrp_cson_t *mrp_cson_ref(mrp_cson_t *o)
{
    return compact_object(o) ? compact_ref(o) : shareable_ref(o);
}


int mrp_cson_unref(mrp_cson_t *o)
{
    return compact_object(o) ? compact_unref(o) : shareable_unref(o);
}


int mrp_cson_set(mrp_cson_t *o, const char *name, mrp_cson_t *v)
{
    if (mrp_cson_get_type(o) == MRP_CSON_TYPE_OBJECT)
        return shareable_set(o, name, v);
    else {
        errno = EINVAL;
        return -1;
    }
}


int mrp_cson_del(mrp_cson_t *o, const char *name)
{
    return shareable_del(o, name);
}


mrp_cson_t *mrp_cson_get(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m = shareable_get(o, name);

    return m ? m->value : NULL;
}


const char *mrp_cson_string_value(mrp_cson_t *o, char *buf, size_t size)
{
    mrp_cson_type_t  type;
    const char      *str;
    int64_t          i;
    uint64_t         u;

    switch ((type = mrp_cson_get_type(o))) {
    case MRP_CSON_TYPE_NULL:
        if (buf != NULL) {
            if (size < 5)
                goto overflow;
            strcpy(buf, "null");
        }
        else
            buf = "null";

        return buf;

    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE: {
        str = (type == MRP_CSON_TYPE_FALSE ? "false" : "true");
        if (buf != NULL && size > 0) {
            if (size < (type == MRP_CSON_TYPE_FALSE ? 6 : 5))
                goto overflow;
            strcpy(buf, str);
        }
        else
            buf = (char *)str;

        return buf;
    }

    case MRP_CSON_TYPE_STRING:
        if (compact_object(o))
            str = (const char *)compact_value(o);
        else
            str = o->str;

        if (buf != NULL) {
            if (snprintf(buf, size, "%s", str) >= (int)size)
                goto overflow;
        }
        else
            buf = (char *)str;

        return buf;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        if (buf == NULL)
            goto nobuf;
        if (compact_object(o))
            i = (int64_t)compact_value(o);
        else
            i = o->i;
        if (snprintf(buf, size, "%lld", (long long)i) >= (int)size)
            goto overflow;

        return buf;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (buf == NULL)
            goto nobuf;
        if (compact_object(o))
            u = (uint64_t)compact_value(o);
        else
            u = o->u;
        if (snprintf(buf, size, "%llu", (unsigned long long)u) >= (int)size)
            goto overflow;

        return buf;

    case MRP_CSON_TYPE_DOUBLE:
        if (buf == NULL)
            goto nobuf;
        if (snprintf(buf, size, "%f", o->dbl) >= (int)size)
            goto overflow;

        return buf;

    default:
        if (buf != NULL && size > 0)
            *buf = '\0';
        else
            buf = "";

        return buf;
    }

 nobuf:
    errno = ENOBUFS;
    return NULL;

 overflow:
    errno = EOVERFLOW;
    return NULL;
}


int mrp_cson_boolean_value(mrp_cson_t *o)
{
    return mrp_cson_integer_value(o) != 0;
}


int mrp_cson_integer_value(mrp_cson_t *o)
{
    int64_t i = mrp_cson_int64_value(o);

    if (i < INT_MIN)
        i = INT_MIN;
    else if (i > INT_MAX)
        i = INT_MAX;

    return (int)i;
}


double mrp_cson_double_value(mrp_cson_t *o)
{
    const char *str;
    char       *e;
    double      dbl;
    int64_t     i;
    uint64_t    u;

    switch (mrp_cson_get_type(o)) {
    case MRP_CSON_TYPE_NULL:
        return 0.0;

    case MRP_CSON_TYPE_FALSE:
        return 0.0;
    case MRP_CSON_TYPE_TRUE:
        return 1.0;

    case MRP_CSON_TYPE_STRING:
        if (compact_object(o))
            str = (const char *)compact_value(o);
        else
            str = o->str;

        dbl = strtod(str, &e);

        if (e && *e)
            goto invalid_value;

        return dbl;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        if (compact_object(o))
            i = (int64_t)compact_value(o);
        else
            i = o->i;

        return (double)i;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (compact_object(o))
            u = (uint64_t)compact_value(o);
        else
            u = o->u;

        return (double)u;

    case MRP_CSON_TYPE_DOUBLE:
        return o->dbl;

    default:
        return 0.0;
    }

 invalid_value:
    errno = EINVAL;
    return -1.0;
}


int8_t mrp_cson_int8_value(mrp_cson_t *o)
{
    int64_t i = mrp_cson_int64_value(o);

    if (i < INT8_MIN)
        i = INT8_MIN;
    else if (i > INT8_MAX)
        i = INT8_MAX;

    return (int8_t)i;
}


int16_t mrp_cson_int16_value(mrp_cson_t *o)
{
    int64_t i = mrp_cson_int64_value(o);

    if (i < INT16_MIN)
        i = INT16_MIN;
    else if (i > INT16_MAX)
        i = INT16_MAX;

    return (int16_t)i;
}


int32_t mrp_cson_int32_value(mrp_cson_t *o)
{
    int64_t i = mrp_cson_int64_value(o);

    if (i < INT32_MIN)
        i = INT32_MIN;
    else if (i > INT32_MAX)
        i = INT32_MAX;

    return (int32_t)i;
}


int64_t mrp_cson_int64_value(mrp_cson_t *o)
{
    const char *str;
    char       *e;
    int64_t     i;
    uint64_t    u;

    switch (mrp_cson_get_type(o)) {
    case MRP_CSON_TYPE_NULL:
        return 0;

    case MRP_CSON_TYPE_FALSE:
        return 0;
    case MRP_CSON_TYPE_TRUE:
        return 1;

    case MRP_CSON_TYPE_STRING:
        if (compact_object(o))
            str = (const char *)compact_value(o);
        else
            str = o->str;

        i = (long long)strtol(str, &e, 10);

        if (e && *e)
            goto invalid_value;

        return i;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        if (compact_object(o))
            i = (int64_t)compact_value(o);
        else
            i = o->i;

        return i;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (compact_object(o))
            u = (uint64_t)compact_value(o);
        else
            u = o->u;

        if (u > LLONG_MAX)
            u = LLONG_MAX;

        return (int64_t)u;

    case MRP_CSON_TYPE_DOUBLE:
        if (o->dbl < LLONG_MIN)
            i = LLONG_MIN;
        else if (o->dbl > LLONG_MAX)
            i = LLONG_MAX;
        else
            i = (int64_t)o->dbl;

        return i;

    default:
        return 0;
    }

 invalid_value:
    errno = EINVAL;
    return -1;
}


uint8_t mrp_cson_uint8_value(mrp_cson_t *o)
{
    uint64_t u = mrp_cson_uint64_value(o);

    if (u > UINT8_MAX)
        u = UINT8_MAX;

    return (uint8_t)u;
}


uint16_t mrp_cson_uint16_value(mrp_cson_t *o)
{
    uint64_t u = mrp_cson_uint64_value(o);

    if (u > UINT16_MAX)
        u = UINT16_MAX;

    return (uint16_t)u;
}


uint32_t mrp_cson_uint32_value(mrp_cson_t *o)
{
    uint64_t u = mrp_cson_int64_value(o);

    if (u > UINT32_MAX)
        u = UINT32_MAX;

    return (uint32_t)u;
}


uint64_t mrp_cson_uint64_value(mrp_cson_t *o)
{
    const char *str;
    char       *e;
    int64_t     i;
    uint64_t    u;

    switch (mrp_cson_get_type(o)) {
    case MRP_CSON_TYPE_NULL:
        return 0;

    case MRP_CSON_TYPE_FALSE:
        return 0;
    case MRP_CSON_TYPE_TRUE:
        return 1;

    case MRP_CSON_TYPE_STRING:
        if (compact_object(o))
            str = (const char *)compact_value(o);
        else
            str = o->str;

        i = (long long)strtol(str, &e, 10);

        if (e && *e)
            goto invalid_value;

        return i;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        if (compact_object(o))
            i = (int64_t)compact_value(o);
        else
            i = o->i;

        if (i < 0)
            i = 0;

        return (uint64_t)i;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (compact_object(o))
            u = (uint64_t)compact_value(o);
        else
            u = o->u;

        return u;

    case MRP_CSON_TYPE_DOUBLE:
        if (o->dbl < 0)
            u = 0;
        else if (o->dbl > ULLONG_MAX)
            u = ULLONG_MAX;
        else
            u = (uint64_t)o->dbl;

        return u;

    default:
        return 0;
    }

 invalid_value:
    errno = EINVAL;
    return -1;
}




#if 0

int mrp_cson_set_string(mrp_cson_t *o, const char *name, const char *str)
{
    mrp_cson_t *v;

    if ((v = mrp_cson_create(MRP_CSON_TYPE_STRING, str)) == NULL)
        return -1;

    if (shareable_set(o, name, v) < 0) {
        mrp_cson_unref(v);
        return -1;
    }

    return 0;
}


const char *mrp_cson_get_string(mrp_cson_t *o, const char *name)
{
    mrp_cson_t *v = mrp_cson_get(o, name);

    if (v == NULL)
        return NULL;

    if (mrp_cson_get_type(v) != MRP_CSON_TYPE_STRING)
        goto invalid_type;

    if (compact_object(v))
        return (const char *)compact_value(v);
    else
        return v->str;

 invalid_type:
    errno = EINVAL;
    return NULL;
}


int mrp_cson_set_boolean(mrp_cson_t *o, const char *name, int bln)
{
    mrp_cson_t *v;

    if ((v = mrp_cson_create(MRP_CSON_TYPE_BOOLEAN, bln)) == NULL)
        return -1;

    if (shareable_set(o, name, v) < 0) {
        mrp_cson_unref(v);
        return -1;
    }

    return 0;
}


int mrp_cson_get_boolean(mrp_cson_t *o, const char *name)
{
    mrp_cson_t *v = mrp_cson_get(o, name);
    const char *str;
    int64_t     i;
    uint64_t    u;

    if (v == NULL)
        return -1;

    switch (mrp_cson_get_type(v)) {
    case MRP_CSON_TYPE_NULL:
        return 0;

    case MRP_CSON_TYPE_FALSE:
        return 0;
    case MRP_CSON_TYPE_TRUE:
        return 1;

    case MRP_CSON_TYPE_STRING:
        if (compact_object(v))
            str = (const char *)compact_value(v);
        else
            str = v->str;

        return str != NULL && *str != '\0';

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
    case MRP_CSON_TYPE_INT64:
        if (compact_object(v))
            i = (int64_t)compact_value(v);
        else
            i = v->i;

        return i != 0;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (compact_object(v))
            u = (uint64_t)compact_value(v) != 0;
        else
            u = v->u;

        return u != 0;

    case MRP_CSON_TYPE_DOUBLE:
        return v->dbl != 0.0;

    case MRP_CSON_TYPE_OBJECT:
        return !mrp_list_empty(&v->object.members);

    case MRP_CSON_TYPE_ARRAY:
        return v->array.nitem != 0;

    default:
        return 0;
    }
}


int mrp_cson_set_integer(mrp_cson_t *o, const char *name, int i)
{
    mrp_cson_t *v;

    if ((v = mrp_cson_create(MRP_CSON_TYPE_INTEGER, i)) == NULL)
        return -1;

    if (shareable_set(o, name, v) < 0) {
        mrp_cson_unref(v);
        return -1;
    }

    return 0;
}


int mrp_cson_get_integer(mrp_cson_t *o, const char *name)
{
    mrp_cson_t *v = mrp_cson_get(o, name);
    const char *str;
    char       *e;
    int         i;
    int64_t     i64;
    uint64_t    u;

    if (v == NULL)
        return -1;

    switch (mrp_cson_get_type(v)) {
    case MRP_CSON_TYPE_NULL:
        return 0;

    case MRP_CSON_TYPE_FALSE:
        return 0;
    case MRP_CSON_TYPE_TRUE:
        return 1;

    case MRP_CSON_TYPE_STRING:
        if (compact_object(v))
            str = (const char *)compact_value(v);
        else
            str = v->str;

        i = strtol(str, &e, 10);

        if (e && *e) {
            errno = EINVAL;
            i = -1;
        }

        return i;

    case MRP_CSON_TYPE_INTEGER:
    case MRP_CSON_TYPE_INT8:
    case MRP_CSON_TYPE_INT16:
    case MRP_CSON_TYPE_INT32:
        if (compact_object(v))
            i = (int)compact_value(v);
        else
            i = (int)v->i;

        return i;

    case MRP_CSON_TYPE_INT64:
        if (compact_object(v))
            i64 = (int64_t)compact_value(v);
        else
            i64 = (int64_t)v->i;

        if (i64 < INT_MIN)
            i = INT_MIN;
        else if (i64 > INT_MAX)
            i = INT_MAX;
        else
            i = (int)i64;

        return i;

    case MRP_CSON_TYPE_UINT8:
    case MRP_CSON_TYPE_UINT16:
    case MRP_CSON_TYPE_UINT32:
    case MRP_CSON_TYPE_UINT64:
        if (compact_object(v))
            u = (uint64_t)compact_value(v);
        else
            u = v->u;

        if (u > INT_MAX)
            i = INT_MAX;
        else
            i = (int)u;

        return i;

    case MRP_CSON_TYPE_DOUBLE:
        if (v->dbl > INT_MAX)
            i = INT_MAX;
        else if (v->dbl < INT_MIN)
            i = INT_MIN;
        else
            i = (int)v->dbl;

        return i;

    default:
        errno = EINVAL;
        return -1;
    }
}


int mrp_cson_set_int8(mrp_cson_t *o, const char *name, int8_t i)
{
    mrp_cson_t *v;

    if ((v = mrp_cson_create(MRP_CSON_TYPE_INT8, i)) == NULL)
        return -1;

    if (shareable_set(o, name, v) < 0) {
        mrp_cson_unref(v);
        return -1;
    }

    return 0;
}


#endif


static int cson_printf_p(FILE *fp, const struct printf_info *info,
                         const void *const *args)
{
    const mrp_cson_t *o;
    char             *buf = NULL;
    int               n   = 0;

    if (!(info->user & mod_cson))
        return -2;

    o = *((const mrp_cson_t **)args[0]);

    /* ugh... horribly inefficient, we print twice, first to calculate size */
    if (compact_object(o)) {
        if (info->is_long || info->alt) {
            n   = compact_print_pretty(buf, n, (mrp_cson_t *)o, 0);
            buf = alloca(n + 1);
            n   = compact_print_pretty(buf, n + 1, (mrp_cson_t *)o, 0);
        }
        else {
            n   = compact_print(buf, n, (mrp_cson_t *)o);
            buf = alloca(n + 1);
            n   = compact_print(buf, n + 1, (mrp_cson_t *)o);
        }
    }
    else {
        if (info->is_long || info->alt) {
            n   = shareable_print_pretty(buf, n, (mrp_cson_t *)o, 0);
            buf = alloca(n + 1);
            n   = shareable_print_pretty(buf, n + 1, (mrp_cson_t *)o, 0);
        }
        else {
            n   = shareable_print(buf, n, (mrp_cson_t *)o);
            buf = alloca(n + 1);
            n   = shareable_print(buf, n + 1, (mrp_cson_t *)o);
        }
    }

    n = fprintf(fp, "%s", buf);

    return n;
}


static int cson_ais(const struct printf_info *info, size_t n,
                    int *argtype, int *size)
{
    MRP_UNUSED(n);

    if (!(info->user & mod_cson))
        return -1;

    argtype[0] = PA_POINTER;
    size[0]    = sizeof(mrp_cson_t *);

    return 1;
}


void MRP_INIT cson_init(void)
{
    /*
     * register a printf CSON modifier and printf-function
     *
     * We register a printf modifier flag ("CSON") and we override
     * the printf handler function for "%p". Our handler tells the
     * caller it cannot handle the call if the CSON modifier was not
     * specified, letting the default implementation take over.
     *
     * These allow us to print or pretty print CSON objects using the
     * stock C library printf family of functions like this:
     *
     * int print_cson(FILE *fp, mrp_cson_t *o, int pretty)
     * {
     *     int n;
     *
     *     if (pretty)
     *         n = fprintf(fp, "pretty-CSON: %#CSONp\n", o);
     *     else
     *         n = fprintf(fp, "CSON: %CSONp\n", o);
     *
     *     return n;
     * }
     */

    mod_cson = register_printf_modifier(L"CSON");
    register_printf_specifier('p', cson_printf_p, cson_ais);

    /*
     * Create symbol table for JSON member names and set the default
     * mode to compact. Unless the default mode is changed to shareable,
     * JSON objects that are not explicitly forced to shareable mode
     * will be created in compact mode whenever possible.
     */
    symtbl_create();
    default_mode = MRP_CSON_TYPE_SHARABLE;
}
