/*
$Id$
*/

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>

#include <krb5.h>
#include <ldap.h>
#include "ldif.h"
#include "lutil.h"
#include "lutil_ldap.h"
#include "ldap_defaults.h"
#include "ldap_log.h"

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"

#include "mod_webauthldap.h"


/** 
 *  Stolen from mod_webauth
 */
static int 
die(const char *message, server_rec *s)
{
    if (s) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "webauthldap: fatal error: %s", message);
    }
    printf("webauthldap: fatal error: %s\n", message);
    exit(1);
}

/** 
 *  Stolen from mod_webauth
 */
static void 
die_directive(server_rec *s, const char *dir, apr_pool_t *ptemp)
{
    char *msg;

    if (s->is_virtual) {
        msg = apr_psprintf(ptemp, 
                          "directive %s must be set for virtual host %s:%d", 
                          dir, s->defn_name, s->defn_line_number);
    } else {
        msg = apr_psprintf(ptemp, 
                          "directive %s must be set in main config", 
                          dir);
    }
    die(msg, s);
}

/**
 * This gets called by SASL to handle the user "auth interaction", like
 * reading the password, etc. In our case it's a no-op. The function signature
 * must comply exactly, but the arguments are discarded in this case.
 */
int sasl_interact_stub(LDAP *ld,
                       unsigned flags,
                       void *defaults,
                       void *in )
{
    return LDAP_SUCCESS;
}

/**
 * Standard conf directive parser for strings
 */
static const char *
cfg_str(cmd_parms * cmd, void *mconf, const char *arg)
{
    int e = (int)cmd->info;
    char *error_str = NULL;
    MWAL_DCONF *dconf = (MWAL_DCONF *) mconf;

    MWAL_SCONF *sconf = (MWAL_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauthldap_module);

    switch (e) {
        /* server configs */
    case E_Host:
        sconf->host = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Base:
        sconf->base = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Binddn:
        sconf->binddn = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Filter_templ:
        sconf->filter_templ = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Keytab:
        sconf->keytab = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Port:
        sconf->port = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Principal:
        sconf->principal = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Tktcache:
        sconf->tktcache = apr_pstrdup(cmd->pool, arg);
        break;

    default:
        error_str =
            apr_psprintf(cmd->pool,
                         "Invalid value cmd->info(%d) for directive %s",
                         e, cmd->directive->directive);
        break;

    }
    return error_str;
}

/**
 * Standard conf directive parser for flags
 */
static const char *
cfg_flag(cmd_parms * cmd, void *mconfig, int flag)
{
    int e = (int)cmd->info;
    char *error_str = NULL;
    MWAL_DCONF *dconf = (MWAL_DCONF *) mconfig;

    MWAL_SCONF *sconf = (MWAL_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauthldap_module);

    switch (e) {
        /* server configs */
    case E_SSL:
        sconf->ssl = flag;
        break;
    case E_Debug:
        sconf->debug = flag;
        break;
    default:
        error_str =
            apr_psprintf(cmd->pool,
                         "Invalid value cmd->info(%d) for directive %s",
                         e, cmd->directive->directive);
        break;

    }
    return error_str;
}

/**
 * Standard conf directive parser for multiple string values
 */
static const char *
cfg_multistr(cmd_parms * cmd, void *mconf, const char *arg)
{
    int e = (int)cmd->info;
    char *error_str = NULL;
    MWAL_DCONF *dconf = (MWAL_DCONF *) mconf;
    char** group;
    char** attrib;

    MWAL_SCONF *sconf = (MWAL_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webauthldap_module);

    switch (e) {
        /* server configs */
    case E_Groups:
        if (dconf->groups == NULL) {
            dconf->groups = apr_array_make(cmd->pool, 5, sizeof(char*));
        }

        group = apr_array_push(dconf->groups);
        *group = apr_pstrdup(cmd->pool, arg);
        break;
    case E_Attribs:
        if (sconf->attribs == NULL) {
            sconf->attribs = apr_array_make(cmd->pool, 5, sizeof(char*));
        }

        attrib = apr_array_push(sconf->attribs);
        *attrib = apr_pstrdup(cmd->pool, arg);
        break;
    default:
        error_str =
            apr_psprintf(cmd->pool,
                         "Invalid value cmd->info(%d) for directive %s",
                         e, cmd->directive->directive);
        break;

    }
    return error_str;
}


