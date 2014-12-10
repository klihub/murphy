/*
 * Copyright (c) 2012, Intel Corporation
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
 *   * Neither the name of Intel Corporation nor the names of its contributors
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

#include <errno.h>
#include <stdarg.h>

#include <murphy/common/debug.h>
#include <murphy/common/mm.h>
#include <murphy/common/hashtbl.h>
#include <murphy/common/utils.h>

#include <murphy-db/mqi.h>
#include <murphy-db/mql.h>
#include <murphy-db/mdb.h>

#include "domain-control.h"
#include "proxy.h"
#include "table.h"

#define FAIL(ec, msg) do {                      \
        *errcode = ec;                          \
        *errmsg = msg;                          \
        goto fail;                              \
    } while (0)


static int instantiate_wildcard_watches(pdp_t *pdp, const char *name);


/*
 * proxied and tracked tables
 */


static void table_change_cb(mqi_event_t *e, void *tptr)
{
    static const char *events[] = {
        "unknown (?)",
        "column change",
        "row insert",
        "row delete",
        "table create",
        "table drop",
        "transaction start (?)",
        "transaction end (?)",
    };
    pep_table_t *t = (pep_table_t *)tptr;

    if (!t->changed) {
        t->changed = true;
        mrp_debug("table '%s' changed by %s event", t->name, events[e->event]);
    }


}


static int add_table_triggers(pep_table_t *t)
{
    mdb_table_t      *tbl;
    mqi_column_def_t  cols[256];
    int               ncol, i;

    if (t->h == MQI_HANDLE_INVALID) {
        errno = EAGAIN;
        return -1;
    }

    if ((tbl = mdb_table_find(t->name)) == NULL) {
        errno = EINVAL;
        return -1;
    }

    if ((ncol = mdb_table_describe(tbl, &cols[0], sizeof(cols))) <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (mdb_trigger_add_row_callback(tbl, table_change_cb, t, NULL)) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < ncol; i++) {
        if (mdb_trigger_add_column_callback(tbl, i, table_change_cb,
                                            t, NULL) < 0) {
            mdb_trigger_delete_row_callback(tbl, table_change_cb, t);
            errno = EINVAL;
            return -1;
        }
    }

    return 0;
}


static void del_table_triggers(pep_table_t *t)
{
    mdb_table_t      *tbl;
    mqi_column_def_t  cols[256];
    int               ncol, i;

    if (t->h == MQI_HANDLE_INVALID)
        return;

    if ((tbl = mdb_table_find(t->name)) == NULL)
        return;

    ncol = mdb_table_describe(tbl, &cols[0], sizeof(cols));

    mdb_trigger_delete_row_callback(tbl, table_change_cb, t);

    for (i = 0; i < ncol; i++)
        mdb_trigger_delete_column_callback(tbl, i, table_change_cb, t);
}


static void table_event_cb(mqi_event_t *e, void *user_data)
{
    pdp_t        *pdp  = (pdp_t *)user_data;
    const char   *name = e->table.table.name;
    mqi_handle_t  h    = e->table.table.handle;
    pep_table_t  *t;

    switch (e->event) {
    case mqi_table_created:
        mrp_debug("table %s (0x%x) created", name, h);
        break;

    case mqi_table_dropped:
        mrp_debug("table %s (0x%x) dropped", name, h);
        break;

    default:
        return;
    }

    instantiate_wildcard_watches(pdp, name);

    t = lookup_watch_table(pdp, name);

    if (t != NULL) {
        t->changed = true;

        if (e->event == mqi_table_created) {
            t->h = h;

            if (!t->expid && t->exported)
                t->expid = pdp->expid++;

            introspect_table(t, h);
            add_table_triggers(t);
        }
        else {
            invalidate_table(t);
            del_table_triggers(t);
        }
    }

    schedule_notification(pdp);
}


static void transaction_event_cb(mqi_event_t *e, void *user_data)
{
    pdp_t *pdp   = (pdp_t *)user_data;
    int    depth = e->transact.depth;

    switch (e->event) {
    case mqi_transaction_end:
        if (depth == 1) {
            mrp_debug("outermost transaction ended");

            if (pdp->ractive) {
                mrp_debug("resolver active, delaying client notifications");
                pdp->rblocked = true;
            }
            else
                schedule_notification(pdp);
        }
        else
            mrp_debug("nested transaction (#%d) ended", depth);
        break;

    case mqi_transaction_start:
        if (depth == 1)
            mrp_debug("outermost transaction started");
        else
            mrp_debug("nested transaction (#%d) started", depth);
        break;

    default:
        break;
    }
}


