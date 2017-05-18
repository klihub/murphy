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
#include <errno.h>
#include <stdarg.h>
#include <printf.h>


#include <murphy/common/macros.h>
#include <murphy/common/mm.h>
#include <murphy/common/debug.h>
#include <murphy/common/list.h>
#include <murphy/common/refcnt.h>
#include <murphy/common/hash-table.h>
#include <murphy/common/cson.h>

typedef struct mrp_cson_member_s mrp_cson_member_t;
typedef struct cson_sym_s        cson_sym_t;

/* a JSON member name */
struct cson_sym_s {
    char         *name;                      /* string (member name) */
    uint32_t      id;                        /* string id (cookie) */
    uint32_t      hash;                      /* string hash value */
    mrp_refcnt_t  refcnt;                    /* active references */
};

/* a JSON object member */
struct mrp_cson_member_s {
    uint32_t         id;                     /* member name */
    mrp_cson_t      *value;                  /* member value */
    mrp_list_hook_t  hook;                   /* to parent object */
};

/* a JSON object */
struct mrp_cson_s {
    mrp_cson_type_t type;                    /* MRP_CSON_TYPE_* */
    mrp_refcnt_t    refcnt;                  /* reference count */
    union {                                  /* type-specific value */
        char     *str;                       /* _STRING */
        int       i;                         /* _INTEGER */
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

        int8_t    s8;                        /* _INT8 */
        uint8_t   u8;                        /* _UINT8 */
        int16_t   s16;                       /* _INT16 */
        uint16_t  u16;                       /* _UINT16 */
        int32_t   s32;                       /* _INT32 */
        uint32_t  u32;                       /* _UINT32 */
        int64_t   s64;                       /* _INT64 */
        uint64_t  u64;                       /* _UINT64 */
    };
};


static mrp_hashtbl_t *symtbl;                /* symbol (member name) table */
static int            expect_all;            /* expect/lock all member names */
static int            default_mode;          /* compact vs. sparse default */
static int            mod_cson;              /* printf CSON %p modifier flag */

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


static inline mrp_hashtbl_t *symtbl_get(void)
{
    return symtbl;
}


static inline cson_sym_t *symbol_ref(cson_sym_t *sym)
{
    return mrp_ref_obj(sym, refcnt);
}


static inline int symbol_unref(cson_sym_t *sym)
{
    return mrp_unref_obj(sym, refcnt);
}


static void symbol_free(void *key, void *obj)
{
    cson_sym_t *sym = obj;

    MRP_UNUSED(key);

    if (sym == NULL)
        return;

    mrp_free(sym->name);
    mrp_free(sym);
}


static cson_sym_t *symbol_get(const char *str, int add)
{
    mrp_hashtbl_t *ht  = symtbl_get();
    cson_sym_t    *sym;
    uint32_t      cookie;

    sym = mrp_hashtbl_lookup(ht, str, MRP_HASH_COOKIE_NONE);

    if (sym != NULL)
        return add ? symbol_ref(sym) : sym;

    if (!add)
        return NULL;

    if ((sym = mrp_allocz(sizeof(*sym))) == NULL ||
        (sym->name = mrp_strdup(str)) == NULL)
        goto nomem;

    cookie = MRP_HASH_COOKIE_NONE;
    if (mrp_hashtbl_add(ht, sym->name, sym, &cookie) < 0)
        goto fail;

    mrp_refcnt_init(&sym->refcnt);
    sym->id   = cookie;
    sym->hash = (1 << ((cookie - 1) % (8 * sizeof(sym->hash))));

    if (expect_all)
        symbol_ref(sym);

    mrp_debug("added symbol '%s' (id: %u, hash: %u)", str, sym->id, sym->hash);

    return sym;

 fail:
    mrp_free(sym->name);
 nomem:
    mrp_free(sym);
    return NULL;
}


static cson_sym_t *symbol_lookup(uint32_t id)
{
    mrp_hashtbl_t *ht  = symtbl;
    cson_sym_t    *sym = mrp_hashtbl_fetch(ht, id);

    return sym;
}


static inline uint32_t symbol_id(const char *name, int add)
{
    cson_sym_t *sym = symbol_get(name, add);

    return sym ? sym->id : 0; /* MRP_HASH_COOKIE_NONE */
}