#define SSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, RSRC_CONF, TAKE1, help}

#define SFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, RSRC_CONF, FLAG, help}

#define ADSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, ACCESS_CONF, TAKE1, help}

#define ADFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, ACCESS_CONF, FLAG, help}

#define DITSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_multistr,(void*)mconfig, OR_AUTHCFG, TAKE1, help}

#define SITSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_multistr,(void*)mconfig, RSRC_CONF, TAKE1, help}

static const command_rec cmds[] = {
    /* server/vhost */
    SSTR(CD_Host, E_Host, CM_Host),
    SSTR(CD_Base, E_Base, CM_Base),
    SSTR(CD_Binddn, E_Binddn, CM_Binddn),
    SSTR(CD_Filter_templ, E_Filter_templ, CM_Filter_templ),
    SSTR(CD_Keytab, E_Keytab, CM_Keytab),
    SSTR(CD_Tktcache, E_Tktcache, CM_Tktcache),
    SSTR(CD_Port, E_Port, CM_Port),
    SSTR(CD_Principal, E_Principal, CM_Principal),
    SSTR(CD_Privgroupattr, E_Privgroupattr, CM_Privgroupattr),

    SFLAG(CD_SSL, E_SSL, CM_SSL),
    SFLAG(CD_Debug, E_Debug, CM_Debug),

    SITSTR(CD_Attribs, E_Attribs, CM_Attribs),
    DITSTR(CD_Groups, E_Groups, CM_Groups),
    {NULL}
};

#undef SSTR
#undef SFLAG
#undef ADSTR
#undef ADFLAG
#undef DITSTR
#undef SITSTR


/**
 * Handler for creating a server conf structure
 */
static void *
config_server_create(apr_pool_t * p, server_rec * s) 
{
    MWAL_SCONF *sconf;

    sconf = (MWAL_SCONF *) apr_pcalloc(p, sizeof(MWAL_SCONF));

    /* init defaults */

    sconf->base = DF_Base;
    sconf->debug = DF_Debug;
    sconf->filter_templ = DF_Filter_templ;
    sconf->host = DF_Host;
    sconf->port = DF_Port;
    sconf->privgroupattr = DF_Privgroupattr;
    sconf->ssl = DF_SSL;

    return (void *)sconf;
}

/**
 * Handler for creating a per-directory conf structure
 */
static void *
config_dir_create(apr_pool_t * p, char *path) 
{
    MWAL_DCONF *dconf;
    dconf = (MWAL_DCONF *) apr_pcalloc(p, sizeof(MWAL_DCONF));
    /* init defaults */

    return (void *)dconf;
}


#define MERGE(field) \
    conf->field = oconf->field ? oconf->field : bconf->field

/**
 * Handler for merging server conf structures
 */
static void *
config_server_merge(apr_pool_t *p, void *basev, void *overv)
{
    MWAL_SCONF *conf, *bconf, *oconf;

    conf = (MWAL_SCONF*) apr_pcalloc(p, sizeof(MWAL_SCONF));
    bconf = (MWAL_SCONF*) basev;
    oconf = (MWAL_SCONF*) overv;

    MERGE(base);
    MERGE(binddn);
    MERGE(debug);
    MERGE(filter_templ);
    MERGE(host);
    MERGE(keytab);
    MERGE(port);
    MERGE(principal);
    MERGE(privgroupattr);
    MERGE(tktcache);
    MERGE(ssl);

    if (bconf->attribs == NULL) {
        conf->attribs = oconf->attribs;
    } else if (oconf->attribs == NULL) {
        conf->attribs = bconf->attribs;
    } else {
        // dups here are OK
        conf->attribs = apr_array_append(p, bconf->attribs, oconf->attribs);
    }

}