static int open_db(pdp_t *pdp)
{
    static bool done = false;

    if (done)
        return TRUE;

    if (mqi_open() == 0) {
        if (mqi_create_transaction_trigger(transaction_event_cb, pdp) == 0 &&
            mqi_create_table_trigger(table_event_cb, pdp) == 0) {
            done = true;
            return TRUE;
        }

        mqi_drop_transaction_trigger(transaction_event_cb, pdp);
    }

    return FALSE;
}


static void close_db(pdp_t *pdp)
{
    mqi_drop_table_trigger(table_event_cb, pdp);
    mqi_drop_transaction_trigger(transaction_event_cb, pdp);
}


static void purge_watch_table_cb(void *key, void *entry);



int init_tables(pdp_t *pdp)
{
    mrp_htbl_config_t hcfg;

    if (open_db(pdp)) {
        mrp_list_init(&pdp->tables);
        mrp_list_init(&pdp->wildcard);

        mrp_clear(&hcfg);
        hcfg.comp = mrp_string_comp;
        hcfg.hash = mrp_string_hash;
        hcfg.free = purge_watch_table_cb;

        pdp->watched = mrp_htbl_create(&hcfg);
    }

    return (pdp->watched != NULL);
}


void destroy_tables(pdp_t *pdp)
{
    close_db(pdp);
    mrp_htbl_destroy(pdp->watched, TRUE);

    pdp->watched = NULL;
}


int exec_mql(mql_result_type_t type, mql_result_t **resultp,
             const char *format, ...)
{
    mql_result_t *r;
    char          buf[4096];
    va_list       ap;
    int           success, n;

    va_start(ap, format);
    n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);

    if (n < (int)sizeof(buf)) {
        mrp_debug("executing DB query '%s'", buf);

        r       = mql_exec_string(type, buf);
        success = (r == NULL || mql_result_is_success(r));

        if (resultp != NULL) {
            *resultp = r;
            return success;
        }
        else {
            mql_result_free(r);
            return success;
        }
    }
    else {
        errno = EOVERFLOW;
        if (resultp != NULL)
            *resultp = NULL;

        return FALSE;
    }
}


const char *describe_mql(char *mql, size_t size, mqi_handle_t h, mql_result_t *r)
{
    mqi_column_def_t  defs[MQI_COLUMN_MAX], *d;
    int               ndef, ncol;
    char             *p;
    int               n, l, idx, i;

    if (r == NULL || mql_result_rows_get_row_count(r) < 1)
        return NULL;

    ndef = mqi_describe(h, defs, MRP_ARRAY_SIZE(defs));
    ncol = mql_result_rows_get_row_column_count(r);
    p    = mql;
    l    = (int)size;

    for (i = 0; i < ncol; i++) {
        idx = mql_result_rows_get_row_column_index(r, i);

        if (idx < 0 || idx >= ndef)
            return NULL;

        d = defs + idx;

        switch (d->type) {
        case mqi_varchar:
            n = snprintf(p, l, "%s%s varchar (%d)", i ? ", " : "",
                         d->name, d->length);
            break;
        case mqi_integer:
            n = snprintf(p, l, "%s%s integer", i ? ", " : "", d->name);
            break;

        case mqi_unsignd:
            n = snprintf(p, l, "%s%s unsigned", i ? ", " : "", d->name);
            break;

        case mqi_floating:
            n = snprintf(p, l, "%s%s floating", i ? ", " : "", d->name);
            break;

        default:
            return NULL;
        }

        if (n >= l)
            return NULL;

        p += n;
        l -= n;
    }

    return mql;
}


