/*
 * Copyright (c) 2014, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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

#include "client.h"
#include "socket.h"

static resource_proxy_global_context_t *global_ctx;

/* helper functions */

int proxy_notify_clients(resource_proxy_global_context_t *ctx,
        enum resource_proxy_status status)
{
    /* Notify the clients that the resource library is ready. This could also
     * be used to finally load the resource frontends. */

    MRP_UNUSED(ctx);

    mrp_debug("notify_clients: %d", status);

    return 0;
}

#define ARRAY_MAX 1024

void proxy_attribute_array_free(mrp_attr_def_t *arr, uint32_t dim)
{
    uint32_t i;
    mrp_attr_def_t *attr;

    if (arr) {
        for (i = 0; i < dim; i++) {
            attr = arr + i;

            mrp_free((void *)attr->name);

            if (attr->type == mqi_string)
                mrp_free((void *) attr->value.string);
        }
        mrp_free(arr);
    }
}


mrp_attr_def_t *proxy_attribute_def_array_dup(uint32_t dim, mrp_attr_t *arr)
{
    size_t size;
    uint32_t i;
    mrp_attr_t *sattr;
    mrp_attr_def_t *dattr;
    mrp_attr_def_t *dup;
    int err;

    size = (sizeof(mrp_attr_def_t) * (dim + 1));

    if (!(dup = mrp_allocz(size))) {
        err = ENOMEM;
        goto failed;
    }

    for (i = 0; i < dim; i++) {
        sattr = arr + i;
        dattr = dup + i;

        /* we are faking this because we don't have the definitions locally */
        dattr->access = MRP_RESOURCE_RW;

        if (!(dattr->name = mrp_strdup(sattr->name))) {
            err = ENOMEM;
            goto failed;
        }

        switch ((dattr->type = sattr->type)) {
        case mqi_string:
            dattr->type = mqi_string;
            if (!(dattr->value.string = mrp_strdup(sattr->value.string))) {
                err = ENOMEM;
                goto failed;
            }
            break;
        case mqi_integer:
            dattr->type = mqi_integer,
            dattr->value.integer = sattr->value.integer;
            break;
        case mqi_unsignd:
            dattr->type = mqi_unsignd;
            dattr->value.unsignd = sattr->value.unsignd;
            break;
        case mqi_floating:
            dattr->type = mqi_floating;
            dattr->value.floating = sattr->value.floating;
            break;
        default:
            err = EINVAL;
            goto failed;
        }
    }

    return dup;

 failed:
    proxy_attribute_array_free(dup, dim);
    errno = err;
    return NULL;
}


void proxy_str_array_free(proxy_string_array_t *arr)
{
    int i;

    mrp_debug("%p", arr);

    if (!arr)
        return;

    for (i = 0; i < arr->num_strings; i++)
        mrp_free((void *) arr->strings[i]);

    mrp_free(arr->strings);
    mrp_free(arr);
}


proxy_string_array_t *proxy_str_array_dup(uint32_t dim, const char **arr)
{
    uint32_t i;
    proxy_string_array_t *dup;

    if (dim >= ARRAY_MAX || !arr)
        return NULL;

    if (!dim && arr) {
        for (dim = 0;  arr[dim];  dim++)
            ;
    }

    if (!(dup = mrp_allocz(sizeof(proxy_string_array_t)))) {
        errno = ENOMEM;
        return NULL;
    }

    dup->num_strings = dim;
    dup->strings = mrp_allocz_array(const char *, dim);

    if (!dup->strings) {
        mrp_free(dup);
        errno = ENOMEM;
        return NULL;
    }

    for (i = 0;   i < dim;   i++) {
        if (arr[i]) {
            if (!(dup->strings[i] = mrp_strdup(arr[i]))) {
                for (; i > 0; i--) {
                    mrp_free((void *)dup->strings[i-1]);
                }
                mrp_free(dup->strings);
                mrp_free(dup);
                errno = ENOMEM;
                return NULL;
            }
            mrp_debug("copied string %s", dup->strings[i]);
        }
    }

    return dup;
}

#undef ARRAY_MAX


static int int_comp(const void *key1, const void *key2)
{
    return key1 - key2;
}


static uint32_t int_hash(const void *key)
{
    return p_to_u(key);
}

/* public API */

void print_attribute(mrp_attr_t *attr)
{
    if (!attr) {
        mrp_debug("NULL attribute");
        return;
    }

    switch (attr->type) {
        case mqi_string:
            mrp_debug("set attr '%s' to value '%s'", attr->name,
                    attr->value.string);
            break;
        case mqi_unsignd:
            mrp_debug("set attr '%s' to value '%u'", attr->name,
                    attr->value.unsignd);
            break;
        case mqi_integer:
            mrp_debug("set attr '%s' to value '%d'", attr->name,
                    attr->value.integer);
            break;
        case mqi_floating:
            mrp_debug("set attr '%s' to value '%f'", attr->name,
                    attr->value.floating);
            break;
        default:
            /*
            mrp_debug("corrupted attribute %s (%d)", attr->name, attr->type);
            */
            break;
    }
}