static const char *symbol_str(uint32_t id)
{
    mrp_hashtbl_t *ht  = symtbl;
    cson_sym_t    *sym = mrp_hashtbl_fetch(ht, id);

    return sym ? sym->name : "<unknow-symbol-id>";
}


int mrp_cson_expect_name(const char *name)
{
    if (name == MRP_CSON_ALL_NAMES) {
        /* XXX hmm... should we extra ref all existing symbols here ? */
        expect_all++;
        return 0;
    }
    else {
        cson_sym_t *sym = symbol_get(name, TRUE);
        return sym != NULL ? 0 : -1;
    }
}


void mrp_cson_forget_name(const char *name)
{
    if (name == MRP_CSON_ALL_NAMES) {
        if (--expect_all < 0) {
            mrp_log_error("imbalanced calls to "                        \
                          "mrp_cson_{expect,forget}_name(MRP_CSON_ALL_NAMES)");
            expect_all = 0;
        }
        else {
            /* XXX hmm... should we unref all existing symbols here ? */;
        }
    }
    else
        symbol_unref(symbol_get(name, FALSE));
}


mrp_cson_t *mrp_cson_create(mrp_cson_type_t type, ...)
{
    mrp_cson_t *o;
    va_list     ap;
    const char *str;

    o = mrp_allocz(sizeof(*o));

    if (o == NULL)
        goto nomem;

    o->type = type;
    mrp_refcnt_init(&o->refcnt);

    va_start(ap, type);

    switch (type) {
    case MRP_CSON_TYPE_UNKNOWN:
        break;

    case MRP_CSON_TYPE_STRING:
        o->str = mrp_strdup(str = va_arg(ap, const char *));

        if (str != NULL && o->str == NULL)
            goto nomem;
        break;

    case MRP_CSON_TYPE_FALSE:
    case MRP_CSON_TYPE_TRUE:
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        o->type = MRP_CSON_TYPE_FALSE + (va_arg(ap, int) ? 1 : 0);
        break;

    case MRP_CSON_TYPE_INTEGER:
        o->i = va_arg(ap, int);
        break;

    case MRP_CSON_TYPE_INT8:
        o->s8 = (int8_t)va_arg(ap, int);
        break;
    case MRP_CSON_TYPE_UINT8:
        o->u8 = (uint8_t)va_arg(ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT16:
        o->s16 = (int16_t)va_arg(ap, int);
        break;
    case MRP_CSON_TYPE_UINT16:
        o->u16 = (uint16_t)va_arg(ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT32:
        o->s32 = (int32_t)va_arg(ap, int);
        break;
    case MRP_CSON_TYPE_UINT32:
        o->u32 = (uint32_t)va_arg(ap, unsigned int);
        break;

    case MRP_CSON_TYPE_INT64:
        o->s64 = (int64_t)va_arg(ap, int64_t);
        break;
    case MRP_CSON_TYPE_UINT64:
        o->u64 = (uint64_t)va_arg(ap, uint64_t);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        o->dbl = va_arg(ap, double);
        break;

    case MRP_CSON_TYPE_OBJECT:
        mrp_list_init(&o->object.members);
        break;

    case MRP_CSON_TYPE_ARRAY:
        break;

    case MRP_CSON_TYPE_NULL:
        break;
    }

    return o;

 nomem:
    mrp_free(o);
    return NULL;
}


static void cson_free(mrp_cson_t *o)
{
#if 0
    mrp_list_hook_t *p, *n;
    mrp_cson_t      *m;
#endif

    if (o == NULL)
        return;

    switch (o->type) {
    case MRP_CSON_TYPE_STRING:
        mrp_free(o->str);
        break;

    case MRP_CSON_TYPE_OBJECT:
#if 0
        mrp_list_foreach(&o->object.>members, p, n) {
            m = mrp_list_entry(p, mrp_cson_t, hook);
            mrp_list_delete(&m->hook);
            cson_free(m);
        }
#endif
        break;

    case MRP_CSON_TYPE_ARRAY:
        break;

    default:
        break;
    }

    o->type = MRP_CSON_TYPE_UNKNOWN;
    mrp_free(o);
}


mrp_cson_type_t mrp_cson_get_type(mrp_cson_t *o)
{
    return o ? o->type : MRP_CSON_TYPE_NULL;
}


mrp_cson_t *mrp_cson_ref(mrp_cson_t *o)
{
    return mrp_ref_obj(o, refcnt);
}


int mrp_cson_unref(mrp_cson_t *o)
{
    int last = mrp_unref_obj(o, refcnt);

    if (last)
        cson_free(o);

    return last;
}


static mrp_cson_member_t *get_member(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m;
    mrp_list_hook_t   *p, *n;
    cson_sym_t        *sym;

    if (o == NULL)
        return NULL;

    if (o->type != MRP_CSON_TYPE_OBJECT)
        goto invalid;

    if ((sym = symbol_get(name, FALSE)) == NULL)
        return NULL;

    if ((o->object.blmmask & sym->hash) != sym->hash)
        return NULL;

    mrp_list_foreach(&o->object.members, p, n) {
        m = mrp_list_entry(p, mrp_cson_member_t, hook);

        if (m->id == sym->id)
            return m;
    }

    errno = ENOENT;
    return NULL;

 invalid:
    errno = EINVAL;
    return NULL;
}


int mrp_cson_set(mrp_cson_t *o, const char *name, mrp_cson_t *v)
{
    mrp_cson_member_t *m = NULL;
    cson_sym_t        *sym;

    if (o->type != MRP_CSON_TYPE_OBJECT)
        goto invalid;

    m = get_member(o, name);

    if (m != NULL) {
        mrp_cson_unref(m->value);
        m->value = v;

        return 0;
    }

    m = mrp_allocz(sizeof(*m));

    if (m == NULL)
        goto nomem;

    mrp_list_init(&m->hook);

    if ((sym = symbol_get(name, TRUE)) == NULL)
        goto nomem;

    o->object.blmmask |= sym->hash;

    m->id = sym->id;
    m->value = v;

    mrp_list_append(&o->object.members, &m->hook);

    return 0;

 invalid:
    errno = EINVAL;
 nomem:
    mrp_free(m);
    return -1;
}


mrp_cson_t *mrp_cson_get(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m = get_member(o, name);

    return (m != NULL ? m->value : NULL);
}


void mrp_cson_del(mrp_cson_t *o, const char *name)
{
    mrp_cson_member_t *m = get_member(o, name);
    cson_sym_t        *sym;

    if (m == NULL)
        return;

    sym = symbol_lookup(m->id);

    MRP_ASSERT(sym != NULL, "member without an existing symbol");

    mrp_list_delete(&m->hook);
    mrp_cson_unref(m->value);
    symbol_unref(sym);
    mrp_free(m);
}


static int cson_print_pretty(char *buf, size_t size, mrp_cson_t *o, int lvl)
{
    int l, c;

    MRP_UNUSED(lvl);

    if (o == NULL)
        return 0;

    l = 0;

    switch (o->type) {
    case MRP_CSON_TYPE_UNKNOWN:
        l = snprintf(buf, size, "<JSON, unknown type>\n");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "<'%s'>\n", o->str);
        break;


    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "<false>\n");
        break;

    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "<true>\n");
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        l = snprintf(buf, size, "<bool:?>\n");
        break;

    case MRP_CSON_TYPE_INTEGER:
        l = snprintf(buf, size, "%d\n", o->i);
        break;

    case MRP_CSON_TYPE_INT8:
        l = snprintf(buf, size, "%d\n", (int)o->s8);
        break;
    case MRP_CSON_TYPE_UINT8:
        l = snprintf(buf, size, "%u\n", (unsigned int)o->u8);
        break;

    case MRP_CSON_TYPE_INT16:
        l = snprintf(buf, size, "%d\n", (int)o->s16);
        break;
    case MRP_CSON_TYPE_UINT16:
        l = snprintf(buf, size, "%u\n", (unsigned int)o->u16);
        break;

    case MRP_CSON_TYPE_INT32:
        l = snprintf(buf, size, "%d\n", (int)o->s32);
        break;
    case MRP_CSON_TYPE_UINT32:
        l = snprintf(buf, size, "%u\n", (unsigned int)o->u32);
        break;

    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%ld\n", o->s64);
        break;
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%lu\n", o->u64);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        l = snprintf(buf, size, "%f\n", o->dbl);
        break;

    case MRP_CSON_TYPE_OBJECT: {
        mrp_cson_member_t *m;
        mrp_list_hook_t   *p, *n;

        c    = snprintf(buf, size, "{\n");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);

        mrp_list_foreach(&o->object.members, p, n) {
            m = mrp_list_entry(p, mrp_cson_member_t, hook);

            c    = snprintf(buf, size, "%s: %#CSONp\n",
                            symbol_str(m->id), m->value);
            l   += c;
            buf += c;
            size = (c > (int)size ? 0 : size - c);
        }

        c    = snprintf(buf, size, "}\n");
        l   += c;
        buf += c;
        size = (c > (int)size ? 0 : size - c);
    }
        break;

    case MRP_CSON_TYPE_ARRAY:
        l = snprintf(buf, size, "[ ]\n");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "<NULL>\n");
        break;

    }

    return l;
}