/**
 * Handler for merging per-directory conf structures
 */
static void *
config_dir_merge(apr_pool_t *p, void *basev, void *overv) 
{
    MWAL_DCONF *conf, *bconf, *oconf;

    conf = (MWAL_DCONF*) apr_pcalloc(p, sizeof(MWAL_DCONF));
    bconf = (MWAL_DCONF*) basev;
    oconf = (MWAL_DCONF*) overv;

    if (bconf->groups == NULL) {
        conf->groups = oconf->groups;
    } else if (oconf->groups == NULL) {
        conf->groups = bconf->groups;
    } else {
        // dups here are OK
        conf->groups = apr_array_append(p, bconf->groups, oconf->groups);
    }

    return (void *)conf;
}

#undef MERGE


/**
 * Called during server startup to initialize this module.
 */
static int
post_config_hook(apr_pool_t *pconf, apr_pool_t *plog,
                 apr_pool_t *ptemp, server_rec *s) 
{
    MWAL_SCONF *sconf = (MWAL_SCONF*)ap_get_module_config(s->module_config,
                                                          &webauthldap_module);
    if (sconf->debug) ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, s, 
                                   "webauthldap: initializing");

    // These all must be non-null:
#define NULCHECK(val, label) \
    if (val == NULL) die_directive(s, label, ptemp);

    NULCHECK(sconf->keytab, CD_Keytab);
    NULCHECK(sconf->principal, CD_Principal);
    NULCHECK(sconf->tktcache, CD_Tktcache);
#undef NULCHECK

    // Global settings
    sconf->ldapversion = LDAP_VERSION3;
    sconf->scope = LDAP_SCOPE_SUBTREE;

    return OK;
}


/**
 * This inserts the userid in every marked spot in the filter string. So
 * e.g. if the marker is the string "USER", a filter like 
 * ((uid=USER)|(sunetid=USER))
 * will be converted to ((uid=antonu)|(sunetid=antonu))
 * @param lc main context struct for this module, for passing things around
 * @return new filter string with userid substituted
 */
char* 
webauthldap_make_filter(MWAL_LDAP_CTXT* lc) 
{
    apr_pool_t * p = lc->r->pool;
    char* userid = lc->r->user;
    char* filter_template = apr_pstrdup(lc->r->pool, lc->sconf->filter_templ);
    char* beg = filter_template;
    char* end = filter_template;
    char* filter = NULL;

    int match_length = strlen(FILTER_MATCH);

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: filter template is %s", filter_template);
    
    do {
        // Everytime we find a marker
        if (strncmp(end, FILTER_MATCH, match_length) == 0) {

            // Can't apr_pstrcat nulls strings - that's how it tells where the
            // last passed parameter is.
            if (filter == NULL)
                filter = apr_pstrcat(p, apr_pstrndup(p, beg, end - beg), 
                                     userid, NULL);
            else
                filter = apr_pstrcat(p,filter, apr_pstrndup(p, beg, end - beg),
                                     userid, NULL);

            beg = end + match_length;
        }
    } while(*(++end) != '\0');
    
    // Append the last chunk. If no substitutions were done, this is the 
    // entire template string.
    if (end > beg)
        filter = apr_pstrcat(p,filter, apr_pstrndup(p, beg, end - beg), NULL);
    
    return filter;
}

/**
 * This obtains the K5 ticket from the given keytab and places it into the 
 * given credentials cache file
 * @param lc main context struct for this module, for passing things around
 * @return zero if OK, kerberos error code if not
 */