int mrp_attribute_set_values(mrp_attr_t      *values,
                             uint32_t          nattr,
                             mrp_attr_def_t   *defs,
                             mrp_attr_value_t *attrs)
{
    mrp_attr_t *attr;
    mrp_attr_def_t *adef;
    mrp_attr_value_t *vsrc;
    mrp_attr_value_t *vdst;
    uint32_t i;

    mrp_debug("%p, %d, %p, %p", values, nattr, defs, attrs);

    if (!(!nattr || (nattr > 0 && defs && attrs))) {
        mrp_debug("argument error");
        return -1;
    }

    for (i = 0; i < nattr; i++) {
        adef = defs + i;
        vdst = attrs + i;

        attr = values;

        /* default value */
        vsrc = &adef->value;

        if (attr && adef->access & MRP_RESOURCE_WRITE) {
            /* write access */
            while (attr->name) {
                if (strcasecmp(adef->name, attr->name) == 0) {
                    vsrc = &attr->value;
                    break;
                }
                attr++;
            }
        }

        if (adef->type != mqi_string)
            *vdst = *vsrc;
        else if (vdst->string != vsrc->string) {
            /* if the string is not the same, change it */
            mrp_free((void *)vdst->string);
            if (!(vdst->string = mrp_strdup(vsrc->string)))
                return -1;
        }

         if (attr)
            print_attribute(attr);
    }

    return 0;
}


mrp_attr_t *mrp_attribute_get_value(uint32_t          idx,
                                    mrp_attr_t       *value,
                                    uint32_t          nattr,
                                    mrp_attr_def_t   *defs,
                                    mrp_attr_value_t *attrs)
{
    mrp_attr_t *vdst;
    mrp_attr_def_t *adef;

    if (idx >= nattr) {
        mrp_debug("invalid argument");
        return NULL;
    }

    if (!(!nattr || (nattr > 0 && defs && attrs))) {
        mrp_debug("invalid argument");
        return NULL;
    }

    if ((vdst = value) || (vdst = mrp_alloc(sizeof(mrp_attr_t)))) {
        adef = defs + idx;

        if (!(adef->access & MRP_RESOURCE_READ))
            memset(vdst, 0, sizeof(mrp_attr_t));
        else {
            vdst->name  = adef->name;
            vdst->type  = adef->type;
            vdst->value = attrs[idx];
        }
    }

    return vdst;
}


mrp_attr_t *mrp_resource_read_attribute(mrp_resource_t *res,
                                        uint32_t        idx,
                                        mrp_attr_t     *value)
{
    mrp_attr_t *ret;
    mrp_attr_def_t *attr_defs;

    if (!res || !res->def)
        return NULL;

    attr_defs = mrp_htbl_lookup(global_ctx->resource_names_to_attribute_defs,
            (void *) res->def->name);

    ret = mrp_attribute_get_value(idx, value, res->def->nattr,
            attr_defs, res->attrs);

    if (!ret) {
        mrp_debug("Memory alloc failure. Can't get "
                "resource '%s' attribute %u", res->def->name, idx);
        return NULL;
    }

    return ret;
}


mrp_attr_t *mrp_attribute_get_all_values(uint32_t          nvalue,
                                         mrp_attr_t       *values,
                                         uint32_t          nattr,
                                         mrp_attr_def_t   *defs,
                                         mrp_attr_value_t *attrs)
{
    mrp_attr_def_t *adef;
    mrp_attr_t *vdst, *vend;
    uint32_t i;

    if (!(!nvalue || (nvalue > 0 && values)) &&
            (!nattr || (nattr > 0 && defs)))
        return NULL;

    if (nvalue)
        nvalue--;
    else {
        for (i = 0;  i < nattr;  i++) {
            if (!attrs || (attrs && (defs[i].access & MRP_RESOURCE_READ)))
                nvalue++;
        }

        if (!(values = mrp_allocz(sizeof(mrp_attr_t) * (nvalue + 1)))) {
            mrp_debug("Memory alloc failure. Can't get attributes");
            return NULL;
        }
    }

    vend = (vdst = values) + nvalue;

    for (i = 0; i < nattr && vdst < vend; i++) {
        adef = defs  + i;

        if (!(adef->access && MRP_RESOURCE_READ))
            continue;

        vdst->name = adef->name;
        vdst->type = adef->type;
        vdst->value = attrs ? attrs[i] : adef->value;

        vdst++;
    }

    memset(vdst, 0, sizeof(*vdst));

    return values;
}


mrp_attr_t *mrp_resource_read_all_attributes(mrp_resource_t *res,
                                             uint32_t nvalue,
                                             mrp_attr_t *values)
{
    mrp_attr_t *retval;
    mrp_attr_def_t *attr_defs;

    if (!res || !res->def)
        return NULL;

    if (!global_ctx)
        return NULL;

    attr_defs = mrp_htbl_lookup(global_ctx->resource_names_to_attribute_defs,
            (void *) res->def->name);

    retval = mrp_attribute_get_all_values(nvalue, values, res->def->nattr,
            attr_defs, res->attrs);

    if (!retval) {
        mrp_debug("Memory alloc failure. Can't get all "
                "attributes of resource '%s'", res->def->name);
    }

    return retval;
}


int mrp_resource_write_attributes(mrp_resource_t *res, mrp_attr_t *values)
{
    int ret;
    mrp_resource_def_t *rdef;
    mrp_attr_def_t *attrdefs;

    mrp_debug("%p, %p", res, values);

    rdef = res->def;

    if (!rdef)
        return -1;

    attrdefs = mrp_htbl_lookup(global_ctx->resource_names_to_attribute_defs,
            (void *) rdef->name);

    ret = mrp_attribute_set_values(values, rdef->nattr,
            attrdefs, res->attrs);

    if (ret < 0) {
        mrp_debug("Memory alloc failure. Can't set attributes "
                "of resource '%s'", rdef->name);
    }

    return ret;
}


