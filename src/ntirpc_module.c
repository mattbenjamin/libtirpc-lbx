

struct ntirpc_ops
{
    /* auth_des.c */
    AUTH * (*authdes_pk_seccreate)(const char *, netobj *, u_int, const char *,
                                   const des_block *, nis_server *);
    AUTH * (*authdes_seccreate)(const char *, const u_int, const char *,
                                const des_block *);
    /* authdes_prot.c */
    bool_t (*xdr_authdes_cred)(XDR *xdrs, struct authdes_cred *cred);
    bool_t (*xdr_authdes_verf)(XDR *xdrs, struct authdes_verf *verf);`
    /* auth_gss.c */
    AUTH * (*authgss_create)(CLIENT *, gss_name_t, struct rpc_gss_sec *);
    AUTH * (*authgss_create_default)(CLIENT *, char *, struct rpc_gss_sec *);
    bool_t (*authgss_get_private_data)(AUTH *, struct authgss_private_data *);
    bool_t (*authgss_service)(AUTH *, int);
    bool_t (*authgss_wrap)(AUTH *, XDR *, xdrproc_t, caddr_t);
    bool_t (*authgss_unwrap)(AUTH *, XDR *, xdrproc_t, caddr_t);
    /* authgss_prot.c */
    bool_t (*xdr_rpc_gss_buf)(XDR *, gss_buffer_t, u_int);
    bool_t (*xdr_rpc_gss_cred)(XDR *, struct rpc_gss_cred *);
    bool_t (*xdr_rpc_gss_init_args)(XDR *, gss_buffer_desc *);
    bool_t (*xdr_rpc_gss_init_res)(XDR *, struct rpc_gss_init_res *);
    bool_t (*xdr_rpc_gss_wrap_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t,
                                    gss_qop_t, rpc_gss_svc_t, u_int);
    bool_t (*xdr_rpc_gss_unwrap_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t,
                                      gss_qop_t, rpc_gss_svc_t, u_int);
    bool_t (*xdr_rpc_gss_data)(XDR *, xdrproc_t, caddr_t, gss_ctx_id_t, gss_qop_t,
                               rpc_gss_svc_t, u_int);
    /*  auth_none.c */
    AUTH * (*authnone_create)(void);
    /* auth_time.c */
    int __rpc_get_time_offset(struct timeval *, nis_server *, char *, char **,
                              struct sockaddr_in *);
    /* auth_unix.c */
    AUTH * (*authunix_create)(char *, uid_t, gid_t, int, gid_t *);
    AUTH * (*authunix_create_default)(void);
    /* authunix_prot.c */
    bool_t (*xdr_authunix_parms)(XDR *, struct authunix_parms *);
    /* bindresvport.c */
    int (*bindresvport)(int, struct sockaddr_in *);
    int (*bindresvport_sa)(int, struct sockaddr *);
    /* clnt_bcast.c */
    int (*__rpc_getbroadifs)(int, int, int, broadlist_t *);
    int (*__rpc_broadenable)(int, int, struct broadif *);
    enum clnt_stat (*rpc_broadcast_exp)(rpcprog_t, rpcvers_t, rpcproc_t,
                                        xdrproc_t, caddr_t, xdrproc_t,
                                        caddr_t, resultproc_t, int,  int,
                                        const char *);
    enum clnt_stat (*rpc_broadcast)(rpcprog_t, rpcvers_t, rpcproc_t,  xdrproc_t,
                                    caddr_t, xdrproc_t, caddr_t, resultproc_t,
                                    const char *);
    /* clnt_dg.c */
    CLIENT * (*clnt_dg_create)(int, const struct netbuf *, rpcprog_t, rpcvers_t,
                               u_int sendsz, u_int);
    /* clnt_generic.c */
    CLIENT * (*clnt_create_vers)(const char *, rpcprog_t, rpcvers_t *, rpcvers_t,
                                 rpcvers_t, const char *);    
    CLIENT * (*clnt_create_vers_timed)(const char *, rpcprog_t, rpcvers_t *,
                                       rpcvers_t, rpcvers_t, const char *,
                                       const struct timeval *);
    CLIENT * (*clnt_create)(const char *, rpcprog_t, rpcvers_t, const char *);
    CLIENT * (*clnt_create_timed)(const char *, rpcprog_t, rpcvers_t,
                                  const char *, const struct timeval *);
    CLIENT * (*clnt_tp_create)(const char *, rpcprog_t, rpcvers_t,
                               const struct netconfig *);
    
    CLIENT * (*clnt_tp_create_timed)(const char *, rpcprog_t, rpcvers_t,
                                     const struct netconfig *, const struct timeval *);
    CLIENT * (clnt_tli_create)(int, const struct netconfig *, struct netbuf *,
                               rpcprog_t, rpcvers_t, u_int, u_int);
    int (*__rpc_raise_fd)(int);
    /* clnt_perror.c */ 
    char * (*clnt_sperror)(CLIENT *, const char *);
    char * (*clnt_sperrno)(enum clnt_stat);
    void (*clnt_perrno)(enum clnt_stat);
    char * (*clnt_spcreateerror)(const char *);
    void (*clnt_pcreateerror)(const char *);
    /* clnt_raw.c */
    CLIENT * (*clnt_raw_create)(rpcprog_t, rpcvers_t);
    /* clnt_simple.c */
    enum clnt_stat (*rpc_call)(const char *, rpcprog_t, rpcvers_t,
                               rpcproc_t, xdrproc_t, const char *, xdrproc_t,
                               char  *, const char *);
    /* clnt_vc.c */
    bool_t (*cond_block_events_client)(CLIENT *);
    void (*cond_unblock_events_client)(CLIENT *);
    CLIENT * (*clnt_vc_create)(int, const struct netbuf *, const rpcprog_t,
                               const rpcvers_t, u_int,  u_int);
    CLIENT * (*clnt_vc_create2)(int, const struct netbuf *, const rpcprog_t,
                                const rpcvers_t, u_int, u_int, u_int);
    /* clnt_vc_dplx.c */
    /* crypt_client.c */
    int (*_des_crypt_call)(char *, int, struct desparams *);
    /* des_crypt.c */
#if 0 /* XXX */
    int (*cbc_crypt)(char *, char *, unsigned, unsigned, char *);
    int (*ecb_crypt)(char *, char *, unsigned, unsigned);
    /* des_soft.c */
    void (*des_setparity)(char *);
#endif
    /* getnetconfig.c */
    void * (*setnetconfig)(void);
    struct netconfig * (*getnetconfig)(void *);
    int (*endnetconfig)(void *);
    struct netconfig * (*getnetconfigent)(const char *);
    void (*freenetconfigent)(struct netconfig *);
    char * (*nc_sperror)(void);
    /* getnetpath.c */
    void * (*setnetpath)(void);
    struct netconfig * (*getnetpath)(void *);
    int (*endnetpath)(void *);
    char * (*_get_next_token)(char *, int); /* really shold be static (in two files) */
    /* getpeereid.c */
    int (*getpeereid)(int, uid_t *, gid_t *);
    /* getpublickey.c */
    int (*getpublicandprivatekey)(char *, char *);
    int getpublickey(const char *, char *);
    /* getrpcent.c */
    struct rpcent * (*getrpcbynumber)(int);
    struct rpcent * (*getrpcbyname)(char *);
    void (*endrpcent)(void);
    struct rpcent * (*getrpcent)(void);
    /* getrpcport.c */    
    int (*getrpcport)(char *, int, int, int);
    /* key_call.c */
    int (*key_setsecret)(const char *);
    int (*key_secretkey_is_set)(void);
    int (*key_encryptsession_pk)(char *, netobj *, des_block *);
    int (*key_decryptsession_pk)(char *, netobj *, des_block *);
    int (*key_decryptsession)(const char *, des_block *);
    int (*key_gendes)(des_block *);
    int (*key_setnet)(struct key_netstarg *);
    int (*key_get_conv)(char *, des_block *);
    /* key_prot_xdr.c */
    bool_t (*xdr_keystatus)(XDR *, keystatus *);
    bool_t (*xdr_keybuf)(XDR *, keybuf);
    bool_t (*xdr_netnamestr)(XDR *, netnamestr *);
    bool_t (*xdr_cryptkeyarg)(XDR *, cryptkeyarg *);
    bool_t (*xdr_cryptkeyarg2)(XDR *, cryptkeyarg2 *);
    bool_t (*xdr_cryptkeyres)(XDR *xdrs, cryptkeyres *);
    bool_t (*xdr_unixcred)(XDR *, unixcred *);
    bool_t (*xdr_getcredres)(XDR *, getcredres *);
    bool_t (*xdr_key_netstarg)(XDR *, key_netstarg *);
    bool_t (*xdr_key_netstres)(XDR *, key_netstres *);
    /* mt_misc.c */
    struct rpc_createerr * (*__rpc_createerr)(void);
    void (*tsd_key_delete)(void);
    /* netname.c */
    int (*getnetname)(char name[MAXNETNAMELEN+1]);
    int (*user2netname)(char netname[MAXNETNAMELEN + 1], const uid_t, const char *);

netnamer.c
ntirpc_module.c
pmap_clnt.c
pmap_getmaps.c
pmap_getport.c
pmap_prot2.c
pmap_prot.c
pmap_rmt.c
rbtree.c
rbtree_x.c
rpcb_clnt.c
rpcb_prot.c
rpcb_st_xdr.c
rpc_callmsg.c
rpc_commondata.c
rpc_ctx.c
rpcdname.c
rpc_dplx.c
rpc_dtablesize.c
rpc_generic.c
rpc_prot.c
rpc_soc.c
rtime.c
svc_auth.c
svc_auth_des.c
svc_auth_gss.c
svc_auth_none.c
svc_auth_unix.c
svc.c
svc_dg.c
svc_dplx.c
svc_generic.c
svc_raw.c
svc_rqst.c
svc_run.c
svc_shim.c
svc_simple.c
svc_vc.c
svc_xprt.c
vc_lock.c
xdr_array.c
xdr.c
xdr_float.c
xdr_mem.c
xdr_rec.c
xdr_reference.c
xdr_sizeof.c
xdr_stdio.c
    */
};