int 
webauthldap_get_ticket(MWAL_LDAP_CTXT* lc)
{
    krb5_context ctx;
    krb5_creds creds;
    krb5_get_init_creds_opt opts;
    krb5_keytab keytab;
    krb5_ccache cc;
    krb5_principal princ;
    krb5_error_code code;

    if ((code = krb5_init_context(&ctx)) != 0)
        return code;

    // FIXME maybe get it out of the keytab?
    if ((code = krb5_parse_name(ctx, lc->sconf->principal, &princ)) != 0)
        return code;

    if ((code = krb5_kt_resolve(ctx, lc->sconf->keytab, &keytab)) != 0)
        return code;

    if ((code = krb5_cc_resolve(ctx, lc->sconf->tktcache, &cc)) != 0) {
        krb5_kt_close(ctx, keytab);
        return code;
    }

    if ((code = krb5_cc_initialize(ctx, cc, princ)) != 0) {
        krb5_kt_close(ctx, keytab);
        return code;
    }

    krb5_get_init_creds_opt_init(&opts);

    code = krb5_get_init_creds_keytab(ctx,
                                      &creds,
                                      princ,
                                      keytab,
                                      0, /* start_time */
                                      NULL, /* in_tkt_service */
                                      &opts);

    krb5_kt_close(ctx, keytab);

    if (code != 0)
        return code;

    /* add the creds to the cache */
    code = krb5_cc_store_cred(ctx, cc, &creds);
    krb5_free_cred_contents(ctx, &creds);
    if (code != 0) {
        return code;
    } else {
        return 0;
    }

}


/**
 * This will remove duplicates in from a given apr_array, and return the 
 * resulting new array, allocated out of the same pool as the original array.
 * @orig the array to remove duplicates from
 * @lowercase true/false flag for comparison and result's case-sensitivity
 * @return the array with no duplicate entries
 */
apr_array_header_t*
webauthldap_undup(apr_array_header_t* orig, int lowercase) 
{
    char* p;
    char** pusher, **popper;
    int i;
    apr_pool_t* pool;
    apr_table_t* eliminator;
    apr_array_header_t* newarray;
    apr_array_header_t* barr;
    apr_table_entry_t* belt;

    if (!orig)
        return NULL;

    if (orig->nelts == 0)
        return NULL;

    pool = orig->pool;
    eliminator = apr_table_make(pool, orig->nelts);

    newarray = apr_array_copy(pool, orig);
    for(i=0; orig->nelts; i++) {
        popper = apr_array_pop(newarray);
        if (lowercase) {
            for (p = *popper; *p != '\0'; p++)
                *p = tolower(*p);
        }
        apr_table_set(eliminator, *popper, *popper);
    }

    barr = (apr_array_header_t*)apr_table_elts(eliminator);
    belt = (apr_table_entry_t *)barr->elts;

    newarray = apr_array_make(pool,  barr->nelts, sizeof(char*));
    for (i = 0; i < barr->nelts; ++i) {
        pusher = apr_array_push(newarray);
        *pusher = belt[i].key;
    }

    return newarray;
}


/**
 * This will initialize the main context struct and set up the table of
 * attributes to later put into environment variables.
 * @param lc main context struct for this module, for passing things around
 * @return zero if OK, HTTP_INTERNAL_SERVER_ERROR if not
 */
int 
webauthldap_init(MWAL_LDAP_CTXT* lc) 
{
    int i;
    char** attrib;
    char *p, *temp;
    apr_array_header_t* attribs;
    char* default_attribs[] = DF_Attribs;
    
    lc->dconf = (MWAL_DCONF*)
        ap_get_module_config(lc->r->per_dir_config, &webauthldap_module);

    lc->sconf = (MWAL_SCONF*) 
        ap_get_module_config(lc->r->server->module_config,&webauthldap_module);

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, "%s %s %s",
                     "webauthldap: invoked for user", lc->r->user,
                     "\n***************************************************");

    // These come with defaults:
    lc->filter = webauthldap_make_filter(lc);
    lc->port = atoi(lc->sconf->port);
    lc->sizelimit  = LDAP_SIZELIMIT;
    lc->privgroups = apr_table_make(lc->r->pool, 10);

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: filter is %s", lc->filter);

    // Allocate the table of attributes to later put into env vars
    lc->envvars = apr_table_make(lc->r->pool, sizeof(default_attribs));

    // Set some default env vars, such as name, mail, etc
    for (i=0; default_attribs[i] != NULL; i++) {
        temp = apr_pstrdup(lc->r->pool, default_attribs[i]);
        for (p = temp; *p != '\0'; p++)
            *p = tolower(*p);
        apr_table_set(lc->envvars, temp, temp);
        
        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: default attribute to put into env: %s",
                         temp);
    }

    // Whatever else env vars the conf file added. This will override the 
    // defaults since apr_table_set is used here, and all names are lowercased.
    if (lc->sconf->attribs) {
        attribs = apr_array_copy(lc->r->pool, lc->sconf->attribs);

        for(i=0; ((attrib = apr_array_pop(attribs)) != NULL); i++) {
            for (p = *attrib; *p != '\0'; p++)
                *p = tolower(*p);
            apr_table_set(lc->envvars, *attrib, *attrib);

        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: conf attribute to put into env: %s",
                         *attrib);
        }
    }
    
    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: initialized sucessfully");

    return 0;
}