mrp_resource_t *mrp_resource_create(const char *name, uint32_t rsetid,
        bool autorel, bool shared, mrp_attr_t *attrs)
{
    mrp_resource_t *res = NULL;
    mrp_resource_def_t *rdef;
    size_t base_size;
    size_t attr_size;
    size_t total_size;
    int sts;

    mrp_debug("%s, %u, %d, %d, %p", name, rsetid, autorel, shared, attrs);

    if (!(rdef = mrp_resource_definition_find_by_name(name))) {
        mrp_debug("Can't find resource definition '%s'. "
                "No resource created", name);
    }
    else {
        base_size  = sizeof(mrp_resource_t);
        attr_size  = sizeof(mrp_attr_value_t) * rdef->nattr;
        total_size = base_size + attr_size;

        if (!(res = mrp_allocz(total_size))) {
            mrp_debug("Memory alloc failure. Can't create "
                    "resource '%s'", name);
        }
        else {
            mrp_attr_def_t *attrdefs;

            mrp_list_init(&res->list);

            res->rsetid = rsetid;
            res->def = rdef;
            res->shared = rdef->shareable ?  shared : false;

            attrdefs = mrp_htbl_lookup(
                    global_ctx->resource_names_to_attribute_defs,
                    (void *) rdef->name);

            sts = mrp_attribute_set_values(attrs, rdef->nattr,
                                           attrdefs, res->attrs);
            if (sts < 0) {
                mrp_debug("Memory alloc failure. No '%s' "
                        "resource created", name);
                return NULL;
            }
        }
    }

    return res;
}


mrp_resource_def_t *mrp_resource_definition_find_by_name(const char *name)
{
    mrp_resource_def_t *rdef;

    if (!global_ctx)
        return NULL;

    if (!global_ctx->defs)
        return NULL;

    rdef = global_ctx->defs;

    do {
        if (rdef->name && strcmp(name, rdef->name) == 0)
            return rdef;

        rdef++;
    } while (rdef->name);

    mrp_debug("not found");
    return NULL;
}


static mrp_resource_t *find_resource_by_name(mrp_resource_set_t *resource_set,
                                             const char *name)
{
    mrp_list_hook_t *entry, *n;
    mrp_resource_t *res;
    mrp_resource_def_t *rdef;

    if (!resource_set || !name)
        return NULL;

    mrp_list_foreach(&resource_set->resource.list, entry, n) {
        res = mrp_list_entry(entry, mrp_resource_t, list);
        rdef = res->def;

        if (!rdef) {
            mrp_log_error("no rdef in %p for %s", resource_set, name);
            return NULL;
        }

        if (!strcasecmp(name, rdef->name))
            return res;
    }

    mrp_debug("not found");
    return NULL;
}

/* initialization */

static void htbl_free_attr_defs(void *key, void *object)
{
    mrp_attr_def_t *attr_defs = object;
    mrp_attr_def_t *iter = attr_defs;

    MRP_UNUSED(key);

    while (iter->name) {

        if (iter->type == mqi_string) {
            mrp_free((void *) iter->value.string);
        }
        mrp_free((void *) iter->name);

        iter++;
    }

    mrp_free(attr_defs);
}

static void htbl_free_client(void *key, void *object)
{
    resource_proxy_client_t *proxy_client = object;

    MRP_UNUSED(key);

    mrp_resource_client_destroy(proxy_client->client);
    mrp_free(proxy_client->name);
    mrp_free(proxy_client);
}

static resource_proxy_global_context_t *initialize_ctx()
{
    mrp_htbl_config_t cfg;
    resource_proxy_global_context_t *ctx = mrp_allocz(
            sizeof(resource_proxy_global_context_t));

    if (!ctx)
        goto error;

    ctx->refcount = 1;

    /* initialize the maps */

    cfg.nentry  = 32;
    cfg.comp    = int_comp;
    cfg.hash    = int_hash;
    cfg.free    = htbl_free_client;
    cfg.nbucket = cfg.nentry;

    ctx->clients_to_proxy_clients = mrp_htbl_create(&cfg);

    cfg.free = NULL;
    ctx->ids_to_proxy_rs = mrp_htbl_create(&cfg);
    ctx->seqnos_to_proxy_rs = mrp_htbl_create(&cfg);

    cfg.free = htbl_free_attr_defs;
    cfg.hash = mrp_string_hash;
    cfg.comp = mrp_string_comp;
    ctx->resource_names_to_attribute_defs = mrp_htbl_create(&cfg);

    /* finally mapping from resource sets to proxy resource sets */

    cfg.free = NULL;
    cfg.hash = int_hash;
    cfg.comp = int_comp;
    ctx->rs_to_proxy_rs = mrp_htbl_create(&cfg);

    if (!ctx->clients_to_proxy_clients ||
            !ctx->ids_to_proxy_rs || !ctx->seqnos_to_proxy_rs ||
            !ctx->resource_names_to_attribute_defs || !ctx->rs_to_proxy_rs)
        goto error;

    /* this is what we tell clients */
    ctx->next_rset_id = 1;

    global_ctx = ctx;

    return ctx;

error:
    mrp_debug("error creating resource proxy context");

    if (ctx) {
        if (ctx->ids_to_proxy_rs)
            mrp_htbl_destroy(ctx->ids_to_proxy_rs, FALSE);

        if (ctx->seqnos_to_proxy_rs)
            mrp_htbl_destroy(ctx->seqnos_to_proxy_rs, FALSE);

        if (ctx->resource_names_to_attribute_defs)
            mrp_htbl_destroy(ctx->resource_names_to_attribute_defs, FALSE);

        if (ctx->clients_to_proxy_clients)
            mrp_htbl_destroy(ctx->clients_to_proxy_clients, FALSE);

        if (ctx->rs_to_proxy_rs)
            mrp_htbl_destroy(ctx->rs_to_proxy_rs, FALSE);

        mrp_free(ctx);
    }

    return NULL;
}

