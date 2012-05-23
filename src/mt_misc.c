
#include <config.h>
#include <sys/cdefs.h>
#include <pthread.h>
#include <reentrant.h>
#include <rpc/rpc.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#include "rpc_com.h"

/* protects the services list (svc.c) */
pthread_rwlock_t	svc_lock = PTHREAD_RWLOCK_INITIALIZER;

/* protects the RPCBIND address cache */
pthread_rwlock_t	rpcbaddr_cache_lock = PTHREAD_RWLOCK_INITIALIZER;

/* protects authdes cache (svcauth_des.c) */
pthread_mutex_t	authdes_lock = PTHREAD_MUTEX_INITIALIZER;

/* serializes authdes ops initializations */
pthread_mutex_t authdes_ops_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects des stats list */
pthread_mutex_t svcauthdesstats_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef KERBEROS
/* auth_kerb.c serialization */
pthread_mutex_t authkerb_lock = PTHREAD_MUTEX_INITIALIZER;
/* protects kerb stats list */
pthread_mutex_t svcauthkerbstats_lock = PTHREAD_MUTEX_INITIALIZER;
#endif /* KERBEROS */

/* auth_none.c serialization */
pthread_mutex_t	authnone_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects the Auths list (svc_auth.c) */
pthread_mutex_t	authsvc_lock = PTHREAD_MUTEX_INITIALIZER;

/* clnt_raw.c serialization */
pthread_mutex_t	clntraw_lock = PTHREAD_MUTEX_INITIALIZER;

/* domainname and domain_fd (getdname.c) and default_domain (rpcdname.c) */
pthread_mutex_t	dname_lock = PTHREAD_MUTEX_INITIALIZER;

/* dupreq variables (svc_dg.c) */
pthread_mutex_t	dupreq_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects first_time and hostname (key_call.c) */
pthread_mutex_t	keyserv_lock = PTHREAD_MUTEX_INITIALIZER;

/* serializes rpc_trace() (rpc_trace.c) */
pthread_mutex_t	libnsl_trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* loopnconf (rpcb_clnt.c) */
pthread_mutex_t	loopnconf_lock = PTHREAD_MUTEX_INITIALIZER;

/* serializes ops initializations */
pthread_mutex_t	ops_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects ``port'' static in bindresvport() */
pthread_mutex_t	portnum_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects proglst list (svc_simple.c) */
pthread_mutex_t	proglst_lock = PTHREAD_MUTEX_INITIALIZER;

/* serializes clnt_com_create() (rpc_soc.c) */
pthread_mutex_t	rpcsoc_lock = PTHREAD_MUTEX_INITIALIZER;

/* svc_raw.c serialization */
pthread_mutex_t	svcraw_lock = PTHREAD_MUTEX_INITIALIZER;

/* protects TSD key creation */
pthread_mutex_t	tsd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Library global tsd keys */
thread_key_t clnt_broadcast_key;
thread_key_t rpc_call_key = -1;
thread_key_t tcp_key = -1;
thread_key_t udp_key = -1;
thread_key_t nc_key = -1;
thread_key_t rce_key = -1;

/* xprtlist (svc_generic.c) */
pthread_mutex_t	xprtlist_lock = PTHREAD_MUTEX_INITIALIZER;

/* serializes calls to public key routines */
pthread_mutex_t serialize_pkey = PTHREAD_MUTEX_INITIALIZER;

#undef	rpc_createerr

struct rpc_createerr rpc_createerr;

struct rpc_createerr *
__rpc_createerr()
{
	struct rpc_createerr *rce_addr;

	mutex_lock(&tsd_lock);
	if (rce_key == -1)
		thr_keycreate(&rce_key, __rpc_free);
	mutex_unlock(&tsd_lock);

	rce_addr = (struct rpc_createerr *)thr_getspecific(rce_key);
	if (!rce_addr) {
		rce_addr = (struct rpc_createerr *)
			mem_alloc(sizeof (struct rpc_createerr));
		if (!rce_addr ||
		    thr_setspecific(rce_key, (void *) rce_addr) != 0) {
			if (rce_addr)
				__rpc_free(rce_addr);
			return (&rpc_createerr);
		}
		memset(rce_addr, 0, sizeof (struct rpc_createerr));
	}
	return (rce_addr);
}

void tsd_key_delete(void)
{
	if (clnt_broadcast_key != -1)
		pthread_key_delete(clnt_broadcast_key);
	if (rpc_call_key != -1)
		pthread_key_delete(rpc_call_key);
	if (tcp_key != -1)
		pthread_key_delete(tcp_key);
	if (udp_key != -1)
		pthread_key_delete(udp_key);
	if (nc_key != -1)
		pthread_key_delete(nc_key);
	if (rce_key != -1)
		pthread_key_delete(rce_key);
	return;
}