int introspect_table(pep_table_t *t, mqi_handle_t h)
{
    mqi_column_def_t    cols[MQI_COLUMN_MAX], *c;
    int                 ncol;
    mrp_domctl_value_t *values = NULL;
    char                mql_columns[4096], *p, *s;
    int                 i, n, l;

    if (h == MQI_HANDLE_INVALID)
        h = mqi_get_table_handle((char *)t->name);

    if (h == MQI_HANDLE_INVALID)
        return TRUE;

    t->h = h;

    ncol = mqi_describe(t->h, cols, MRP_ARRAY_SIZE(cols));

    if (ncol <= 0)
        return FALSE;

    if (t->ncolumn != 0 && t->ncolumn != ncol)
        return FALSE;

    if (t->columns == NULL)
        t->columns = mrp_allocz_array(mqi_column_def_t , ncol);

    if (t->coldesc == NULL)
        t->coldesc = mrp_allocz_array(mqi_column_desc_t, ncol + 1);

    if (t->columns == NULL || t->coldesc == NULL)
        return FALSE;

    memcpy(t->columns, cols, ncol * sizeof(*t->columns));
    t->ncolumn = ncol;

    p = mql_columns;
    l = sizeof(mql_columns);
    for (i = 0, c = cols; i < ncol; i++, c++) {
        t->coldesc[i].cindex = i;
        t->coldesc[i].offset = (int)(ptrdiff_t)&values[i].str;

        s = i ? "," : "";
        switch (c->type) {
        case mqi_varchar:
            n = snprintf(p, l, "%s%s varchar (%d)", s, c->name, c->length);
            break;

        case mqi_integer:
            n = snprintf(p, l, "%s%s integer", s, c->name);
            break;

        case mqi_unsignd:
            n = snprintf(p, l, "%s%s unsigned", s, c->name);
            break;

        case mqi_floating:
            n = snprintf(p, l, "%s%s floating", s, c->name);
            break;

        default:
            return FALSE;
        }

        if (n >= l - 1)
            return FALSE;

        p += n;
        l -= n;

        if (c->flags & MQI_COLUMN_KEY) {
            t->idx_col   = i;
            if (t->mql_index == NULL)
                t->mql_index = mrp_strdup(c->name);

            if (t->mql_index == NULL)
                return FALSE;
        }
    }

    *p = '\0';

    t->coldesc[i].cindex = -1;
    if (t->mql_columns == NULL)
        t->mql_columns = mrp_strdup(mql_columns);

    if (t->mql_columns == NULL)
        return FALSE;

    mrp_debug("table %s (handle 0x%x):", t->name, t->h);
    mrp_debug("    columns: %s", t->mql_columns);
    mrp_debug("      index: %s", t->mql_index ? t->mql_index : "<none>");

    return TRUE;
}


void invalidate_table(pep_table_t *t)
{
    mrp_free(t->mql_columns);
    mrp_free(t->mql_index);
    mrp_free(t->columns);
    mrp_free(t->coldesc);

    t->h           = MQI_HANDLE_INVALID;
    t->mql_columns = NULL;
    t->mql_index   = NULL;
    t->columns     = NULL;
    t->coldesc     = NULL;
    t->ncolumn     = 0;
    t->idx_col     = -1;
}


int create_proxy_table(pep_proxy_t *proxy, uint32_t id, const char *name,
                       const char *mql_columns, const char *mql_index,
                       int *errcode, const char **errmsg)
{
    pep_table_t *t = NULL;

    if (find_proxy_table(proxy, name) != NULL)
        FAIL(EEXIST, "table already exists");

    t = mrp_allocz(sizeof(*t));

    if (t == NULL)
        FAIL(ENOMEM, "failed to allocate table");

    mrp_list_init(&t->hook);
    mrp_list_init(&t->watches);

    t->name        = mrp_strdup(name);
    t->mql_columns = mrp_strdup(mql_columns);
    t->mql_index   = mrp_strdup(mql_index);

    if (!t->name || !t->mql_columns || (!t->mql_index && mql_index))
        FAIL(ENOMEM, "failed to allocate table");

    if (mqi_get_table_handle((char *)t->name) != MQI_HANDLE_INVALID)
        FAIL(EEXIST, "DB error: table already exists");

    if (exec_mql(mql_result_dontcare, NULL,
                 "create temporary table %s (%s)", t->name, t->mql_columns)) {
        if (t->mql_index && t->mql_index[0]) {
            if (!exec_mql(mql_result_dontcare, NULL,
                          "create index on %s (%s)", t->name, t->mql_index))
                FAIL(EINVAL, "DB error: failed to create table index");
        }

        if (!introspect_table(t, MQI_HANDLE_INVALID))
            FAIL(EINVAL, "DB error: failed to get table description");

        mrp_list_append(&proxy->tables, &t->hook);
        t->id = id;

        return TRUE;
    }
    else
        FAIL(ENOMEM, "DB error: failed to create table");

 fail:
    if (t != NULL) {
        mrp_free(t->name);
        mrp_free(t->mql_columns);
        mrp_free(t->mql_index);

        mrp_free(t);
    }
    return FALSE;
}


