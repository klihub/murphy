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

#ifndef __MURPHY_DOMAIN_CONTROL_TYPES_H__
#define __MURPHY_DOMAIN_CONTROL_TYPES_H__

#include <stdbool.h>
#include <regex.h>
#include <sys/types.h>

#include <murphy/common/list.h>
#include <murphy/common/mainloop.h>
#include <murphy/common/transport.h>
#include <murphy/common/hashtbl.h>
#include <murphy/core/context.h>
#include <murphy-db/mql.h>

#include "client.h"

typedef struct pep_proxy_s pep_proxy_t;
typedef struct pep_table_s pep_table_t;
typedef struct pep_watch_s pep_watch_t;
typedef struct pdp_s       pdp_t;
typedef union  msg_u       msg_t;

/*
 * a domain controller (on the client side)
 */

struct mrp_domctl_s {
    char                    *name;       /* enforcment point name */
    mrp_mainloop_t          *ml;         /* main loop */
    mrp_sockaddr_t           addr;       /* server address */
    socklen_t                addrlen;    /* address length */
    mrp_timer_t             *ctmr;       /* connection timer */
    int                      cival;      /* connection attempt interval */
    const char              *ttype;      /* transport type */
    mrp_transport_t         *t;          /* transport towards murphy */
    int                      connected;  /* transport is up */
    mrp_domctl_table_t      *tables;     /* owned tables */
    int                      ntable;     /* number of owned tables */
    mrp_domctl_watch_t      *watches;    /* watched tables */
    int                      nwatch;     /* number of watched tables */
    mrp_domctl_connect_cb_t  connect_cb; /* connection state change callback */
    mrp_domctl_watch_cb_t    watch_cb;   /* watched table change callback */
    void                    *user_data;  /* opqaue user data for callbacks */
    int                      busy;       /* non-zero if a callback is active */
    int                      destroyed:1;/* non-zero if destroy pending */
    uint32_t                 seqno;      /* request sequence number */
    mrp_list_hook_t          pending;    /* queue of outstanding requests */
    mrp_list_hook_t          methods;    /* registered proxied methods */
};


/*
 * a table associated with or tracked by an enforcement point
 */

struct pep_table_s {
    char               *name;            /* table name */
    char               *mql_columns;     /* column definition clause */
    char               *mql_index;       /* index column list */
    mrp_list_hook_t     hook;            /* to list of all or pep tables */
    mqi_handle_t        h;               /* table handle */
    uint32_t            id;              /* id within proxy */
    mqi_column_def_t   *columns;         /* column definitions */
    mqi_column_desc_t  *coldesc;         /* column descriptors */
    int                 ncolumn;         /* number of columns */
    int                 idx_col;         /* column index of index column */
    mrp_list_hook_t     watches;         /* watches for this table */
    int                 changed : 1;     /* whether has unsynced changes */
    int                 exported : 1;    /* whether exported to parent */
    int                 created : 1;     /* whether created in parent */
    int                 imported : 1;    /* whether imported from parent */
    int                 wildcard : 1;    /* whether a wildcard watch table */
    uint32_t            expid;           /* id in parent */
    regex_t             re;              /* name-matching regexp if wildcard */
};


/*
 * a table watch
 */

struct pep_watch_s {
    pep_table_t     *table;              /* table being watched */
    char            *mql_columns;        /* column list to select */
    char            *mql_where;          /* where clause for select */
    int              max_rows;           /* max number of rows to select */
    pep_proxy_t     *proxy;              /* enforcement point */
    uint32_t         id;                 /* table id within proxy */
    int              nwatch;             /* instances if wildcard watch */
    mrp_list_hook_t  tbl_hook;           /* hook to table watch list */
    mrp_list_hook_t  pep_hook;           /* hook to proxy watch list */
    int              notify : 1;         /* whether to notify this watch */
    int              describe : 1;       /* whether needs to describe table */
};


/*
 * a policy enforcement point (on the server side)
 */

typedef struct {
    int  (*send_msg)(pep_proxy_t *proxy, msg_t *msg);
    void (*unref)(void *data);
    int  (*create_notify)(pep_proxy_t *proxy);
    int  (*update_notify)(pep_proxy_t *proxy, const char *tblname, int tblid,
                          mql_result_t *r, const char *describe);
    int  (*send_notify)(pep_proxy_t *proxy);
    void (*free_notify)(pep_proxy_t *proxy);
} proxy_ops_t;



struct pep_proxy_s {
    char              *name;             /* enforcement point name */
    pdp_t             *pdp;              /* domain controller context */
    mrp_transport_t   *t;                /* associated transport */
    mrp_list_hook_t    hook;             /* to list of all enforcement points */
    mrp_list_hook_t    tables;           /* tables owned by this */
    mrp_list_hook_t    watches;          /* tables watched by this */
    mrp_list_hook_t    wildcard;         /* wildcard watches */
    uint32_t           tblid;            /* next table ID */
    proxy_ops_t       *ops;              /* transport/messaging operations */
    uint32_t           seqno;            /* request sequence number */
    mrp_list_hook_t    pending;          /* pending method invocations */
    void              *notify_msg;       /* notification being built */
    int                notify_ntable;    /* number of changed tables */
    int                notify_ncolumn;   /* total columns in notification */
    int                notify_fail : 1;  /* notification failure */
    int                notify : 1;       /* whether has pending notifications */
};


/*
 * policy domain controller context
 */

struct pdp_s {
    mrp_context_t   *ctx;                /* murphy context */
    const char      *address;            /* external transport address */
    mrp_transport_t *extt;               /* external transport */
    mrp_transport_t *wrtt;               /* WRT transport */
    mrp_transport_t *intt;               /* internal transport */
    mrp_list_hook_t  proxies;            /* list of enforcement points */
    mrp_list_hook_t  tables;             /* list of tables we track */
    mrp_htbl_t      *watched;            /* tracked tables by name */
    mrp_list_hook_t  wildcard;           /* wildcard watches */
    mrp_deferred_t  *notify;             /* deferred notification */
    bool             notify_scheduled;   /* is notification scheduled? */
    void            *reh;                /* resolver event handler */
    int              ractive;            /* resolver active */
    bool             rblocked;           /* resolver blocked update */
    mrp_list_hook_t  imports;            /* tables imported from master */
    mrp_list_hook_t  exports;            /* tables exported to master */
    int              nexport;            /* number of exported tables */
    void            *dc;                 /* domain-controller client */
    uint32_t         expid;              /* next exported id */
};



#endif /* __MURPHY_DOMAIN_CONTROL_TYPES_H__ */