/* implementaion of the resource API */

mrp_resource_client_t *mrp_resource_client_create(const char *name,
                                                  void *user_data)
{
    mrp_resource_client_t *client = NULL;
    resource_proxy_client_t *proxy_client = NULL;

    /* local: returns a client object */

    mrp_debug("%s, %p", name, user_data);

    /* create a global context if it doesn't exist */

    if (!global_ctx) {
        goto error;
    }

    client = mrp_allocz(sizeof(mrp_resource_client_t));
    if (!client)
        goto error;

    mrp_list_init(&client->resource_sets);
    mrp_list_init(&client->list);

    proxy_client = mrp_allocz(sizeof(*proxy_client));
    if (!proxy_client)
        goto error;

    proxy_client->name = mrp_strdup(name);
    if (!proxy_client->name)
        goto error;

    client->name = proxy_client->name;

    proxy_client->client = client;

    /* add client->proxy_client to map in global context */

    mrp_htbl_insert(global_ctx->clients_to_proxy_clients, client, proxy_client);

    return client;

error:
    mrp_free(client);

    if (proxy_client) {
        mrp_free(proxy_client->name);
        mrp_free(proxy_client);
    }

    return NULL;
}


static void free_resource_set(mrp_resource_set_t *rset,
        resource_proxy_global_context_t *ctx)
{
    mrp_resource_t *res;
    mrp_list_hook_t *entry, *n;

    if (!rset)
        return;

    mrp_list_foreach(&rset->resource.list, entry, n) {
        uint32_t i;
        mrp_attr_def_t *attr_defs;

        res = mrp_list_entry(entry, mrp_resource_t, list);

        attr_defs = mrp_htbl_lookup(ctx->resource_names_to_attribute_defs,
                (void *) res->def->name);

        mrp_list_delete(&res->list);

        for (i = 0; i < res->def->nattr; i++) {
            mrp_attr_value_t *attr = res->attrs + i;
            if (attr_defs[i].type == mqi_string)
                mrp_free((void *) attr->string);
        }

        mrp_free(res);
    }

    mrp_list_delete(&rset->client.list);
    rset->event = NULL;
    mrp_free(rset);
}


static void free_proxy_resource_set(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    mrp_list_hook_t *entry, *n;

    if (!prset)
        return;

    mrp_list_foreach(&prset->operation_queue, entry, n) {
        resource_proxy_rset_operation_t *op = mrp_list_entry(entry,
                resource_proxy_rset_operation_t, hook);
        mrp_list_delete(&op->hook);
        mrp_free(op);
    }

    if (!ctx)
        return;

    free_resource_set(prset->rs, ctx);

    mrp_free(prset->class_name);
    mrp_free(prset->zone_name);
    mrp_free(prset);
}


void mrp_resource_client_destroy(mrp_resource_client_t *client)
{
    /* local: destroys the proxy client object */

    mrp_debug("%p", client);

    if (!client)
        return;

    if (global_ctx) {
        mrp_list_hook_t *entry, *n;
        resource_proxy_client_t *proxy_client =
                mrp_htbl_lookup(global_ctx->clients_to_proxy_clients, client);

        /* delete all resource sets belonging to the client */

        mrp_list_foreach(&client->resource_sets, entry, n) {

            resource_proxy_resource_set_t *prset;

            mrp_resource_set_t *rset = mrp_list_entry(entry, mrp_resource_set_t,
                    client.list);

            if (!rset) {
                mrp_debug("client resource set list error");
                continue;
            }

            prset = mrp_htbl_lookup(global_ctx->rs_to_proxy_rs, rset);

            if (!prset) {
                mrp_debug("proxy resource set not found");
                continue;
            }

            mrp_htbl_remove(global_ctx->seqnos_to_proxy_rs, u_to_p(prset->seqno),
                    FALSE);
            mrp_htbl_remove(global_ctx->ids_to_proxy_rs, u_to_p(prset->id),
                    FALSE);
            mrp_htbl_remove(global_ctx->rs_to_proxy_rs, prset->rs, FALSE);
            destroy_resource_set_request(global_ctx, prset);
            free_proxy_resource_set(global_ctx, prset);
        }

        if (proxy_client) {
            mrp_free(proxy_client->name);
            mrp_free(proxy_client);
        }

        mrp_free(client);

        global_ctx->refcount--;

        if (global_ctx->refcount == 0) {
            /* TODO: no clients, clean up? */
        }
    }
}


