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

#include "client.h"
#include <errno.h>
#include <resource/protocol.h>

static int proxy_resource_add_to_prset_queue(
        resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset,
        enum resource_proxy_action action, uint32_t request_id);

static int proxy_resource_process_queue(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset);

bool fetch_resource_set_state(mrp_msg_t *msg, void **pcursor,
                                     uint16_t *pstate)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_RESOURCE_STATE || type != MRP_MSG_FIELD_UINT16)
    {
        *pstate = 0;
        return false;
    }

    *pstate = value.u16;
    return true;
}


bool fetch_resource_set_mask(mrp_msg_t *msg, void **pcursor,
                                    int mask_type, uint32_t *pmask)
{
    uint16_t expected_tag;
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    switch (mask_type) {
    case 0:    expected_tag = RESPROTO_RESOURCE_GRANT;     break;
    case 1:   expected_tag = RESPROTO_RESOURCE_ADVICE;    break;
    default:       /* don't know what to fetch */              return false;
    }

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != expected_tag || type != MRP_MSG_FIELD_UINT32)
    {
        *pmask = 0;
        return false;
    }

    *pmask = value.u32;
    return true;
}


bool fetch_resource_set_id(mrp_msg_t *msg, void **pcursor, uint32_t *pid)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_RESOURCE_SET_ID || type != MRP_MSG_FIELD_UINT32)
    {
        *pid = 0;
        return false;
    }

    *pid = value.u32;
    return true;
}


bool fetch_mrp_str_array(mrp_msg_t *msg, void **pcursor,
                   uint16_t expected_tag, proxy_string_array_t **parr)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != expected_tag || type != MRP_MSG_FIELD_ARRAY_OF(STRING))
    {
        *parr = proxy_str_array_dup(0, NULL);
        return false;
    }

    if (!(*parr = proxy_str_array_dup(size, (const char **)value.astr)))
        return false;

    return true;
}


bool fetch_seqno(mrp_msg_t *msg, void **pcursor, uint32_t *pseqno)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_SEQUENCE_NO || type != MRP_MSG_FIELD_UINT32)
    {
        *pseqno = 0;
        return false;
    }

    *pseqno = value.u32;
    return true;
}


bool fetch_request(mrp_msg_t *msg, void **pcursor, uint16_t *preqtype)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_REQUEST_TYPE || type != MRP_MSG_FIELD_UINT16)
    {
        *preqtype = 0;
        return false;
    }

    *preqtype = value.u16;
    return true;
}


bool fetch_status(mrp_msg_t *msg, void **pcursor, int16_t *pstatus)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_REQUEST_STATUS || type != MRP_MSG_FIELD_SINT16)
    {
        *pstatus = EIO;
        return false;
    }

    *pstatus = value.s16;
    return true;
}


bool fetch_attribute_array(mrp_msg_t *msg, void **pcursor,
                                 size_t dim, mrp_attr_t *arr,
                                 int *n_arr)
{
    mrp_attr_t *attr;
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;
    size_t i;
    *n_arr = 0;

    i = 0;

    while (mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size)) {
        if (i >= (dim-1)) {
            /* need to save one space for the NULL termination */
            break;
        }

        if (tag == RESPROTO_SECTION_END && type == MRP_MSG_FIELD_UINT8)
            break;

        if (tag  != RESPROTO_ATTRIBUTE_NAME ||
            type != MRP_MSG_FIELD_STRING ||
            i >= dim - 1) {
            return false;
        }

        attr = arr + i++;
        attr->name = value.str;

        if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
            tag != RESPROTO_ATTRIBUTE_VALUE) {
            return false;
        }

        switch (type) {
        case MRP_MSG_FIELD_STRING:
            attr->type = mqi_string;
            attr->value.string = value.str;
            break;
        case MRP_MSG_FIELD_SINT32:
            attr->type = mqi_integer;
            attr->value.integer = value.s32;
            break;
        case MRP_MSG_FIELD_UINT32:
            attr->type = mqi_unsignd;
            attr->value.unsignd = value.u32;
            break;
        case MRP_MSG_FIELD_DOUBLE:
            attr->type = mqi_floating;
            attr->value.floating = value.dbl;
            break;
        default:
            return false;
        }
    }

    memset(arr + i, 0, sizeof(mrp_attr_t));

    *n_arr = i;

    return TRUE;
}