/**
 * This will set some LDAP options, initialize the ldap connection and
 * bind to the ldap server. If at first the bind fails with a "local error"
 * it will try to renew the kerberos ticket and try binding again.
 * @param lc main context struct for this module, for passing things around
 * @return zero if OK, HTTP_INTERNAL_SERVER_ERROR if not
 */
int
webauthldap_bind(MWAL_LDAP_CTXT* lc) 
{
    int rc;
    char* tktenv;
    MWAL_SASL_DEFAULTS *defaults;

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: begins ldap bind");

    // Initialize the connection
    lc->ld = ldap_init(lc->sconf->host, lc->port);

    if (lc->ld == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "webauthldap: ldap_init failure ld is NULL");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    // Set to no referrals
    if (ldap_set_option(lc->ld, LDAP_OPT_REFERRALS, LDAP_OPT_OFF)
        != LDAP_OPT_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "webauthldap: Could not set LDAP_OPT_REFERRALS");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    // Only works with version 3
    if (ldap_set_option(lc->ld, LDAP_OPT_PROTOCOL_VERSION, 
                        &lc->sconf->ldapversion)
        != LDAP_OPT_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "webauthldap: Could not set LDAP_OPT_PROTOCOL_VERSION %d",
                     lc->sconf->ldapversion);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (lc->sconf->ssl) {
        rc = ldap_start_tls_s(lc->ld, NULL, NULL);
        
        if (rc != LDAP_SUCCESS) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                         "webauthldap: Could not start tls: %s",
                         ldap_err2string(rc), rc);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    // Set up SASL defaults.
    defaults = (MWAL_SASL_DEFAULTS*) apr_pcalloc(lc->r->pool, 
                                                 sizeof(MWAL_SASL_DEFAULTS));
    ldap_get_option(lc->ld, LDAP_OPT_X_SASL_MECH, &defaults->mech);
    ldap_get_option(lc->ld, LDAP_OPT_X_SASL_REALM, &defaults->realm);
    ldap_get_option(lc->ld, LDAP_OPT_X_SASL_AUTHCID, &defaults->authcid);
    ldap_get_option(lc->ld, LDAP_OPT_X_SASL_AUTHZID, &defaults->authzid);

    // since SASL will look there
    tktenv = apr_psprintf(lc->r->pool, "%s=%s", ENV_KRB5_TICKET, 
                          lc->sconf->tktcache);
    if (putenv(tktenv) != 0) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server,
                     "webauthldap: cannot set ticket cache env var");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: set ticket to %s", tktenv);


    rc = ldap_sasl_interactive_bind_s(lc->ld, lc->sconf->binddn,
                                      defaults->mech, NULL, NULL,
                                      LDAP_SASL_QUIET, sasl_interact_stub,
                                      defaults);

    // this means the ticket expired
    if (rc == LDAP_LOCAL_ERROR) {

        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: getting new ticket");

        // so let's get a new one
        if ((rc = webauthldap_get_ticket(lc)) != 0) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server,
                         "mod_webauthldap: cannot get ticket: %s", 
                         error_message(rc));
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        // and try to bind one more time
        rc = ldap_sasl_interactive_bind_s(lc->ld, lc->sconf->binddn,
                                          defaults->mech, NULL, NULL,
                                          LDAP_SASL_QUIET, sasl_interact_stub,
                                          defaults);
    } else {
        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: using existing ticket");
    }

    if (rc != LDAP_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "ldap_sasl_interactive_bind_s: %s (%d)",
                     ldap_err2string(rc), rc);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: bound sucessfully to %s", lc->sconf->host);

    return 0;
}



