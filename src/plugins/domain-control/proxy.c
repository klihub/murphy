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

#include <murphy/common/log.h>
#include <murphy/common/mm.h>
#include <murphy/common/list.h>

#include "domain-control-types.h"
#include "table.h"
#include "proxy.h"


/*
 * a pending proxied invocation
 */

typedef struct {
    mrp_list_hook_t         hook;        /* to pending list */
    uint32_t                id;          /* request id */
    mrp_domain_return_cb_t  cb;          /* return callback */
    void                   *user_data;   /* opaque callback data */
} pending_t;

static void purge_pending(pep_proxy_t *proxy);


int init_proxies(pdp_t *pdp)
{
    mrp_list_init(&pdp->proxies);

    return TRUE;
}


void destroy_proxies(pdp_t *pdp)
{
    MRP_UNUSED(pdp);

    return;
}


pep_proxy_t *create_proxy(pdp_t *pdp)
{
    pep_proxy_t *proxy;

    proxy = mrp_allocz(sizeof(*proxy));

    if (proxy != NULL) {
        mrp_list_init(&proxy->hook);
        mrp_list_init(&proxy->tables);
        mrp_list_init(&proxy->watches);
        mrp_list_init(&proxy->wildcard);
        mrp_list_init(&proxy->pending);

        proxy->pdp   = pdp;
        proxy->seqno = 1;

        mrp_list_append(&pdp->proxies, &proxy->hook);
    }

    return proxy;
}


void destroy_proxy(pep_proxy_t *proxy)
{
    if (proxy == NULL)
        return;

    mrp_list_delete(&proxy->hook);
    destroy_proxy_tables(proxy);
    destroy_proxy_watches(proxy);
    purge_pending(proxy);

    mrp_free(proxy);
}


pep_table_t *find_proxy_table(pep_proxy_t *proxy, const char *name)
{
    mrp_list_hook_t *p, *n;
    pep_table_t     *t;

    mrp_list_foreach(&proxy->tables, p, n) {
        t = mrp_list_entry(p, typeof(*t), hook);

        if (t->name != NULL && !strcmp(t->name, name))
            return t;
    }

    return NULL;
}


pep_table_t *lookup_proxy_table(pep_proxy_t *proxy, uint32_t id)
{
    mrp_list_hook_t *p, *n;
    pep_table_t     *t;

    mrp_list_foreach(&proxy->tables, p, n) {
        t = mrp_list_entry(p, typeof(*t), hook);

        if (t->id == id)
            return t;
    }

    return NULL;
}


pep_watch_t *find_proxy_watch(pep_proxy_t *proxy, const char *name)
{
    mrp_list_hook_t *p, *n;
    pep_watch_t     *w;

    mrp_list_foreach(&proxy->watches, p, n) {
        w = mrp_list_entry(p, typeof(*w), pep_hook);

        if (w->table != NULL && !strcmp(w->table->name, name))
            return w;
    }

    return NULL;
}


static pep_watch_t *lookup_proxy_watch(pep_proxy_t *proxy, uint32_t id)
{
    mrp_list_hook_t *p, *n;
    pep_watch_t     *w;

    mrp_list_foreach(&proxy->watches, p, n) {
        w = mrp_list_entry(p, typeof(*w), pep_hook);

        if (w->id == id)
            return w;
    }

    return NULL;
}


int create_proxy_tables(pep_proxy_t *proxy, mrp_domctl_table_t *tables,
                        int ntable, int *error, const char **errmsg)
{
    mrp_domctl_table_t *t;
    int                 i;

    for (i = 0, t = tables; i < ntable; i++, t++) {
        if (lookup_proxy_table(proxy, t->id) != NULL) {
            mrp_log_error("Client %s already has table #%u.", proxy->name,
                          t->id);
            *error  = EEXIST;
            *errmsg = "table id already in use";
            ntable = i;
            goto fail;
        }


        if (create_proxy_table(proxy, t->id, t->table, t->mql_columns,
                               t->mql_index, error, errmsg)) {
            mrp_log_info("Client %s created table %s.", proxy->name, t->table);
        }
        else {
            mrp_log_error("Client %s failed to create table %s (%d: %s).",
                          proxy->name, t->table, *error, *errmsg);
            ntable = i;
            goto fail;
        }
    }

    return TRUE;

 fail:
    for (i = 0, t = tables; i < ntable; i++, t++)
        destroy_proxy_table(lookup_proxy_table(proxy, t->id));

    return FALSE;
}


int delete_proxy_tables(pep_proxy_t *proxy, uint32_t *ids, int nid,
                        int *error, const char **errmsg)
{
    pep_table_t *t;
    int          i;

    MRP_UNUSED(error);
    MRP_UNUSED(errmsg);

    for (i = 0; i < nid; i++) {
        if ((t = lookup_proxy_table(proxy, ids[i])) != NULL) {
            mrp_log_info("Client %s destroyed table #%u (%s).", proxy->name,
                         t->id, t->name ? t->name : "unknown");
            destroy_proxy_table(t);
        }
    }

    return TRUE;
}