bool fetch_resource_name(mrp_msg_t *msg, void **pcursor,
                                const char **pname)
{
    uint16_t tag;
    uint16_t type;
    mrp_msg_value_t value;
    size_t size;

    if (!mrp_msg_iterate(msg, pcursor, &tag, &type, &value, &size) ||
        tag != RESPROTO_RESOURCE_NAME || type != MRP_MSG_FIELD_STRING)
    {
        *pname = "<unknown>";
        return FALSE;
    }

    *pname = value.str;
    return TRUE;
}



static void recvfrom_msg(mrp_transport_t *transp, mrp_msg_t *msg,
                         mrp_sockaddr_t *addr, socklen_t addrlen,
                         void *user_data)
{
    resource_proxy_global_context_t *ctx = user_data;
    uint32_t seqno;
    uint16_t request_type;
    void *cursor = NULL;
    resource_proxy_resource_set_t *prset = NULL;

    MRP_UNUSED(transp);
    MRP_UNUSED(addr);
    MRP_UNUSED(addrlen);

    mrp_debug("recv msg");

    /* parse the message */

    if (!fetch_seqno(msg, &cursor, &seqno) ||
            !fetch_request(msg, &cursor, &request_type)) {
        mrp_debug("Failed to parse message");
        return;
    }

    switch (request_type) {
        case RESPROTO_QUERY_CLASSES:
        {
            int16_t status;
            proxy_string_array_t *arr = NULL;

            mrp_debug("RESPROTO_QUERY_CLASSES, seqno %d", seqno);

            if (!fetch_status(msg, &cursor, &status) || (status == 0 &&
                !fetch_mrp_str_array(msg, &cursor, RESPROTO_CLASS_NAME, &arr)))
            {
                mrp_debug("ignoring malformed response to class query");
                return;
            }

            if (status != 0) {
                mrp_debug("class query failed with error code %u", status);
                proxy_str_array_free(arr);
                return;
            }

            ctx->classes = arr;

            ctx->queried_classes = TRUE;

            if (ctx->queried_resources) {
                /* got all information */
                proxy_notify_clients(ctx, RP_CONNECTED);
            }
            break;
        }
        case RESPROTO_QUERY_RESOURCES:
        {
            int16_t status;
            int dim = 0;
            mrp_resource_def_t *rdef = ctx->defs;
            mrp_attr_t attrs[128];
            mrp_attr_def_t *copy;
            const char *resource_name;

            mrp_debug("RESPROTO_QUERY_RESOURCES, seqno %d", seqno);

            if (!fetch_status(msg, &cursor, &status)) {
                mrp_debug("Failed to parse message: status");
                return;
            }

            if (status != 0) {
                mrp_debug("resource query failed with error code %u", status);
                return;
            }

            while (fetch_resource_name(msg, &cursor, &resource_name)) {
                int n_attrs = 0;

                rdef[dim].name = mrp_strdup(resource_name);

                mrp_debug("got resource name '%s'", rdef[dim].name);

                if (!fetch_attribute_array(msg, &cursor, 128,
                        attrs, &n_attrs)) {
                    mrp_debug("Failed to parse attribute array");
                    return;
                }

                if (!(copy = proxy_attribute_def_array_dup(n_attrs, attrs))) {
                    mrp_debug("Failed to duplicate attributes");
                    return;
                }

                /* initialize attribute definitions with this data */

                mrp_htbl_insert(ctx->resource_names_to_attribute_defs,
                        (void *) rdef[dim].name, copy);

                rdef[dim].nattr = n_attrs;
                rdef[dim].id = dim; /* assumption: resources are in order */

                if (dim < MAX_RESOURCES) {
                    dim++;
                }
                else {
                    mrp_debug("Error: too many resources");
                    return;
                }
                if (dim < MAX_RESOURCES) {
                    rdef[dim].name = NULL;
                    ctx->num_defs = dim;
                }

                ctx->queried_resources = TRUE;
                if (ctx->queried_classes) {
                    /* got all information */
                    proxy_notify_clients(ctx, RP_CONNECTED);
                }
            }
            if (dim >= 128)
                dim = 128 - 1;

            /* NULL terminate the resource definition array */
            memset(&rdef[dim], 0, sizeof(mrp_resource_def_t));
            break;
        }
        case RESPROTO_CREATE_RESOURCE_SET:
            {
                int16_t status;
                uint32_t rset_id;

                mrp_debug("RESPROTO_CREATE_RESOURCE_SET, seqno %d", seqno);

                prset = mrp_htbl_lookup(ctx->seqnos_to_proxy_rs, u_to_p(seqno));
                if (!prset) {
                    mrp_debug("Failed to find resource set");
                    return;
                }

                if (!fetch_status(msg, &cursor, &status)) {
                    mrp_debug("Failed to parse message: status");
                    return;
                }

                if (status < 0) {
                    mrp_debug("Request failed");
                    /* TODO: all requests made with this resource set must be
                     * errors from now on. */
                    return;
                }

                if (!fetch_resource_set_id(msg, &cursor, &rset_id)) {
                    mrp_debug("Failed to parse message: resource set id");
                    return;
                }

                mrp_debug("CREATE resp: rset id: %u", rset_id);

                prset->id = rset_id;
                prset->initialized = TRUE;

                if (!mrp_htbl_insert(ctx->ids_to_proxy_rs,
                        u_to_p(prset->id), prset))
                    return;

                mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(seqno), FALSE);
                break;
            }
        case RESPROTO_DESTROY_RESOURCE_SET:
            {
                mrp_debug("RESPROTO_DESTROY_RESOURCE_SET, seqno %d", seqno);

                mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(seqno), FALSE);
                return;
            }
        case RESPROTO_ACQUIRE_RESOURCE_SET:
            {
                uint32_t rset_id;
                int16_t status;

                mrp_debug("RESPROTO_ACQUIRE_RESOURCE_SET, seqno %d", seqno);

                prset = mrp_htbl_lookup(ctx->seqnos_to_proxy_rs, u_to_p(seqno));
                if (!prset) {
                    mrp_debug("Failed to find resource set");
                    return;
                }

                if (!fetch_resource_set_id(msg, &cursor, &rset_id) ||
                        !fetch_status(msg, &cursor, &status)) {
                    mrp_debug("Error parsing message");
                    return;
                }

                mrp_debug("ACQUIRE resp: rset id: %u, status: %u", rset_id,
                        status);
#if 0
                mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(seqno), FALSE);