mrp_resource_set_t *mrp_resource_client_find_set(mrp_resource_client_t *client,
                                                 uint32_t resource_set_id)
{
    /* local: returns a proxy resource set object */

    mrp_list_hook_t *entry, *n;
    mrp_resource_set_t *rset;

    mrp_debug("%p, %d", client, resource_set_id);

    if (client) {
        mrp_list_foreach(&client->resource_sets, entry, n) {
            rset = mrp_list_entry(entry, mrp_resource_set_t, client.list);

            if (resource_set_id == rset->id)
                return rset;
        }
    }

    mrp_debug("not found");
    return NULL;
}


const char **mrp_application_class_get_all_names(uint32_t n_items,
        const char **buf)
{
    uint32_t i;

    /* local: returns values that were retrieved during initialization */

    mrp_debug("%d, %p", n_items, buf);

    if (!global_ctx || !global_ctx->classes)
        return NULL;

    if (buf) {
        if (n_items < (unsigned) (global_ctx->classes->num_strings + 1))
            return NULL;
    }
    else {
        n_items = global_ctx->classes->num_strings;
        if (!(buf = mrp_allocz(sizeof(const char *) * (n_items + 1)))) {
            mrp_log_error("Memory alloc failure. Can't get class names");
            return NULL;
        }
    }

    for (i = 0; i < n_items; i++) {
        buf[i] = global_ctx->classes->strings[i];
    }

    buf[i] = NULL;

    return buf;
}


const char **mrp_zone_get_all_names(uint32_t n_items, const char **buf)
{
    /* local: returns values that were retrieved during initialization */

    /* We assume that slave Murphy will just live in one zone, and we'll
     * return the (configured) zone here. */

    mrp_debug("%d, %p", n_items, buf);

    if (!global_ctx || !global_ctx->zone)
        return NULL;

    if (buf) {
        if (n_items < 1 + 1)
            return NULL;
    }
    else {
        n_items = 1;
        if (!(buf = mrp_allocz(sizeof(const char *) * (n_items + 1)))) {
            mrp_log_error("Memory alloc failure. Can't get zone names");
            return NULL;
        }
    }

    buf[0] = global_ctx->zone;
    buf[1] = NULL;

    return buf;
}


const char **mrp_resource_definition_get_all_names(uint32_t n_items,
        const char **buf)
{
    /* local: returns values that were retrieved during initialization */

    uint32_t i = 0;
    mrp_resource_def_t *rdef;

    mrp_debug("%d, %p", n_items, buf);

    if (!global_ctx || !global_ctx->defs)
        return NULL;

    rdef = global_ctx->defs;

    if (buf) {
        if (n_items == 0)
            return NULL;
    }
    else {
        n_items = global_ctx->num_defs;
        if (!(buf = mrp_allocz(sizeof(const char *) * (n_items + 1)))) {
            mrp_debug("Memory alloc failure. Can't get all res def names");
            return NULL;
        }
    }

    while (i < n_items && rdef->name) {
        mrp_debug("%s", rdef->name);
        buf[i] = rdef->name;
        rdef++;
        i++;
    }

    if (i < n_items) {
        buf[i] = NULL;
    }

    return buf;
}


uint32_t mrp_resource_definition_get_resource_id_by_name(
        const char *resource_name)
{
    /* local: returns a value that was retrieved during initialization */

    mrp_resource_def_t *def =
            mrp_resource_definition_find_by_name(resource_name);

    mrp_debug("%s", resource_name);

    if (!def) {
        mrp_debug("error");
        return MRP_RESOURCE_ID_INVALID;
    }

    return def->id;
}


mrp_attr_t *mrp_resource_definition_read_all_attributes(uint32_t resource_id,
        uint32_t n_attrs, mrp_attr_t *buf)
{
    /* local: returns values that were retrieved during initialization */

    mrp_resource_def_t *rdef = NULL, *iter;
    mrp_attr_def_t *attr_defs;
    mrp_attr_t *attr;
    bool needs_free = FALSE;

    mrp_debug("%d, %d, %p", resource_id, n_attrs, buf);

    if (!global_ctx || !global_ctx->defs)
        return NULL;

    iter = global_ctx->defs;

    while (iter->name) {
        if (iter->id == resource_id) {
            rdef = iter;
            break;
        }
        iter++;
    }

    if (!rdef)
        return NULL;

    attr_defs = mrp_htbl_lookup(global_ctx->resource_names_to_attribute_defs,
            (void *) rdef->name);

    /* convert attribute definitions to attributes */

    if (n_attrs == 0) {
        buf = mrp_alloc(n_attrs * sizeof(MAX_ATTRS+1));
        needs_free = TRUE;
        n_attrs = MAX_ATTRS;
    }

    attr = &buf[0];

    while (attr_defs->name && n_attrs != 0) {
        attr->type = attr_defs->type;
        attr->name = attr_defs->name;

        switch (attr_defs->type) {
            case mqi_string:
                attr->value.string = attr_defs->value.string;
                break;
            case mqi_unsignd:
                attr->value.unsignd = attr_defs->value.unsignd;
                break;
            case mqi_integer:
                attr->value.integer = attr_defs->value.integer;
                break;
            case mqi_floating:
                attr->value.floating = attr_defs->value.floating;
                break;
            default:
                if (needs_free)
                    mrp_free(buf);
                return NULL;
        }

        n_attrs--;
        attr_defs++;
        attr++;
    }

    if (n_attrs != 0)
        memset(attr, 0, sizeof(*attr));

    return buf;
}