void destroy_proxy_table(pep_table_t *t)
{
    if (t == NULL)
        return;

    mrp_debug("destroying table %s", t->name ? t->name : "<unknown>");

    mrp_list_delete(&t->hook);

    if (t->h != MQI_HANDLE_INVALID)
        mqi_drop_table(t->h);

    mrp_free(t->name);
    mrp_free(t->mql_columns);
    mrp_free(t->mql_index);

    mrp_free(t->columns);
    mrp_free(t->coldesc);

    if (t->wildcard)
        regfree(&t->re);

    mrp_free(t);
}


void destroy_proxy_tables(pep_proxy_t *proxy)
{
    mrp_list_hook_t *p, *n;
    pep_table_t     *t;
    mqi_handle_t     tx;

    mrp_debug("destroying tables of client %s", proxy->name);

    tx = mqi_begin_transaction();
    mrp_list_foreach(&proxy->tables, p, n) {
        t = mrp_list_entry(p, typeof(*t), hook);
        destroy_proxy_table(t);
    }
    mqi_commit_transaction(tx);
}


pep_table_t *create_watch_table(pdp_t *pdp, const char *name)
{
    pep_table_t *t;

    t = mrp_allocz(sizeof(*t));

    if (t != NULL) {
        mrp_list_init(&t->hook);
        mrp_list_init(&t->watches);

        t->h    = MQI_HANDLE_INVALID;
        t->name = mrp_strdup(name);

        if (t->name == NULL)
            goto fail;

        if (wildcard_watch(name)) {
            if (regcomp(&t->re, name, REG_EXTENDED | REG_NOSUB) != 0)
                goto fail;

            t->wildcard = true;

            mrp_list_append(&pdp->wildcard, &t->hook);
        }
        else {
            introspect_table(t, MQI_HANDLE_INVALID);

            if (t->h != MQI_HANDLE_INVALID)
                add_table_triggers(t);

            mrp_list_append(&pdp->tables, &t->hook);

            if (!mrp_htbl_insert(pdp->watched, t->name, t))
                goto fail;
        }
    }

    return t;

 fail:
    destroy_watch_table(pdp, t);

    return FALSE;
}


static void destroy_table_watches(pep_table_t *t)
{
    pep_watch_t     *w;
    mrp_list_hook_t *p, *n;

    if (t != NULL) {
        del_table_triggers(t);

        mrp_list_foreach(&t->watches, p, n) {
            w = mrp_list_entry(p, typeof(*w), tbl_hook);

            mrp_list_delete(&w->tbl_hook);
            mrp_list_delete(&w->pep_hook);

            mrp_free(w->mql_columns);
            mrp_free(w->mql_where);
            mrp_free(w);
        }
    }
}


void destroy_watch_table(pdp_t *pdp, pep_table_t *t)
{
    mrp_list_delete(&t->hook);
    t->h = MQI_HANDLE_INVALID;

    if (pdp != NULL)
        mrp_htbl_remove(pdp->watched, t->name, FALSE);

    destroy_table_watches(t);
}


pep_table_t *lookup_watch_table(pdp_t *pdp, const char *name)
{
    pep_table_t     *t;
    mrp_list_hook_t *p, *n;

    t = mrp_htbl_lookup(pdp->watched, (void *)name);

    if (t != NULL)
        return t;

    if (wildcard_watch(name)) {
        mrp_list_foreach(&pdp->wildcard, p, n) {
            t = mrp_list_entry(p, typeof(*t), hook);

            if (!strcmp(t->name, name))
                return t;
        }
    }

    return NULL;
}


static void purge_watch_table_cb(void *key, void *entry)
{
    pep_table_t *t = (pep_table_t *)entry;

    MRP_UNUSED(key);

    destroy_watch_table(NULL, t);
}


