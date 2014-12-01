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

#ifndef __MURPHY_RESOURCE_PROXY_SOCKET_H__
#define __MURPHY_RESOURCE_PROXY_SOCKET_H__

#include <murphy/common.h>

#include "client.h"

int connect_to_master(resource_proxy_global_context_t *ctx, const char *addr,
                mrp_mainloop_t *ml);

void disconnect_from_master(resource_proxy_global_context_t *ctx);

int resource_proxy_get_initial_values(resource_proxy_global_context_t *ctx);

int create_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, const char *class_name,
        const char *zone_name, uint32_t request_id);

int destroy_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset);

int acquire_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, uint32_t request_id);

int release_resource_set_request(resource_proxy_global_context_t *ctx,
        resource_proxy_resource_set_t *prset, uint32_t request_id);

#endif
