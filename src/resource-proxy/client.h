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

#ifndef __MURPHY_RESOURCE_PROXY_CLIENT_H__
#define __MURPHY_RESOURCE_PROXY_CLIENT_H__

#include <murphy/common.h>

/* this is the interface that we promise */
#include "murphy/resource/client-api.h"


#include "murphy/resource/resource-client.h"
#include "murphy/resource/resource-set.h"
#include "murphy/resource/resource.h"

#define MAX_RESOURCES 64
#define MAX_ATTRS 64

/* The idea is that we implement the resource backend library client API (meant
 * for the resource frontends such as dbus and others) so that it just proxies
 * the requests over the native resource protocol. This is bit difficult because
 * the client API is rather synchronous. The library needs to use the domain
 * controller facility to get the resource database tables back from the master
 * murphy (that does the real resource decisions).
 */

enum resource_proxy_status {
    RP_CONNECTED,
    RP_DISCONNECTED,
};

typedef struct {
    int num_strings;
    const char **strings;
} proxy_string_array_t;

typedef struct {
    uint32_t seqno;
} transaction_t;

typedef struct {

    mrp_mainloop_t *ml;

    int refcount;

    /* mapping of resource set ids to proxy resource sets */

    mrp_htbl_t *ids_to_proxy_rs;

    /* mapping of resource clients to proxy resource clients */

    mrp_htbl_t *clients_to_proxy_clients;

    /* mapping of transaction sequence numbers to proxy resource sets */

    mrp_htbl_t *seqnos_to_proxy_rs;

    /* mapping of resource names to attribute definition arrays */

    mrp_htbl_t *resource_names_to_attribute_defs;

    mrp_htbl_t *rs_to_proxy_rs;

    /* master resource definitions */

    uint32_t num_defs;
    mrp_resource_def_t defs[MAX_RESOURCES];

    /* master application classes */

    proxy_string_array_t *classes;

    /* message protocol */

    mrp_sockaddr_t addr;
    mrp_transport_t *transport;
    uint32_t next_seqno;
    bool connected;
    bool queried_resources;
    bool queried_classes;

    uint32_t next_rset_id;

    char *zone;

} resource_proxy_global_context_t;

typedef struct {
    char *name;
    mrp_resource_client_t *client;

} resource_proxy_client_t;


enum resource_proxy_action {
    RP_CREATE_RSET,
    RP_ACQUIRE_RSET,
    RP_RELEASE_RSET,
    RP_DESTROY_RSET,
};


typedef struct {
    enum resource_proxy_action action;
    mrp_list_hook_t hook;
    uint32_t request_id;
} resource_proxy_rset_operation_t;


typedef struct {
    mrp_resource_set_t *rs;

    uint32_t id; /* protocol id */

    /* processing of the operation queue */
    bool in_progress;
    mrp_list_hook_t operation_queue;

    char *class_name;
    char *zone_name;

    bool initialized;

    uint32_t seqno;

} resource_proxy_resource_set_t;


inline void *u_to_p(uint32_t u)
{
#ifdef __SIZEOF_POINTER__
#if __SIZEOF_POINTER__ == 8
    uint64_t o = u;
#else
    uint32_t o = u;
#endif
#else
    uint32_t o = o;
#endif
    return (void *) o;
}

inline uint32_t p_to_u(const void *p)
{
#ifdef __SIZEOF_POINTER__
#if __SIZEOF_POINTER__ == 8
    uint32_t o = 0;
    uint64_t big = (uint64_t) p;
    o = big & 0xffffffff;
#else
    uint32_t o = (uint32_t) p;
#endif
#else
    uint32_t o = p;
#endif
    return o;
}

#endif



int proxy_notify_clients(resource_proxy_global_context_t *ctx,
        enum resource_proxy_status status);

proxy_string_array_t *proxy_str_array_dup(uint32_t dim, const char **arr);

void proxy_str_array_free(proxy_string_array_t *arr);

void proxy_attribute_array_def_free(mrp_attr_def_t *arr, uint32_t dim);

mrp_attr_def_t *proxy_attribute_def_array_dup(uint32_t dim, mrp_attr_t *arr);

/* get the global resource proxy context */
resource_proxy_global_context_t *resource_proxy_get_context();

/* resource API extension */

resource_proxy_global_context_t *mrp_create_resource_proxy(mrp_mainloop_t *ml,
        const char *master_address, const char *zone);

void mrp_destroy_resource_proxy(resource_proxy_global_context_t *ctx);