#endif
                break;
            }
        case RESPROTO_RELEASE_RESOURCE_SET:
            {
                uint32_t rset_id;
                int16_t status;

                mrp_debug("RESPROTO_RELEASE_RESOURCE_SET, seqno %d", seqno);

                prset = mrp_htbl_lookup(ctx->seqnos_to_proxy_rs, u_to_p(seqno));
                if (!prset) {
                    mrp_debug("Failed to find resource set");
                    return;
                }

                if (!fetch_resource_set_id(msg, &cursor, &rset_id) ||
                        !fetch_status(msg, &cursor, &status)) {
                    mrp_debug("Error parsing message");
                    return;
                }

                mrp_debug("RELEASE resp: rset id: %u, status: %u", rset_id,
                        status);
#if 0
                mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(seqno), FALSE);
#endif
                break;
            }
        case RESPROTO_RESOURCES_EVENT:
            {
                uint32_t rset_id;

                mrp_resproto_state_t status;
                uint16_t status_;
                uint32_t grant;
                uint32_t advice;

                uint16_t tag;
                uint16_t type;
                mrp_msg_value_t value;
                size_t size;

                mrp_debug("RESPROTO_RESOURCES_EVENT, seqno %d", seqno);

                if (seqno) {
                    prset = mrp_htbl_lookup(ctx->seqnos_to_proxy_rs,
                            u_to_p(seqno));
                    if (!prset) {
                        mrp_debug("Resource set not found by seqno");
                    }
                }

                if (!fetch_resource_set_id(msg, &cursor, &rset_id) ||
                        !fetch_resource_set_state(msg, &cursor, &status_) ||
                        !fetch_resource_set_mask(msg, &cursor, 0, &grant) ||
                        !fetch_resource_set_mask(msg, &cursor, 1, &advice)) {
                    mrp_debug("Failed to parse resource event message");
                    return;
                }

                status = status_;

                if (prset) {
                    /* saniity check */
                    if (rset_id != prset->id) {
                        mrp_debug("resource set mismatch: (msg: %u vs map: %u)",
                                rset_id, prset->id);
                        /* The protocol is really strange here. We need to get
                           the real resource id from the event message instead
                           of waiting for create request callback. */
                        if (prset->id == 0) {
                            mrp_debug("updating resource set id");
                            prset->id = rset_id;
                        }
                    }
                }

                mrp_debug("event for rset %u: (%d, %u, %u)", rset_id, status,
                        grant, advice);

                if (!prset) {
                    prset = mrp_htbl_lookup(ctx->ids_to_proxy_rs,
                            u_to_p(rset_id));
                    if (!prset) {
                        mrp_debug("Resource set not found by id");
                        return;
                    }
                }

                mrp_debug("found resource set %d, (%p, proxy %p)", prset->id,
                        prset->rs, prset);

                prset->rs->resource.mask.grant = grant;
                prset->rs->resource.mask.advice = advice;

                if (grant)
                    prset->rs->state = mrp_resource_acquire;
                else
                    prset->rs->state = mrp_resource_release;

                while (mrp_msg_iterate(msg, &cursor, &tag, &type, &value,
                        &size)) {
                    const char *name;
                    mrp_attr_t attrs[128];
                    int n_attrs;

                    if ((tag != RESPROTO_RESOURCE_ID ||
                            type != MRP_MSG_FIELD_UINT32) ||
                            !fetch_resource_name(msg, &cursor, &name)) {
                        mrp_debug("Failed to parse resource from message");
                        return;
                    }

                    if (!fetch_attribute_array(msg, &cursor, 128, attrs,
                            &n_attrs)) {
                        mrp_debug("failed to parse attributes from message");
                        return;
                    }

                    /* TODO: make sure this is not written back to server */
                    mrp_resource_set_write_attributes(prset->rs, name, attrs);
                }

                if (prset->rs->event) {
                    mrp_debug("calling event handler");

                    /* request.id needs to be tied to the particular
                     * operation that we were processing at the moment */

                    mrp_debug("calling event handler: request_id %d, rs %p",
                            prset->rs->request.id, prset->rs);

                    prset->rs->event(prset->rs->request.id, prset->rs,
                            prset->rs->user_data);
                }

                return;
            }
        default:
            mrp_debug("Unhandled resource protocol request %d, seqno %d",
                    request_type, seqno);
            return;
    }

    if (prset && prset->in_progress) {
        mrp_debug("request no longer in progress");
        prset->rs->request.id = 0;
        prset->in_progress = FALSE;
        /* Go through the queue of commands relating to this resource set.
           The reason for this is that the resource set id is not yet known
           after the "create" call before the resource set callback has been
           processed. This means that "acquire" and "release" commands might be
           sent with wrong ID. */
        proxy_resource_process_queue(ctx, prset);
    }
}