int mrp_application_class_add_resource_set(const char *class_name,
        const char *zone_name, mrp_resource_set_t *resource_set,
        uint32_t request_id)
{
    /* remote: assign resource to application class */

    resource_proxy_resource_set_t *prset;

    mrp_debug("%s, %s, %p, %d", class_name, zone_name, resource_set,
            request_id);

    prset = mrp_htbl_lookup(global_ctx->rs_to_proxy_rs,
            resource_set);
    if (!prset) {
        mrp_debug("error");
        return -1;
    }

    /* resource_set->request.id = request_id; */

    /* TODO: mangle zone name */
    create_resource_set_request(global_ctx, prset, class_name, zone_name,
            request_id);

    return 0;
}


#define PRIORITY_MAX  ((uint32_t)1 << MRP_KEY_PRIORITY_BITS)

mrp_resource_set_t *mrp_resource_set_create(mrp_resource_client_t *client,
        bool auto_release, bool dont_wait, uint32_t priority,
        mrp_resource_event_cb_t event_cb, void *user_data)
{
    /* local: create a proxy resource set object */

    mrp_resource_set_t *rset = NULL;
    resource_proxy_resource_set_t *prset = NULL;

    mrp_debug("%p, %d, %d, %d, %p, %p", client, auto_release, dont_wait,
            priority, event_cb, user_data);

    if (priority >= PRIORITY_MAX)
        priority = PRIORITY_MAX - 1;

    if (!global_ctx)
        return NULL;

    rset = mrp_allocz(sizeof(*rset));
    if (!rset)
        goto error;

    prset = mrp_allocz(sizeof(*prset));
    if (!prset)
        goto error;

    rset->state = mrp_resource_no_request;

    mrp_list_init(&prset->operation_queue);

    rset->auto_release.client = auto_release;
    rset->auto_release.current = auto_release;
    rset->dont_wait.client = dont_wait;
    rset->dont_wait.current = dont_wait;

    mrp_list_init(&rset->resource.list);
    mrp_list_init(&rset->client.list);
    rset->resource.share = false;

    mrp_list_append(&client->resource_sets, &rset->client.list);
    rset->client.ptr = client;
    rset->client.reqno = MRP_RESOURCE_REQNO_INVALID;

    mrp_list_init(&rset->class.list);
    rset->class.priority = priority;

    rset->event = event_cb;
    rset->user_data = user_data;

    prset->rs = rset;
    prset->rs->id = global_ctx->next_rset_id++;
    prset->id = 0; /* this will be updated from master */

    if (!mrp_htbl_insert(global_ctx->rs_to_proxy_rs, rset, prset))
        goto error;

    mrp_debug("create resource set %d (%p, proxy %p)", rset->id, rset, prset);

    return rset;

 error:
    mrp_debug("error");

    if (rset) {
        mrp_htbl_remove(global_ctx->rs_to_proxy_rs, rset, FALSE);
        mrp_free(rset);
    }

    mrp_free(prset);

    return NULL;
}

#undef PRIORITY_MAX


void mrp_resource_set_destroy(mrp_resource_set_t *resource_set)
{
    /* remote: destroy the proxy resource set object */

    resource_proxy_resource_set_t *prset = NULL;

    mrp_debug("%p", resource_set);

    if (!global_ctx) {
        return;
    }

    prset = mrp_htbl_lookup(global_ctx->rs_to_proxy_rs, resource_set);
    if (!prset) {
        free_resource_set(resource_set, global_ctx);
        return;
    }

    mrp_debug("prset: %p (%u, %u)", prset, prset->id, prset->seqno);

    mrp_htbl_remove(global_ctx->seqnos_to_proxy_rs, u_to_p(prset->seqno), FALSE);
    mrp_htbl_remove(global_ctx->ids_to_proxy_rs, u_to_p(prset->id), FALSE);
    mrp_htbl_remove(global_ctx->rs_to_proxy_rs, prset->rs, FALSE);

    destroy_resource_set_request(global_ctx, prset);
    free_proxy_resource_set(global_ctx, prset);

    return;
}


uint32_t mrp_get_resource_set_id(mrp_resource_set_t *resource_set)
{
    /* local: get proxy resource set id after application class assignment  */

    mrp_debug("%p", resource_set);

    if (!resource_set) {
        mrp_debug("error");
        return 0;
    }

    mrp_debug("return %u", resource_set->id);
    return resource_set->id;
}


mrp_resource_state_t mrp_get_resource_set_state(mrp_resource_set_t
        *resource_set)
{
    /* local: return proxy resource set state */

    mrp_debug("%p", resource_set);

    if (!resource_set) {
        mrp_debug("error");
        return mrp_resource_release;
    }

    mrp_debug("return %u", resource_set->state);
    return resource_set->state;
}


mrp_resource_mask_t mrp_get_resource_set_grant(mrp_resource_set_t *resource_set)
{
    /* local: return proxy resource set grant status */

    mrp_debug("%p", resource_set);

    if (!resource_set)
        return 0;

    return resource_set->resource.mask.grant;
}