static int create_wildcard_watches(pep_table_t *t, const char *name)
{
    pep_watch_t     *w;
    mrp_list_hook_t *p, *n;
    int              error;
    const char      *errmsg;

    mrp_list_foreach(&t->watches, p, n) {
        w = mrp_list_entry(p, typeof(*w), tbl_hook);

        if (find_proxy_watch(w->proxy, name) != NULL)
            continue;

        mrp_log_info("Subscribing client %s for table %s (%s).",
                     w->proxy->name, name, w->table->name);

        create_proxy_watch(w->proxy, w->id + w->nwatch++, name,
                           w->mql_columns, w->mql_where, w->max_rows,
                           &error, &errmsg);
    }

    return TRUE;
}


static int instantiate_wildcard_watches(pdp_t *pdp, const char *name)
{
    pep_table_t     *t, *e;
    mrp_list_hook_t *p, *n;
    regmatch_t       m[1];

    mrp_list_foreach(&pdp->wildcard, p, n) {
        t = mrp_list_entry(p, typeof(*t), hook);

        if (regexec(&t->re, name, 1, m, 0) == REG_NOMATCH)
            continue;

        if (t->imported)
            create_wildcard_watches(t, name);

        if (!t->exported)
            continue;

        if ((e = lookup_watch_table(pdp, name)) == NULL)
            e = create_watch_table(pdp, name);

        if (e != NULL && !e->exported) {
            e->exported = true;
            e->expid    = pdp->nexport++;
            mrp_log_info("Table %s marked for exporting.", e->name);
        }
    }

    return TRUE;
}


static int create_matching_watches(pep_watch_t *w)
{
    char       *tables[256];
    int         ntable, i;
    regmatch_t  m[1];
    int         error;
    const char *errmsg;

    ntable = mqi_show_tables(MQI_TEMPORARY, &tables[0], MRP_ARRAY_SIZE(tables));

    for (i = 0; i < ntable; i++) {
        if (regexec(&w->table->re, tables[i], 1, m, 0) == REG_NOMATCH)
            continue;

        mrp_log_info("Subscribing client %s for table %s (%s).",
                     w->proxy->name, tables[i], w->table->name);

        create_proxy_watch(w->proxy, w->id + w->nwatch++, tables[i],
                           w->mql_columns, w->mql_where, w->max_rows,
                           &error, &errmsg);
    }

    return TRUE;
}


int create_proxy_watch(pep_proxy_t *proxy, uint32_t id,
                       const char *table, const char *mql_columns,
                       const char *mql_where, int max_rows,
                       int *error, const char **errmsg)
{
    pdp_t       *pdp = proxy->pdp;
    pep_watch_t *w   = NULL;
    pep_table_t *t;
    bool         wildcard;

    t = lookup_watch_table(pdp, table);
    wildcard = wildcard_watch(table);

    /* Notes: in principle, we could allow these... */
    if (wildcard) {
        if (mql_columns && *mql_columns && strcmp(mql_columns, "*") != 0) {
            *error  = EINVAL;
            *errmsg = "columns must be * for wildcard watch";
            goto fail;
        }
        if (mql_where && *mql_where) {
            *error  = EINVAL;
            *errmsg = "where-clause not supported for wildcard watch";
            goto fail;
        }
    }

    if (t == NULL) {
        t = create_watch_table(pdp, table);

        if (t == NULL) {
            *error  = EINVAL;
            *errmsg = "failed to watch table";
        }
    }

    w = mrp_allocz(sizeof(*w));

    if (w != NULL) {
        mrp_list_init(&w->tbl_hook);
        mrp_list_init(&w->pep_hook);

        w->table        = t;
        w->mql_columns  = mrp_strdup(mql_columns);
        w->mql_where    = mrp_strdup(mql_where ? mql_where : "");
        w->max_rows     = max_rows;
        w->proxy        = proxy;
        w->id           = id;
        w->notify       = true;
        w->describe     = true;

        if (w->mql_columns == NULL || w->mql_where == NULL)
            goto fail;

        mrp_list_append(&t->watches, &w->tbl_hook);

        if (!wildcard)
            mrp_list_append(&proxy->watches, &w->pep_hook);
        else
            mrp_list_append(&proxy->wildcard, &w->pep_hook);

        if (wildcard)
            create_matching_watches(w);

        return TRUE;
    }
    else {
        *error  = ENOMEM;
        *errmsg = "failed to allocate table watch";
    }

 fail:
    if (w != NULL) {
        mrp_free(w->mql_columns);
        mrp_free(w->mql_where);
        mrp_free(w);
    }

    return FALSE;
}