static void recv_msg(mrp_transport_t *t, mrp_msg_t *msg, void *user_data)
{
    return recvfrom_msg(t, msg, NULL, 0, user_data);
}


void closed_evt(mrp_transport_t *transp, int error, void *user_data)
{
    /* TODO: reconnect later */

    resource_proxy_global_context_t *ctx = user_data;

    MRP_UNUSED(transp);
    MRP_UNUSED(error);

    proxy_notify_clients(ctx, RP_DISCONNECTED);

    mrp_debug("closed");
}


void disconnect_from_master(resource_proxy_global_context_t *ctx)
{
    if (ctx->connected == FALSE || !ctx->transport)
        return;

    mrp_transport_disconnect(ctx->transport);
    mrp_transport_destroy(ctx->transport);
    ctx->transport = NULL;

    ctx->connected = FALSE;
}


int connect_to_master(resource_proxy_global_context_t *ctx, const char *addr,
        mrp_mainloop_t *ml)
{
    int alen;
    const char *type;

    static mrp_transport_evt_t evt = {
        { .recvmsg     = recv_msg },
        { .recvmsgfrom = recvfrom_msg },
        .closed        = closed_evt,
        .connection    = NULL
    };

    /* connect to Murphy */

    mrp_debug("%p, %p", ctx, ml);

    alen = mrp_transport_resolve(NULL, addr, &ctx->addr, sizeof(ctx->addr),
            &type);

    ctx->transport = mrp_transport_create(ml, type, &evt, ctx, 0);

    if (!ctx->transport)
        goto error;

    if (!mrp_transport_connect(ctx->transport, &ctx->addr, alen))
        goto error;

    ctx->connected = TRUE;

    return 0;

error:
    mrp_debug("error");

    if (ctx->transport) {
        mrp_transport_disconnect(ctx->transport);
        mrp_transport_destroy(ctx->transport);
    }

    return -1;
}