/**
 * This will parse a given ldap entry, placing all attributes and values into
 * the given apr table. It will also copy out the privgroup attributes into a
 * separate table. Duplicates are preserved in both cases.
 * @param lc main context struct for this module, for passing things around
 * @param entry the given LDAP entry to parse
 * @param attr_table is the table to place the attributes into
 * @return nothing
 */
static void
webauthldap_parse_entry(MWAL_LDAP_CTXT* lc, LDAPMessage * entry, 
                        apr_table_t * attr_table)
{
    char *a, *dn;
    int i, rc;
    BerElement *ber = NULL;
    struct berval **bvals;
    FILE *tmpfp;
    char *val;

    for (a = ldap_first_attribute(lc->ld, entry, &ber); a != NULL;
         a = ldap_next_attribute(lc->ld, entry, ber)) {
        if ((bvals = ldap_get_values_len(lc->ld, entry, a)) != NULL) {
            for (i = 0; bvals[i] != NULL; i++) {
                val = apr_pstrndup(lc->r->pool, (char *)bvals[i]->bv_val,
                                   (apr_size_t) bvals[i]->bv_len);
                apr_table_add(attr_table, a, val);

                if (!strcasecmp(a, lc->sconf->privgroupattr))
                    apr_table_addn(lc->privgroups, val, val);

            }
            ber_bvecfree(bvals);
        }
    }

    if (ber != NULL) {
        ber_free(ber, 0);
    }
}


/**
 * This will conduct the ldap search and parse the returned messages. It 
 * all messages except entries, on which it calls ebauthldap_parse_entry
 * @param lc main context struct for this module, for passing things around
 * @return zero if OK, HTTP_INTERNAL_SERVER_ERROR if not
 */
static int
webauthldap_dosearch(MWAL_LDAP_CTXT* lc)
{
    LDAPMessage *res = NULL;
    LDAPMessage *msg = NULL;
    ber_int_t msgid;
    int rc, numMessages;
    int attrsonly = 0;

    rc = ldap_search_ext(lc->ld, lc->sconf->base, lc->sconf->scope, lc->filter,
                         lc->attrs, attrsonly, NULL, NULL, NULL, lc->sizelimit,
                         &msgid);

    if (rc != LDAP_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "webauthldap: ldap_search_ext: %s (%d)",
                     ldap_err2string(rc), rc);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if ((rc = 
         ldap_result(lc->ld, LDAP_RES_ANY, LDAP_MSG_ALL, NULL, &res)) > 0) {
        
        numMessages = ldap_count_messages(lc->ld, res);
        
        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: search returned %d messages", 
                         numMessages);
        
        if (numMessages > 0) {
            lc->entries = (apr_table_t **) 
                apr_pcalloc(lc->r->pool, 
                            (numMessages+1)*sizeof(apr_table_t *));
            lc->numEntries = 0;
            for (msg = ldap_first_message(lc->ld, res);
                 msg != NULL; msg = ldap_next_message(lc->ld, msg)) {
                
                if (ldap_msgtype(msg) == LDAP_RES_SEARCH_ENTRY) {
                    lc->entries[lc->numEntries] = apr_table_make(lc->r->pool,
                                                                 50);
                    webauthldap_parse_entry(lc, msg, 
                                            lc->entries[lc->numEntries]);
                    lc->numEntries++;
                }
            }
            if (lc->sconf->debug)
                ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                             "webauthldap: search returned %d entries", 
                             lc->numEntries);
        }
        ldap_msgfree(res);
    }

    if ((rc == -1) || (rc == 0)) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, lc->r->server, 
                     "webauthldap: ldap_result: %s (%d)",
                     ldap_err2string(rc), rc);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    return 0;
}