static int cson_print_compact(char *buf, size_t size, mrp_cson_t *o)
{
    int l;

    if (o == NULL)
        return 0;

    l = 0;

    switch (o->type) {
    case MRP_CSON_TYPE_UNKNOWN:
        l = snprintf(buf, size, "<JSON, unknown type>");
        break;

    case MRP_CSON_TYPE_STRING:
        l = snprintf(buf, size, "'%s'", o->str);
        break;

    case MRP_CSON_TYPE_FALSE:
        l = snprintf(buf, size, "false");
        break;

    case MRP_CSON_TYPE_TRUE:
        l = snprintf(buf, size, "true");
        break;

    case MRP_CSON_TYPE_BOOLEAN:
        l = snprintf(buf, size, "bool:?");
        break;

    case MRP_CSON_TYPE_INTEGER:
        l = snprintf(buf, size, "%d", o->i);
        break;

    case MRP_CSON_TYPE_INT8:
        l = snprintf(buf, size, "%d", (int)o->s8);
        break;
    case MRP_CSON_TYPE_UINT8:
        l = snprintf(buf, size, "%u", (unsigned int)o->u8);
        break;

    case MRP_CSON_TYPE_INT16:
        l = snprintf(buf, size, "%d", (int)o->s16);
        break;
    case MRP_CSON_TYPE_UINT16:
        l = snprintf(buf, size, "%u", (unsigned int)o->u16);
        break;

    case MRP_CSON_TYPE_INT32:
        l = snprintf(buf, size, "%d", (int)o->s32);
        break;
    case MRP_CSON_TYPE_UINT32:
        l = snprintf(buf, size, "%u", (unsigned int)o->u32);
        break;

    case MRP_CSON_TYPE_INT64:
        l = snprintf(buf, size, "%ld", o->s64);
        break;
    case MRP_CSON_TYPE_UINT64:
        l = snprintf(buf, size, "%lu", o->u64);
        break;

    case MRP_CSON_TYPE_DOUBLE:
        l = snprintf(buf, size, "%f", o->dbl);
        break;

    case MRP_CSON_TYPE_OBJECT:
        l = snprintf(buf, size, "{}");
        break;

    case MRP_CSON_TYPE_ARRAY:
        l = snprintf(buf, size, "[]");
        break;

    case MRP_CSON_TYPE_NULL:
        l = snprintf(buf, size, "<NULL>");
        break;
    }

    return l;

}

static int cson_printf_p(FILE *fp, const struct printf_info *info,
                         const void *const *args)
{
    const mrp_cson_t *o;
    char             *buf = NULL;
    int               n   = 0;

    if (!(info->user & mod_cson))
        return -2;

    o = *((const mrp_cson_t **)args[0]);

    /* horribly inefficient, we print twice, first to calculate size */
    if (info->is_long || info->alt) {
        n   = cson_print_pretty(buf, n, (mrp_cson_t *)o, 0);
        buf = alloca(n + 1);
        n   = cson_print_pretty(buf, n + 1, (mrp_cson_t *)o, 0);
    }
    else {
        n   = cson_print_compact(buf, n, (mrp_cson_t *)o);
        buf = alloca(n + 1);
        n   = cson_print_compact(buf, n + 1, (mrp_cson_t *)o);
    }

    n = fprintf(fp, "%s", buf);

    return n;
}


static int cson_ais(const struct printf_info *info, size_t n,
                    int *argtype, int *size)
{
    MRP_UNUSED(info);
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

    symtbl_create();
}