int get_application_classes_request(resource_proxy_global_context_t *ctx)
{
    mrp_msg_t *msg = NULL;

    if (!ctx->connected)
        goto error;

    msg = mrp_msg_create(RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, 0,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16, RESPROTO_QUERY_CLASSES,
            RESPROTO_MESSAGE_END);

    if (!msg)
        goto error;

    if (!mrp_transport_send(ctx->transport, msg))
        goto error;

    mrp_msg_unref(msg);
    return 0;

error:
    mrp_debug("error");
    mrp_msg_unref(msg);
    return -1;
}


int get_available_resources_request(resource_proxy_global_context_t *ctx)
{
    mrp_msg_t *msg = NULL;

    if (!ctx->connected)
        goto error;

    msg = mrp_msg_create(RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, 0,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16,
                    RESPROTO_QUERY_RESOURCES,
            RESPROTO_MESSAGE_END);

    if (!msg)
        goto error;

    if (!mrp_transport_send(ctx->transport, msg))
        goto error;

    mrp_msg_unref(msg);
    return 0;

error:
    mrp_debug("error");
    mrp_msg_unref(msg);
    return -1;
}


int resource_proxy_get_initial_values(resource_proxy_global_context_t *ctx)
{
    if (get_available_resources_request(ctx) < 0 ||
            get_application_classes_request(ctx) < 0) {

        mrp_debug("error");
        return -1;
    }

    return 0;
}


static int release_resource_set(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    mrp_msg_t *msg = NULL;

    mrp_debug("%p, %p", ctx, prset);

    if (!ctx || !prset || !ctx->connected)
        return -1;

    prset->in_progress = TRUE;

    ctx->next_seqno++;

    msg = mrp_msg_create(
            RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, ctx->next_seqno,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16,
            RESPROTO_RELEASE_RESOURCE_SET,
            RESPROTO_RESOURCE_SET_ID, MRP_MSG_FIELD_UINT32, prset->id,
            RESPROTO_MESSAGE_END);

    if (!msg)
        return -1;

    mrp_htbl_insert(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), prset);

    if (!mrp_transport_send(ctx->transport, msg))
        goto error_remove_seqno;

    prset->seqno = ctx->next_seqno;
    mrp_msg_unref(msg);
    return 0;

error_remove_seqno:
    mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), FALSE);

    mrp_msg_unref(msg);
    return -1;
}


int release_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, uint32_t request_id)
{
    mrp_debug("%p, %p, %u", ctx, prset, request_id);

    if (!prset->in_progress) {
        prset->rs->request.id = request_id;
        return release_resource_set(ctx, prset);
    }
    else {
        mrp_debug("queuing the releasing of resource set %p (possible id %u)",
                prset, prset->id);
        proxy_resource_add_to_prset_queue(ctx, prset, RP_RELEASE_RSET,
                request_id);
    }

    return 0;
}


static int acquire_resource_set(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    mrp_msg_t *msg = NULL;

    mrp_debug("%p, %p", ctx, prset);

    if (!ctx || !prset || !ctx->connected)
        return -1;

    prset->in_progress = TRUE;
    ctx->next_seqno++;

    msg = mrp_msg_create(
            RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, ctx->next_seqno,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16,
            RESPROTO_ACQUIRE_RESOURCE_SET,
            RESPROTO_RESOURCE_SET_ID, MRP_MSG_FIELD_UINT32, prset->id,
            RESPROTO_MESSAGE_END);

    if (!msg)
        return -1;

    mrp_htbl_insert(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), prset);

    if (!mrp_transport_send(ctx->transport, msg))
        goto error_remove_seqno;

    prset->seqno = ctx->next_seqno;
    mrp_msg_unref(msg);
    return 0;