mrp_resource_mask_t mrp_get_resource_set_advice(
        mrp_resource_set_t *resource_set)
{
    /* local: return proxy resource set advice status */

    mrp_debug("%p", resource_set);

    if (!resource_set)
        return 0;

    return resource_set->resource.mask.advice;
}


mrp_resource_client_t * mrp_get_resource_set_client(
        mrp_resource_set_t *resource_set)
{
    /* local: return the proxy resource set client associated with the proxy
     * resource set */

    mrp_debug("%p", resource_set);

    return resource_set->client.ptr;
}


int mrp_resource_set_add_resource(mrp_resource_set_t *resource_set,
        const char *resource_name, bool shared, mrp_attr_t *attribute_list,
        bool mandatory)
{
    /* local: link resource name to proxy resource set */

    uint32_t mask;
    mrp_resource_t *res;
    uint32_t rsetid;
    bool autorel;

    mrp_debug("%p, %s, %d, %p, %d", resource_set, resource_name, shared,
            attribute_list, mandatory);

    rsetid  = resource_set->id;
    autorel = resource_set->auto_release.client;

    if (!(res = mrp_resource_create(resource_name, rsetid, autorel, shared,
            attribute_list))) {
        mrp_log_error("Can't add resource '%s' name to resource set %u",
                      resource_name, resource_set->id);
        return -1;
    }

    mask = mrp_resource_get_mask(res);

    resource_set->resource.mask.all       |= mask;
    resource_set->resource.mask.mandatory |= mandatory ? mask : 0;
    resource_set->resource.share          |= mrp_resource_is_shared(res);

    mrp_list_append(&resource_set->resource.list, &res->list);

    return 0;
}


mrp_attr_t * mrp_resource_set_read_attribute(mrp_resource_set_t *resource_set,
        const char *resource_name, uint32_t attribute_index, mrp_attr_t *buf)
{
    /* local: proxy resource set -> resource -> attribute map */

    mrp_resource_t *res;

    mrp_debug("%p, %s, %d, %p", resource_set, resource_name, attribute_index,
            buf);

    if (!(res = find_resource_by_name(resource_set, resource_name))) {
        mrp_debug("error");
        return NULL;
    }

    return mrp_resource_read_attribute(res, attribute_index, buf);
}


mrp_attr_t *mrp_resource_set_read_all_attributes(
        mrp_resource_set_t *resource_set, const char *resource_name,
        uint32_t buflen, mrp_attr_t *buf)
{
    /* local: proxy resource set -> resource -> attribute map */

    mrp_resource_t *res;

    mrp_debug("%p, %s, %d, %p", resource_set, resource_name, buflen, buf);

    if (!(res = find_resource_by_name(resource_set, resource_name))) {
        mrp_debug("error");
        return NULL;
    }

    return mrp_resource_read_all_attributes(res, buflen, buf);
}


int mrp_resource_set_write_attributes(mrp_resource_set_t *resource_set,
        const char *resource_name, mrp_attr_t *attribute_list)
{
    /* remote: update attributes */

    mrp_resource_t *res;

    mrp_debug("%p, %s, %p", resource_set, resource_name, attribute_list);

    if (!(res = find_resource_by_name(resource_set, resource_name))) {
        mrp_debug("error");
        return -1;
    }

    if (mrp_resource_write_attributes(res, attribute_list) < 0) {
        mrp_debug("error");
        return -1;
    }

    /* TODO: update remote attributes */

    return 0;
}


void mrp_resource_set_acquire(mrp_resource_set_t *resource_set,
        uint32_t request_id)
{
    /* remote: acquire via proxy object */

    resource_proxy_resource_set_t *prset;

    mrp_debug("%p, %d", resource_set, request_id);

    if (!global_ctx) {
        mrp_debug("error");
    }

    resource_set->state = mrp_resource_acquire;

    prset = mrp_htbl_lookup(global_ctx->rs_to_proxy_rs,
            resource_set);
    if (!prset) {
        mrp_debug("error");
        return;
    }

    acquire_resource_set_request(global_ctx, prset, request_id);
}


void mrp_resource_set_release(mrp_resource_set_t *resource_set,
        uint32_t request_id)
{
    /* remote: release via proxy object */

    resource_proxy_resource_set_t *prset;

    mrp_debug("%p, %d", resource_set, request_id);

    if (!global_ctx) {
        mrp_debug("error");
    }

    resource_set->state = mrp_resource_release;
    /* resource_set->request.id = request_id; */

    prset = mrp_htbl_lookup(global_ctx->rs_to_proxy_rs,
            resource_set);
    if (!prset) {
        mrp_debug("error");
        return;
    }

    release_resource_set_request(global_ctx, prset, request_id);
}


mrp_resource_t * mrp_resource_set_iterate_resources(
        mrp_resource_set_t *resource_set, void **it)
{
    /* local: process proxy resource set */

    mrp_list_hook_t *list, *entry;

    mrp_debug("%p, %p", resource_set, it);

    list  = &resource_set->resource.list;
    entry = (*it == NULL) ? list->next : (mrp_list_hook_t *) *it;

    if (entry == list)
        return NULL;

    *it = entry->next;

    return mrp_list_entry(entry, mrp_resource_t, list);
}


uint32_t mrp_resource_get_id(mrp_resource_t *resource)
{
    /* local: get id from initial configuration */

    if (!resource || !resource->def)
        return 0;

    mrp_debug("%p", resource);

    return resource->def->id;
}