int create_proxy_watches(pep_proxy_t *proxy, mrp_domctl_watch_t *watches,
                         int nwatch, int *error, const char **errmsg)
{
    pep_watch_t *w;
    int          i;

    for (i = 0; i < nwatch; i++) {
        if ((w = find_proxy_watch(proxy, watches[i].table)) != NULL ||
            (w = lookup_proxy_watch(proxy, watches[i].id))  != NULL) {
            mrp_log_error("Client %s already subscribed for #%u or %s.",
                          proxy->name, watches[i].id, watches[i].table);
            *error  = EEXIST;
            *errmsg = "watch/id already exists";
            nwatch = i;
            goto fail;
        }

        if (create_proxy_watch(proxy, watches[i].id, watches[i].table,
                               watches[i].mql_columns,
                               watches[i].mql_where,
                               watches[i].max_rows,
                               error, errmsg)) {
            mrp_log_info("Client %s subscribed for table %s.", proxy->name,
                         watches[i].table);
        }
        else {
            mrp_log_error("Client %s failed to subscribe for table %s.",
                          proxy->name, watches[i].table);
            nwatch = i;
            goto fail;
        }
    }

    return TRUE;

 fail:
    for (i = 0; i < nwatch; i++) {
        if ((w = lookup_proxy_watch(proxy, watches[i].id)) != NULL)
            destroy_proxy_watch(w);
    }

    return FALSE;
}


int delete_proxy_watches(pep_proxy_t *proxy, uint32_t *ids, int nid,
                         int *error, const char **errmsg)
{
    pep_watch_t *w;
    int          i;

    MRP_UNUSED(error);
    MRP_UNUSED(errmsg);

    for (i = 0; i < nid; i++) {
        if ((w = lookup_proxy_watch(proxy, ids[i])) != NULL) {
            mrp_log_info("Client %s unsubscribed from table #%u (%s).",
                         proxy->name, ids[i], w->table && w->table->name ?
                         w->table->name : "unknown");
            destroy_proxy_watch(w);
        }
    }

    return TRUE;
}


int register_proxy(pep_proxy_t *proxy, char *name,
                   mrp_domctl_table_t *tables, int ntable,
                   mrp_domctl_watch_t *watches, int nwatch,
                   int *error, const char **errmsg)
{
    proxy->name   = mrp_strdup(name);
    proxy->notify = true;

    if (proxy->name == NULL) {
        *error  = ENOMEM;
        *errmsg = "failed to allocate proxy table";

        return FALSE;
    }

    if (!create_proxy_tables(proxy, tables, ntable, error, errmsg))
        return FALSE;

    if (!create_proxy_watches(proxy, watches, nwatch, error, errmsg))
        return FALSE;

    return TRUE;
}


int unregister_proxy(pep_proxy_t *proxy)
{
    destroy_proxy(proxy);

    return TRUE;
}


pep_proxy_t *find_proxy(pdp_t *pdp, const char *name)
{
    mrp_list_hook_t *p, *n;
    pep_proxy_t     *proxy;

    mrp_list_foreach(&pdp->proxies, p, n) {
        proxy = mrp_list_entry(p, typeof(*proxy), hook);

        if (!strcmp(proxy->name, name))
            return proxy;
    }

    return NULL;
}


uint32_t proxy_queue_pending(pep_proxy_t *proxy,
                             mrp_domain_return_cb_t return_cb, void *user_data)
{
    pending_t *pending;

    if (return_cb == NULL)
        return proxy->seqno++;

    pending = mrp_allocz(sizeof(*pending));

    if (pending == NULL)
        return 0;

    mrp_list_init(&pending->hook);

    pending->id        = proxy->seqno++;
    pending->cb        = return_cb;
    pending->user_data = user_data;

    mrp_list_append(&proxy->pending, &pending->hook);

    return pending->id;
}


int proxy_dequeue_pending(pep_proxy_t *proxy, uint32_t id,
                          mrp_domain_return_cb_t *cbp, void **user_datap)
{
    mrp_list_hook_t *p, *n;
    pending_t       *pending;

    mrp_list_foreach(&proxy->pending, p, n) {
        pending = mrp_list_entry(p, typeof(*pending), hook);

        if (pending->id == id) {
            mrp_list_delete(&pending->hook);
            *cbp        = pending->cb;
            *user_datap = pending->user_data;

            mrp_free(pending);

            return TRUE;
        }
    }

    return FALSE;
}


static void purge_pending(pep_proxy_t *proxy)
{
    mrp_list_hook_t *p, *n;
    pending_t       *pending;

    mrp_list_foreach(&proxy->pending, p, n) {
        pending = mrp_list_entry(p, typeof(*pending), hook);

        mrp_list_delete(&pending->hook);
        mrp_free(pending);
    }
}