error_remove_seqno:
    mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), FALSE);

    mrp_msg_unref(msg);
    return -1;
}


int acquire_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, uint32_t request_id)
{
    mrp_debug("%p, %p, %u", ctx, prset, request_id);

    if (!prset->in_progress) {
        prset->rs->request.id = request_id;
        return acquire_resource_set(ctx, prset);
    }
    else {
        mrp_debug("queuing the acquisition of resource set %p (possible id %u)",
                prset, prset->id);
        proxy_resource_add_to_prset_queue(ctx, prset, RP_ACQUIRE_RSET,
                request_id);
    }

    return 0;
}


static int create_resource_set(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    mrp_msg_t *msg = NULL;
    uint32_t j;
    uint32_t rset_flags = 0;
    mrp_list_hook_t *entry, *n;
    mrp_attr_def_t *attr_defs;

    mrp_debug("%p, %p", ctx, prset);

    if (!ctx || !prset || !ctx->connected)
        return -1;

    if (prset->rs->auto_release.client)
        rset_flags |= RESPROTO_RSETFLAG_AUTORELEASE;

    prset->in_progress = TRUE;
    ctx->next_seqno++;

    msg = mrp_msg_create(
            RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, ctx->next_seqno,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16,
            RESPROTO_CREATE_RESOURCE_SET,
            RESPROTO_RESOURCE_FLAGS, MRP_MSG_FIELD_UINT32, rset_flags,
            RESPROTO_RESOURCE_PRIORITY, MRP_MSG_FIELD_UINT32, 0,
            RESPROTO_CLASS_NAME, MRP_MSG_FIELD_STRING, prset->class_name,
            RESPROTO_ZONE_NAME, MRP_MSG_FIELD_STRING, prset->zone_name,
            RESPROTO_MESSAGE_END);

    if (!msg)
        return -1;

    mrp_htbl_insert(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), prset);

    mrp_list_foreach(&prset->rs->resource.list, entry, n) {

        mrp_resource_t *res;
        uint32_t res_flags = 0;

        res = mrp_list_entry(entry, mrp_resource_t, list);

        if (res->shared)
            res_flags |= RESPROTO_RESFLAG_SHARED;

#if 0
        /* TODO */
        if (res->mandatory)
            res_flags |= RESPROTO_RESFLAG_MANDATORY;
#endif

        if (!mrp_msg_append(msg, RESPROTO_RESOURCE_NAME, MRP_MSG_FIELD_STRING,
                res->def->name))
            goto error_remove_seqno;

        if (!mrp_msg_append(msg, RESPROTO_RESOURCE_FLAGS, MRP_MSG_FIELD_UINT32,
                res_flags))
            goto error_remove_seqno;

        attr_defs = mrp_htbl_lookup(ctx->resource_names_to_attribute_defs,
                (void *) res->def->name);

        for (j = 0; j < res->def->nattr; j++) {
            mrp_attr_value_t *attr_value = &res->attrs[j];
            mrp_attr_def_t *attr_def = &attr_defs[j];
            const char *attr_name = attr_def->name;

            if (!mrp_msg_append(msg, RESPROTO_ATTRIBUTE_NAME,
                    MRP_MSG_FIELD_STRING, attr_name))
                goto error_remove_seqno;

            switch (attr_def->type) {
                case mqi_string:
                    if (!mrp_msg_append(msg, RESPROTO_ATTRIBUTE_VALUE,
                            MRP_MSG_FIELD_STRING, attr_value->string))
                        goto error_remove_seqno;
                    break;
                case mqi_integer:
                    if (!mrp_msg_append(msg, RESPROTO_ATTRIBUTE_VALUE,
                            MRP_MSG_FIELD_SINT32, attr_value->integer))
                        goto error_remove_seqno;
                    break;
                case mqi_unsignd:
                    if (!mrp_msg_append(msg, RESPROTO_ATTRIBUTE_VALUE,
                            MRP_MSG_FIELD_UINT32, attr_value->unsignd))
                        goto error_remove_seqno;
                    break;
                case mqi_floating:
                    if (!mrp_msg_append(msg, RESPROTO_ATTRIBUTE_VALUE,
                            MRP_MSG_FIELD_DOUBLE, attr_value->floating))
                        goto error_remove_seqno;
                    break;
                default:
                    break;
            }
        }

        if (!mrp_msg_append(msg, RESPROTO_SECTION_END, MRP_MSG_FIELD_UINT8, 0))
            goto error_remove_seqno;
    }

    if (!mrp_transport_send(ctx->transport, msg))
        goto error_remove_seqno;

    prset->seqno = ctx->next_seqno;
    mrp_msg_unref(msg);

    return 0;