void destroy_proxy_watch(pep_watch_t *w)
{
    if (w != NULL) {
        mrp_list_delete(&w->tbl_hook);
        mrp_list_delete(&w->pep_hook);

        mrp_free(w->mql_columns);
        mrp_free(w->mql_where);

        mrp_free(w);
    }
}


void destroy_proxy_watches(pep_proxy_t *proxy)
{
    pep_watch_t     *w;
    mrp_list_hook_t *p, *n;

    if (proxy != NULL) {
        mrp_list_foreach(&proxy->watches, p, n) {
            w = mrp_list_entry(p, typeof(*w), pep_hook);

            destroy_proxy_watch(w);
        }

        mrp_list_foreach(&proxy->wildcard, p, n) {
            w = mrp_list_entry(p, typeof(*w), pep_hook);

            destroy_proxy_watch(w);
        }
    }
}


static void reset_proxy_tables(pep_proxy_t *proxy)
{
    mrp_list_hook_t *p, *n;
    pep_table_t     *t;

    mrp_list_foreach(&proxy->tables, p, n) {
        t = mrp_list_entry(p, typeof(*t), hook);
        mqi_delete_from(t->h, NULL);
    }
}


static int insert_into_table(pep_table_t *t,
                             mrp_domctl_value_t **rows, int nrow)
{
    void *data[2];
    int   i;

    data[1] = NULL;

    for (i = 0; i < nrow; i++) {
        data[0] = rows[i];
        if (mqi_insert_into(t->h, 0, t->coldesc, data) != 1)
            return FALSE;
    }

    return TRUE;
}


int set_proxy_tables(pep_proxy_t *proxy, mrp_domctl_data_t *tables, int ntable,
                     int *error, const char **errmsg)
{
    mqi_handle_t    tx;
    pep_table_t    *t;
    int             i;

    tx = mqi_begin_transaction();

    if (tx != MQI_HANDLE_INVALID) {
        reset_proxy_tables(proxy);

        for (i = 0; i < ntable; i++) {
            t = lookup_proxy_table(proxy, tables[i].id);

            if (t == NULL)
                goto fail;

            if (tables[i].ncolumn != t->ncolumn)
                goto fail;

            if (!insert_into_table(t, tables[i].rows, tables[i].nrow))
                goto fail;

            mrp_log_info("Client %s set table #%u (%s, %d rows).", proxy->name,
                         tables[i].id, t->name, tables[i].nrow);
        }

        mqi_commit_transaction(tx);

        return TRUE;

    fail:
        mrp_log_error("Client %s failed to set table #%u (%s).", proxy->name,
                      tables[i].id, t ? t->name : "unknown");

        *error  = EINVAL;
        *errmsg = "failed to set tables";
        mqi_rollback_transaction(tx);
    }

    return FALSE;
}


void dump_table_data(mrp_domctl_data_t *table)
{
    mrp_domctl_value_t *row;
    int                 i, j;
    char                buf[1024], *p;
    const char         *t;
    int                 n, l;

    mrp_log_info("Table #%d ('%s'): %d rows x %d columns",
                 table->id, table->name,
             table->nrow, table->ncolumn);
    if (table->columns != NULL)
        mrp_log_info("    column definition: '%s'", table->columns);

    for (i = 0; i < table->nrow; i++) {
        row = table->rows[i];
        p   = buf;
        n   = sizeof(buf);

        for (j = 0, t = ""; j < table->ncolumn; j++, t = ", ") {
            switch (row[j].type) {
            case MRP_DOMCTL_STRING:
                l  = snprintf(p, n, "%s'%s'", t, row[j].str);
                p += l;
                n -= l;
                break;
            case MRP_DOMCTL_INTEGER:
                l  = snprintf(p, n, "%s%d", t, row[j].s32);
                p += l;
                n -= l;
                break;
            case MRP_DOMCTL_UNSIGNED:
                l  = snprintf(p, n, "%s%u", t, row[j].u32);
                p += l;
                n -= l;
                break;
            case MRP_DOMCTL_DOUBLE:
                l  = snprintf(p, n, "%s%f", t, row[j].dbl);
                p += l;
                n -= l;
                break;
            default:
                l  = snprintf(p, n, "%s<invalid column 0x%x>",
                              t, row[j].type);
                p += l;
                n -= l;
            }
        }

        mrp_log_info("row #%d: { %s }", i, buf);
    }
}
