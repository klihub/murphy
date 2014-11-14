/*
 * Copyright (c) 2014, Intel Corporation
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

#include <murphy/common/macros.h>
#include <murphy/common/list.h>
#include <murphy/common/mm.h>

#include <murphy-db/mqi.h>
#include <murphy-db/mql.h>
#include <murphy-db/mdb.h>

#include <murphy/domain-control/domain-control.h>

#include "table.h"
#include "replicator.h"



static int reset_table(pdp_t *pdp, mrp_domctl_data_t *t)
{
    mqi_handle_t  h = mqi_get_table_handle((char *)t->name);
    pep_table_t  *tbl;

    if (h != MQI_HANDLE_INVALID) {
        mqi_delete_from(h, NULL);
    }
    else {
        if (t->columns != NULL) {
            exec_mql(mql_result_dontcare, NULL,
                     "create temporary table %s (%s)", t->name, t->columns);

            if (!(tbl = lookup_watch_table(pdp, t->name)))
                tbl = create_watch_table(pdp, t->name);

            if (tbl != NULL)
                tbl->imported = true;
        }
    }

    return TRUE;
}


static int fill_table(mrp_domctl_data_t *t)
{
    mqi_handle_t         h;
    mdb_table_t         *tbl;
    mqi_column_def_t     defs[MQI_COLUMN_MAX];
    int                  ndef;
    mqi_column_desc_t    cols[MQI_COLUMN_MAX];
    void               **data;
    int                  i;

    if (t->nrow == 0)
        return TRUE;

    h   = mqi_get_table_handle((char *)t->name);
    tbl = mdb_table_find((char *)t->name);

    if (h == MQI_HANDLE_INVALID || tbl == NULL)
        return FALSE;

    ndef = mqi_describe(h, defs, MRP_ARRAY_SIZE(defs));

    if (ndef <= 0 || ndef != t->ncolumn)
        return FALSE;

    data = alloca((t->nrow + 1) * sizeof(data[0]));

    for (i = 0; i < ndef; i++) {
        cols[i].cindex = i;
        cols[i].offset = i * sizeof(mrp_domctl_value_t) +
            MRP_OFFSET(mrp_domctl_value_t, str);
    }
    cols[i].cindex = -1;

    for (i = 0; i < t->nrow; i++)
        data[i] = t->rows[i];
    data[i] = NULL;

    if (mdb_table_insert(tbl, false, cols, data) < 0)
        return FALSE;
    else
        return TRUE;
}


static int import_tables(pdp_t *pdp, mrp_domctl_data_t *tables, int ntable)
{
    mqi_handle_t       tx;
    mrp_domctl_data_t *t;
    int                i;

    MRP_UNUSED(pdp);

    tx = mqi_begin_transaction();

    for (i = 0, t = tables; i < ntable; i++, t++) {
        reset_table(pdp, t);
        fill_table(t);
    }

    mqi_commit_transaction(tx);

    return TRUE;
#if 0
 fail:
    mqi_rollback_transaction(tx);

    return FALSE;
#endif
}


static void data_notify(mrp_domctl_t *dc, mrp_domctl_data_t *tables,
                        int ntable, void *user_data)
{
    pdp_t             *pdp = (pdp_t *)user_data;
    mrp_domctl_data_t *t;
    int                i;

    MRP_UNUSED(dc);

    for (i = 0, t = tables; i < ntable; i++, t++)
        dump_table_data(t);

    import_tables(pdp, tables, ntable);
}


static void create_status(mrp_domctl_t *dc, int errcode,
                          const char *errmsg, void *user_data)
{
    pdp_t *pdp = (pdp_t *)user_data;

    MRP_UNUSED(dc);
    MRP_UNUSED(pdp);

    if (errcode)
        mrp_log_error("Failed to replicate/create tables (%d: %s).",
                      errcode, errmsg);
}


static void drop_status(mrp_domctl_t *dc, int errcode,
                       const char *errmsg, void *user_data)
{
    pdp_t *pdp = (pdp_t *)user_data;

    MRP_UNUSED(dc);
    MRP_UNUSED(pdp);

    if (errcode)
        mrp_log_error("Failed to replicate/drop tables (%d: %s).",
                      errcode, errmsg);
}


static void set_status(mrp_domctl_t *dc, int errcode,
                       const char *errmsg, void *user_data)
{
    pdp_t *pdp = (pdp_t *)user_data;

    MRP_UNUSED(dc);
    MRP_UNUSED(pdp);

    if (errcode)
        mrp_log_error("Failed to replicate/set tables (%d: %s).",
                      errcode, errmsg);
    else
        mrp_log_info("Tables replicated succesfully.");
}


int mark_matching_exports(pdp_t *pdp, pep_table_t *t)
{
    char       *tables[256];
    int         ntable, i;
    regmatch_t  m[1];

    ntable = mqi_show_tables(MQI_TEMPORARY, &tables[0], MRP_ARRAY_SIZE(tables));

    for (i = 0; i < ntable; i++) {
        if (t->exported)
            continue;

        if (regexec(&t->re, tables[i], 1, m, 0) != REG_NOMATCH) {
            t->exported = true;
            t->expid    = pdp->nexport++;
            mrp_log_info("Table %s marked for exporting.", t->name);
        }
    }

    return TRUE;
}


static int export_tables(pdp_t *pdp)
{
    mrp_list_hook_t    *p, *n;
    pep_table_t        *tbl;
    mrp_domctl_table_t *crt, *c;
    uint32_t           *drp, *d;
    mrp_domctl_data_t  *set, *t;
    mrp_domctl_value_t *v;
    int                 nc, nd, ns, i, j;
    mql_result_t       *r;

    if (pdp->nexport == 0)
        return TRUE;

    crt = alloca(pdp->nexport * sizeof(*crt));
    drp = alloca(pdp->nexport * sizeof(*drp));
    set = alloca(pdp->nexport * sizeof(*set));
    c   = crt;
    d   = drp;
    t   = set;
    nc  = 0;
    nd  = 0;
    ns  = 0;

    mrp_list_foreach(&pdp->tables, p, n) {
        tbl = mrp_list_entry(p, typeof(*tbl), hook);

        if (!tbl->exported)
            continue;

        if (tbl->exported && tbl->changed)
            mrp_debug("exporting table %s", tbl->name);

        if (!tbl->created && tbl->h != MQI_HANDLE_INVALID) {
            c->table       = tbl->name;
            c->id          = tbl->expid;
            c->mql_columns = tbl->mql_columns;
            c->mql_index   = tbl->mql_index;
            c++;
            nc++;

            tbl->created = true;
        }

        if (tbl->created && tbl->h == MQI_HANDLE_INVALID) {
            *d++ = tbl->expid;
            nd++;
            tbl->created = false;
        }

        if (tbl->h == MQI_HANDLE_INVALID)
            continue;

        t->name = tbl->name;
        t->id   = tbl->expid;

        r = NULL;
        if (!exec_mql(mql_result_rows, &r, "select * from %s", t->name))
            return FALSE;

        t->columns = NULL;
        t->ncolumn = tbl->ncolumn;
        t->nrow    = mql_result_rows_get_row_count(r);

        mrp_debug("replicating %d rows for table %s", t->nrow, t->name);

        t->rows = alloca(t->nrow * sizeof(*t->rows));
        v = alloca(t->ncolumn * t->nrow * sizeof(*v));

        for (i = 0; i < t->nrow; i++) {
            t->rows[i] = v;
            for (j = 0; j < t->ncolumn; j++) {
                switch (tbl->columns[j].type) {
                case mqi_string:
                    v->type = MRP_DOMCTL_STRING;
                    v->str  = mql_result_rows_get_string(r, j, i, NULL, 0);
                    break;
                case mqi_integer:
                    v->type = MRP_DOMCTL_INTEGER;
                    v->s32  = mql_result_rows_get_integer(r, j, i);
                    break;
                case mqi_unsignd:
                    v->type = MRP_DOMCTL_UNSIGNED;
                    v->s32  = mql_result_rows_get_unsigned(r, j, i);
                    break;
                case mqi_floating:
                    v->type = MRP_DOMCTL_DOUBLE;
                    v->s32  = mql_result_rows_get_floating(r, j, i);
                    break;
                default:
                    mql_result_free(r);
                    return FALSE;
                }
                v++;
            }
        }

        mql_result_free(r);

        t++;
        ns++;
    }

    if (nc > 0)
        mrp_domctl_create_tables(pdp->dc, crt, nc, create_status, pdp);
    if (nd > 0)
        mrp_domctl_drop_tables(pdp->dc, drp, nd, drop_status, pdp);
    if (ns > 0)
        if (!mrp_domctl_set_data(pdp->dc, set, ns, set_status, pdp))
            mrp_log_error("Failed to replicate table data.");

    return TRUE;
}


int replicate_exports(pdp_t *pdp)
{
    return export_tables(pdp);
}


static void connect_notify(mrp_domctl_t *dc, int connected, int errcode,
                           const char *errmsg, void *user_data)
{
    pdp_t           *pdp = (pdp_t *)user_data;
    mrp_list_hook_t *p, *n;
    pep_table_t     *t;

    MRP_UNUSED(dc);

    if (connected) {
        mrp_log_info("replicator: connection to master up");
        export_tables(pdp);
    }
    else {
        mrp_log_info("replicator: connection to master down (%d: %s).",
                     errcode, errmsg);

        mrp_list_foreach(&pdp->tables, p, n) {
            t = mrp_list_entry(p, typeof(*t), hook);

            if (t->exported)
                t->created = false;

            if (t->imported) {
                exec_mql(mql_result_dontcare, NULL, "drop table %s", t->name);
                invalidate_table(t);
            }
        }
    }
}


#define NID_PER_WILDCARD 256
int create_replicator(pdp_t *pdp, const char *master,
                      const char *imports, const char *exports)
{
    mrp_mainloop_t     *ml        = pdp->ctx->ml;
    int                 nwatch    = 0;
    int                 nwildcard = 0;
    mrp_domctl_watch_t  watches[32], *w;
    pep_table_t        *table;
    const char         *t, *e;
    char               *name, tbl[128];
    int                 l;

    mrp_list_init(&pdp->imports);
    mrp_list_init(&pdp->exports);

    if (imports == NULL && exports == NULL)
        return TRUE;

    t = imports;
    while (t && *t) {
        while (*t == ' ')
            t++;

        e = strchr(t, ',');

        if (e != NULL)
            l = e - t;
        else
            l = strlen(t);

        if (nwatch >= (int)MRP_ARRAY_SIZE(watches))
            return FALSE;

        name = alloca(l + 1);
        strncpy(name, t, l);
        name[l] = '\0';

        w = watches + nwatch;

        w->table       = name;
        w->id          = nwatch++ + nwildcard * NID_PER_WILDCARD;
        w->mql_columns = "*";
        w->mql_where   = NULL;
        w->max_rows    = 0;

        if (wildcard_watch(name))
            nwildcard++;

        if (e)
            t = e + 1;
        else
            t = NULL;
    }

    t = exports;
    while (t && *t) {
        while (*t == ' ')
            t++;

        e = strchr(t, ',');

        if (e != NULL)
            l = e - t;
        else
            l = strlen(t);

        snprintf(tbl, sizeof(tbl), "%*.*s", l, l, t);

        table = lookup_watch_table(pdp, tbl);

        if (table == NULL)
            table = create_watch_table(pdp, tbl);

        if (table == NULL)
            return FALSE;

        table->exported = true;
        table->created  = false;

        if (!wildcard_watch(tbl))
            table->expid = pdp->nexport++;
        else
            mark_matching_exports(pdp, table);

        if (e)
            t = e + 1;
        else
            t = NULL;
    }

    pdp->dc = mrp_domctl_create("murphy", ml, NULL, 0, watches, nwatch,
                                connect_notify, data_notify, pdp);

    if (master == NULL)
        master = MRP_DEFAULT_DOMCTL_ADDRESS;

    if (pdp->dc != NULL)
        mrp_domctl_connect(pdp->dc, master, 0);

    return TRUE;
}


void destroy_replicator(pdp_t *pdp)
{
    MRP_UNUSED(pdp);

    return;
}