error_remove_seqno:
    mrp_debug("error");

    mrp_htbl_remove(ctx->seqnos_to_proxy_rs, u_to_p(ctx->next_seqno), FALSE);
    mrp_msg_unref(msg);
    return -1;
}


int create_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, const char *class_name,
        const char *zone_name, uint32_t request_id)
{
    mrp_debug("%p, %p, %s, %s, %u", ctx, prset, class_name, zone_name, request_id);

    prset->class_name = mrp_strdup(class_name);
    prset->zone_name = mrp_strdup(zone_name);

    if (!prset->in_progress) {
        prset->rs->request.id = request_id;
        return create_resource_set(ctx, prset);
    }
    else {
        mrp_debug("queuing the creation of resource set %p", prset);
        proxy_resource_add_to_prset_queue(ctx, prset, RP_CREATE_RSET,
                request_id);
    }

    return 0;
}


static int destroy_resource_set(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    mrp_msg_t *msg;

    mrp_debug("%p, %p", ctx, prset);

    if (!ctx || !prset || !ctx->connected)
        return -1;

    ctx->next_seqno++;

    msg = mrp_msg_create(
            RESPROTO_SEQUENCE_NO, MRP_MSG_FIELD_UINT32, ctx->next_seqno,
            RESPROTO_REQUEST_TYPE, MRP_MSG_FIELD_UINT16,
            RESPROTO_DESTROY_RESOURCE_SET,
            RESPROTO_RESOURCE_ID, MRP_MSG_FIELD_UINT32, prset->id,
            RESPROTO_MESSAGE_END);

    if (!msg)
        return -1;

    if (!mrp_transport_send(ctx->transport, msg))
        goto error_remove_seqno;

    mrp_msg_unref(msg);

    return 0;

error_remove_seqno:
    mrp_debug("error");

    mrp_msg_unref(msg);
    return -1;
}


int destroy_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    /* this cannot be proxied, since the client is about to free the resource
       set very soon */

    if (!prset->initialized) {
        /* TODO: if not created yet, destroy as soon as it is created */
        return 0;
    }

    return destroy_resource_set(ctx, prset);
}


int proxy_resource_add_to_prset_queue(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, enum resource_proxy_action action,
        uint32_t request_id)
{
    resource_proxy_rset_operation_t *op;

    if (!ctx || !prset)
        return -1;

    op = mrp_allocz(sizeof(*op));

    if (!op)
        return -1;

    mrp_list_init(&op->hook);
    op->action = action;
    op->request_id = request_id;

    mrp_list_append(&prset->operation_queue, &op->hook);

    mrp_debug("queued operation (%p) %u, request_id %u", op, op->action,
            op->request_id);

    return 0;
}


int proxy_resource_process_queue(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset)
{
    resource_proxy_rset_operation_t *op;

    if (!ctx || !prset)
        return -1;

    if (!prset->operation_queue.next) {
        return -1;
    }

    if (mrp_list_empty(&prset->operation_queue)) {
        return 0;
    }

    op = mrp_list_entry(prset->operation_queue.next,
            resource_proxy_rset_operation_t, hook);

    if (op) {
        mrp_list_delete(&op->hook);

        switch (op->action) {
            case RP_CREATE_RSET:
                create_resource_set(ctx, prset);
                break;
            case RP_ACQUIRE_RSET:
                acquire_resource_set(ctx, prset);
                break;
            case RP_RELEASE_RSET:
                release_resource_set(ctx, prset);
                break;
            case RP_DESTROY_RSET:
                destroy_resource_set(ctx, prset);
                break;
        }

        /* the client resource id needs to be put in place */

        mrp_debug("processing operation queue: op (%p) %u, request_id %u", op,
                op->action, op->request_id);
        prset->rs->request.id = op->request_id;


        mrp_free(op);
        return 0;
    }

    mrp_debug("error");
    return -1;
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
