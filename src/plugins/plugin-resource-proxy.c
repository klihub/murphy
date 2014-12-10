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

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <murphy/common.h>

#include <murphy/core/plugin.h>

#include "resource-proxy/client.h"

enum {
    ARG_ADDRESS,
    ARG_ZONE,
};

typedef struct {
    bool connected;

    char *address;
    char *zone;

    resource_proxy_global_context_t *resource_ctx;
    mrp_plugin_t *plugin;
} resource_proxy_t;

static int resource_proxy_init(mrp_plugin_t *plugin)
{
    resource_proxy_t *ctx = NULL;

    mrp_debug("> resource_proxy_init");

    ctx = mrp_allocz(sizeof(*ctx));
    if (!ctx)
        goto error;

    ctx->plugin = plugin;

    ctx->address = mrp_strdup(plugin->args[ARG_ADDRESS].str);
    if (!ctx->address)
        goto error;

    ctx->zone = mrp_strdup(plugin->args[ARG_ZONE].str);
    if (!ctx->zone)
        goto error;

    /* connect the resource client of the master Murphy */

    ctx->resource_ctx = mrp_create_resource_proxy(plugin->ctx->ml,
            ctx->address, "driver");
    if (!ctx->resource_ctx)
        goto error;

    plugin->data = ctx;

    return TRUE;

error:
    if (ctx){
        mrp_destroy_resource_proxy(ctx->resource_ctx);
        mrp_free(ctx->address);
        mrp_free(ctx->zone);
        mrp_free(ctx);
    }
    return FALSE;
}


static void resource_proxy_exit(mrp_plugin_t *plugin)
{
    resource_proxy_t *ctx = (resource_proxy_t *) plugin->data;

    mrp_debug("> resource_proxy_exit");

    if (ctx) {
        mrp_destroy_resource_proxy(ctx->resource_ctx);

        mrp_free(ctx->address);
        mrp_free(ctx->zone);
        mrp_free(ctx);
    }
}

#define RESOURCE_PROXY_DESCRIPTION "Plugin to implement proxying resources"
#define RESOURCE_PROXY_HELP        ""
#define RESOURCE_PROXY_VERSION     MRP_VERSION_INT(0, 0, 1)
#define RESOURCE_PROXY_AUTHORS     "Ismo Puustinen <ismo.puustinen@intel.com>"

static mrp_plugin_arg_t args[] = {
    MRP_PLUGIN_ARGIDX(ARG_ADDRESS, STRING, "address", ""),
    MRP_PLUGIN_ARGIDX(ARG_ZONE, STRING, "zone", "driver"),
};

MURPHY_REGISTER_PLUGIN("resource-proxy", RESOURCE_PROXY_VERSION,
        RESOURCE_PROXY_DESCRIPTION,RESOURCE_PROXY_AUTHORS, RESOURCE_PROXY_HELP,
        MRP_SINGLETON, resource_proxy_init, resource_proxy_exit,
        args, MRP_ARRAY_SIZE(args), NULL, 0, NULL, 0, NULL);