const char *mrp_resource_get_name(mrp_resource_t *resource)
{
    /* local: get name from proxy object */

    mrp_debug("%p", resource);

    if (resource && resource->def && resource->def->name) {
        return resource->def->name;
    }

    return "<unknown resource>";
}


mrp_resource_mask_t mrp_resource_get_mask(mrp_resource_t *resource)
{
    /* local: get map from proxy object */

    mrp_resource_mask_t mask = 0;

    mrp_debug("%p", resource);

    if (resource && resource->def) {
        mask = (mrp_resource_mask_t) 1 << resource->def->id;
    }

    mrp_debug("mask for %s: 0x%08x", resource->def->name, mask);

    return mask;
}


bool mrp_resource_is_shared(mrp_resource_t *resource)
{
    /* local: get shared data from proxy object */

    mrp_debug("%p", resource);

    if (!resource)
        return FALSE;

    return resource->shared;
}


struct resource_table_iter_s {
    uint32_t id;
    mrp_resource_set_t *rset;
};


int resource_table_iter_cb(void *key, void *object, void *user_data)
{
    struct resource_table_iter_s *i = user_data;
    mrp_resource_set_t *rset = key;

    MRP_UNUSED(object);

    if (rset->id == i->id) {
        i->rset = rset;
        return MRP_HTBL_ITER_STOP;
    }

    return MRP_HTBL_ITER_MORE;
}


mrp_resource_set_t *mrp_resource_set_find_by_id(uint32_t id)
{
    /* local: return proxy object */

    struct resource_table_iter_s i;

    mrp_debug("%d", id);

    i.id = id;
    i.rset = NULL;

    if (!global_ctx)
        return NULL;

    mrp_htbl_foreach(global_ctx->rs_to_proxy_rs, resource_table_iter_cb, &i);

    return i.rset;
}


mrp_attr_t *mrp_resource_set_get_attribute_by_name(
        mrp_resource_set_t *resource_set, const char *resource_name,
        const char *attribute_name)
{
    /* local: proxy object data */

    mrp_attr_t *attr = NULL, *attrs;
    uint32_t res_id;
    mrp_attr_t attr_buf[128];
    uint32_t attr_idx = 0;

    mrp_debug("%p, %s, %s", resource_set, resource_name, attribute_name);

    memset(attr_buf, 0, sizeof(attr_buf));

    res_id = mrp_resource_definition_get_resource_id_by_name(resource_name);
    attrs = mrp_resource_definition_read_all_attributes(res_id, 128, attr_buf);

    if (!attrs)
        return NULL;

    while (attrs->name != NULL) {
        if (strcmp(attrs->name, attribute_name) == 0) {

            mrp_attr_t *buf = mrp_allocz(sizeof(mrp_attr_t));
            mrp_resource_set_read_attribute(resource_set, resource_name,
                    attr_idx, buf);

            attr = buf;

            break;
        }
        attr_idx++;
        attrs++;
    }

    return attr;
}


void mrp_resource_set_free_attribute(mrp_attr_t *attr)
{
    /* local: proxy object data */

    mrp_debug("%p", attr);

    if (!attr)
        return;

    if (attr->type == mqi_string)
        mrp_free((void *) attr->value.string);

    mrp_free(attr);
}



resource_proxy_global_context_t *mrp_create_resource_proxy(mrp_mainloop_t *ml,
        const char *master_address, const char *zone)
{
    resource_proxy_global_context_t *ctx = initialize_ctx();

    if (!ctx)
        return NULL;

    mrp_debug("addr: %s", master_address);

    if (connect_to_master(ctx, master_address, ml) < 0) {
        mrp_debug("connecing to master Murphy failed");
    }

    ctx->zone = mrp_strdup(zone);

    /* get the initial class and zone values */

    resource_proxy_get_initial_values(ctx);

    return ctx;
}


void mrp_destroy_resource_proxy(resource_proxy_global_context_t *ctx)
{
    mrp_debug("ctx: %p", ctx);

    if (!ctx)
        return;

    /* destroy all clients */

    mrp_htbl_destroy(ctx->clients_to_proxy_clients, TRUE);
    mrp_htbl_destroy(ctx->resource_names_to_attribute_defs, TRUE);
    mrp_htbl_destroy(ctx->ids_to_proxy_rs, FALSE);
    mrp_htbl_destroy(ctx->seqnos_to_proxy_rs, FALSE);
    mrp_htbl_destroy(ctx->rs_to_proxy_rs, FALSE);

    /* disconnect */

    if (ctx->transport) {
        mrp_transport_disconnect(ctx->transport);
        mrp_transport_destroy(ctx->transport);
    }

    /* destroy context */

    mrp_free(ctx->zone);

    mrp_free(ctx);

    return;
}


/* called by plugin-resource-native -- should have no place here */

void mrp_resource_configuration_init(void) { }


/* these also have no place here, but console crashes if these are missing */

int mrp_application_class_print(char *buf, int len, bool with_rsets)
{
    MRP_UNUSED(with_rsets);

    snprintf(buf, len,
            "NIH. Also, console printouts should have no place"
            "in resource library.\n");

    return 0;
}


int mrp_resource_owner_print(char *buf, int len)
{
    snprintf(buf, len,
            "NIH. If somewhere, these should be implemented in plugins"
            "that can query database and resource library.\n");

    return 0;
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