/**
 * This will set be called with every attribute value pair that was received
 * from the LDAP search. Only attributes that were requested through the conf 
 * directives as well as a few default attributes will be placed in 
 * environment variables starting with "WEBAUTH_".
 *
 * The single valued attributes go into appropriately named env vars, while
 * multivalued attributes have a env var for each value, with the name of the 
 * var containing a sequence number at the end. No particular order is 
 * guaranteed. In the multivalued case, the env var with the canonical 
 * (unnumbered) name will contain the value we encounter, essentially random.
 * @param lcp main context struct for this module, for passing things around
 * @param key the attribute name, as supplied by LDAP api
 * @param val the value of the attribute
 * @return always 1, which means keep going through the table
 */
int
webauthldap_setenv(void* lcp, const char *key, const char *val)
{
    int i;
    char* newkey, *numbered_key, *p, *existing_val;
    MWAL_LDAP_CTXT* lc = (MWAL_LDAP_CTXT*) lcp;

    if (key == NULL | val == NULL)
        return 1;

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                     "webauthldap: got attrib: %s val: %s", key, val);

    // conf directive could have been in different capitalization, 
    // simpler to just lowercase for the comparison
    newkey = apr_psprintf(lc->r->pool, key);
    for (p = newkey; *p != '\0'; p++)
        *p = tolower(*p);

    // set into the environment only those attributes, which were specified
    if (!apr_table_get(lc->envvars, newkey))
        return 1;
    
    newkey = apr_psprintf(lc->r->pool, "WEBAUTH_%s", key);

    // environment var names should be uppercased
    for (p = newkey; *p != '\0'; p++)
        *p = toupper(*p);

    existing_val = (char*) apr_table_get(lc->r->subprocess_env, newkey);

    // normal case of single-valued attribute
    if (existing_val == NULL) {
        if (lc->sconf->debug)
            ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                         "webauthldap: setting %s as single valued", newkey);
        apr_table_set(lc->r->subprocess_env, newkey, val);
    } else {
        // set WEBAUTH_BLAH1 to be the same as WEBAUTH_BLAH
        numbered_key = apr_psprintf(lc->r->pool, "%s%d", newkey, 1);
        if (apr_table_get(lc->r->subprocess_env, numbered_key) == NULL) {
            if (lc->sconf->debug)
                ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                             "webauthldap: setting %s", numbered_key);
            apr_table_set(lc->r->subprocess_env, numbered_key, existing_val);
        }

        // now set WEBAUTH_BLAH2 WEBAUTH_BLAH3 and so on
        for (i=2; i<MAX_ENV_VALUES; i++) {
            numbered_key = apr_psprintf(lc->r->pool, "%s%d", newkey, i);
            if (apr_table_get(lc->r->subprocess_env, numbered_key) == NULL) {
                if (lc->sconf->debug)
                    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, lc->r->server, 
                                 "webauthldap: setting %s", numbered_key);

                apr_table_setn(lc->r->subprocess_env, numbered_key, val);
                break;
            }
        }
    }

    return 1; // means keep going thru all available entries
}

/**
 * This is the API hook for this module, it gets called first in the 
 * auth_check stage, and is only invoked if some require directive was 
 * present at the requested location. This initializes the module, binds to 
 * the ldap server and conducts the search for the user's record. Then it 
 * checks the access validity against the user's priviledge group attributes
 * and sets specified attributes into environment variables.
 * @param r is the apache request record pointer
 * @return the HTTP code in case of an error, HTTP_UNAUTHORIZED is access is
 * not allowed, or OK if access is confirmed.
 */
static int
auth_checker_hook(request_rec * r)
{
    const char *at = ap_auth_type(r);
    MWAL_LDAP_CTXT* lc;
    int rc, i;

    apr_array_header_t *reqs_arr;
    apr_array_header_t *reqs_temp;
    require_line *reqs;
    const char *t, *w;
    int m = r->method_number;
    require_line* popper;
    require_line* pusher;

    apr_array_header_t* groups;
    char** group;

    if ((at == NULL) || (strcmp(at, "WebAuth") != 0)) {
        return DECLINED;
    }

    if (r->user == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, 
                     "webauthldap: user is not set");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    lc = (MWAL_LDAP_CTXT*) apr_pcalloc(r->pool, sizeof(MWAL_LDAP_CTXT));
    lc->r = r;

    if ((rc = webauthldap_init(lc)) != 0)
        return rc;

    if ((rc = webauthldap_bind(lc)) != 0)
        return rc;

    if ((rc = webauthldap_dosearch(lc)) != 0)
        return rc;

    //
    // Validate privgroups. this code is inspired by similar code in mod_auth.c
    // except in this case, we pull out our own group require's and leave the 
    // rest to mod_auth.c to deal with
    reqs_arr = (apr_array_header_t*)ap_requires(r);
    if (reqs_arr) {
        reqs = (require_line *)reqs_arr->elts;
        reqs_temp = apr_array_make(r->pool, 5, sizeof(require_line*));

        while ((popper = apr_array_pop(reqs_arr)) != NULL) {
            if (!(popper->method_mask & (AP_METHOD_BIT << m))) {
                pusher = apr_array_push(reqs_temp);
                *pusher = *popper;
                continue;
            }

            t = popper->requirement;
            w = ap_getword_white(r->pool, &t);
            if (!strcmp(w, REQUIRE_DIRECTIVE)) {
                while (t[0]) {
                    w = ap_getword_conf(r->pool, &t);
                    if (apr_table_get(lc->privgroups, w) == NULL) {
                        if (lc->sconf->debug)
                            ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, 
                                         "%s %s %s", 
                                         "webauthldap: UNAUTHORIZED:",
                                         "does not have group", w);
                        return HTTP_UNAUTHORIZED;
                    }
                }
            } else {
                pusher = apr_array_push(reqs_temp);
                *pusher = *popper;
            }
        }

         while ((popper = apr_array_pop(reqs_temp)) != NULL) {
             pusher = apr_array_push(reqs_arr);
             *pusher = *popper;
         }
    }


    /*
    // FIXME remains to be decided which version of privgroups code to use
    // Alternate Validate privgroups code in case the mod_auth trick wont fly
    //
    if (lc->dconf->groups != NULL) {
        groups = apr_array_copy(lc->r->pool, lc->dconf->groups);

        while((group = apr_array_pop(groups)) != NULL) {
            if (apr_table_get(lc->privgroups, *group) == NULL) {
                if (lc->sconf->debug)
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, 
                                 "%s %s %s", 
                                 "webauthldap: UNAUTHORIZED:",
                                 "does not have group", *group);
                return HTTP_UNAUTHORIZED;
            }
        }
    }
    */

    //
    // Set the env vars
    //
    apr_table_set(r->subprocess_env, ENV_WEBAUTH_USER, r->user);
    for (i=0; i<lc->numEntries; i++) {
        apr_table_do(webauthldap_setenv, lc, lc->entries[i], NULL);
    }

    if (lc->sconf->debug)
        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, r->server, "%s %s %s",
                     "webauthldap: finished for user", lc->r->user,
                     "\n***************************************************");

    return DECLINED; // this is so modules like mod_auth get to run
}

/**
 * Standard hook registration function 
 */
static void
webauthldap_register_hooks(apr_pool_t * p)
{
    /* get this module called after webauth */
    static const char * const mods[]={ "mod_access.c", "mod_auth.c", NULL };

    ap_hook_post_config(post_config_hook, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_auth_checker(auth_checker_hook, NULL, mods, APR_HOOK_FIRST);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA webauthldap_module = {
    STANDARD20_MODULE_STUFF,
    config_dir_create,               /* create per-dir config structures */
    config_dir_merge,                /* merge  per-dir    config structures */
    config_server_create,            /* create per-server config structures */
    config_server_merge,             /* merge  per-server config structures */
    cmds,                            /* table of config file commands */
    webauthldap_register_hooks       /* register hooks */
};


/* 
** Local variables: 
** mode: c 
** c-basic-offset: 4 
** indent-tabs-mode: nil 
** end: 
*/