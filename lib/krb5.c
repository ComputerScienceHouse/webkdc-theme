/*
 * Kerberos interface for WebAuth.
 *
 * All WebAuth functions that use Kerberos use the routines in this file.
 * This is the only code in WebAuth with direct Kerberos dependencies, so
 * ports to different versions of Kerberos should only require changing this
 * one file and its associated components.
 *
 * There are currently only two functions whose implementation varies between
 * MIT and Heimdal, namely encode_cred and decode_cred.  Since those functions
 * need (in most cases) intimate knowledge of the layout of data structures,
 * it's easiest to just maintain two implementations.  Accordingly, we
 * *include* either krb5-mit.c or krb5-heimdal.c into this file based on
 * configure results.  We do this with the preprocessor to preserve static
 * linkage of functions.
 *
 * If you don't see some function here, look in krb5-mit.c and krb5-heimdal.c.
 * If you have to modify either of those files, you'll probably need to modify
 * both.
 *
 * Written by Roland Schemers
 * Copyright 2002, 2003, 2006, 2007, 2009, 2010, 2011, 2012
 *     The Board of Trustees of the Leland Stanford Junior University
 *
 * See LICENSE for licensing terms.
 */

#include <config.h>
#include <portable/apr.h>
#include <portable/krb5.h>
#include <portable/system.h>

#include <lib/internal.h>
#include <util/macros.h>
#include <webauth/basic.h>
#include <webauth/krb5.h>

/*
 * A WebAuth Kerberos context.  This represents a local identity and set of
 * tickets, along with APR and Kerberos context.
 */
struct webauth_krb5 {
    apr_pool_t *pool;
    krb5_context ctx;
    krb5_ccache cc;
    krb5_principal princ;
};

/*
 * Forward declarations for the functions that have to be used by the MIT- and
 * Heimdal-specific code.
 */
static int decode_principal(struct webauth_context *, struct webauth_krb5 *,
                            const char *, krb5_principal *)
    __attribute__((__nonnull__));
static int encode_principal(struct webauth_context *, struct webauth_krb5 *,
                            krb5_principal, char **)
    __attribute__((__nonnull__));
static int error_set(struct webauth_context *, struct webauth_krb5 *,
                     krb5_error_code, const char *, ...)
    __attribute__((__nonnull__(1, 4), __format__(printf, 4, 5)));

/* Include the appropriate implementation-specific Kerberos bits. */
#if HAVE_KRB5_MIT
# include "krb5-mit.c"
#elif HAVE_KRB5_HEIMDAL
# include "krb5-heimdal.c"
#else
# error "Unknown Kerberos implementation"
#endif


/*
 * Replacement for krb5_unparse_name_flags for implementations that don't have
 * it (MIT and older Heimdal).  Only supports the
 * KRB5_PRINCIPAL_UNPARSE_NO_REALM flag and always assumes that flag is set.
 */
#ifndef HAVE_KRB5_UNPARSE_NAME_FLAGS
#define KRB5_PRINCIPAL_UNPARSE_NO_REALM 1
static krb5_error_code
krb5_unparse_name_flags(krb5_context ctx, krb5_principal princ,
                        int flags UNUSED, char **name)
{
    krb5_error_code code;
    char *realm;

    code = krb5_unparse_name(ctx, princ, name);
    if (code != 0)
        return code;
    /* FIXME: Doesn't handle escaped @ characters. */
    realm = strchr(*name, '@');
    if (realm != NULL)
        *realm = '\0';
    return 0;
}
#endif


/*
 * Set the WebAuth error message in the context following a Kerberos error.
 * Supports printf-style formatting and appends the Kerberos error to the
 * provided user error.  Returns WA_ERR_KRB5 for the convenience of the
 * caller, who can then just call this function and return its return status.
 */
static int
error_set(struct webauth_context *ctx, struct webauth_krb5 *kc,
          krb5_error_code err, const char *format, ...)
{
    va_list args;
    char *string;
    const char *k5_msg;

    va_start(args, format);
    string = apr_pvsprintf(ctx->pool, format, args);
    va_end(args);
    if (kc == NULL || kc->ctx == NULL)
        wai_error_set(ctx, WA_ERR_KRB5, "no Kerberos context");
    else {
        k5_msg = krb5_get_error_message(kc->ctx, err);
        wai_error_set(ctx, WA_ERR_KRB5, "%s: %s", string, k5_msg);
        krb5_free_error_message(kc->ctx, k5_msg);
    }
    return WA_ERR_KRB5;
}


/*
 * Convert a principal into a string, taking the contexts, the principal, and
 * the location into which to store the resulting principal.  Returns a
 * WebAuth status.
 */
static int
encode_principal(struct webauth_context *ctx, struct webauth_krb5 *kc,
                 krb5_principal princ, char **principal)
{
    krb5_error_code code;
    char *name;

    code = krb5_unparse_name(kc->ctx, princ, &name);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot unparse principal");
    *principal = apr_pstrdup(kc->pool, name);
    krb5_free_unparsed_name(kc->ctx, name);
    return WA_ERR_NONE;
}


/*
 * Convert a principal from a string to the Kerberos representation, taking
 * the contexts, the string, and the destination principal structure.  Returns
 * a WebAuth status.
 *
 * Note that this uses the Kerberos library to allocate the principal data
 * structures, not an APR pool, so the resulting principal will need to be
 * manually freed.
 */
static int
decode_principal(struct webauth_context *ctx, struct webauth_krb5 *kc,
                 const char *name, krb5_principal *princ)
{
    krb5_error_code code;

    code = krb5_parse_name(kc->ctx, name, princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot parse principal %s", name);
    return WA_ERR_NONE;
}


/*
 * Open up a keytab and return a krb5_principal to use with that keytab.  If
 * in_principal is NULL, returned out_principal is the first principal found
 * in keytab.  The caller is responsible for freeing the returned principal
 * and keytab.
 */
static int
open_keytab(struct webauth_context *ctx, struct webauth_krb5 *kc,
            const char *path, const char *principal, krb5_principal *princ,
            krb5_keytab *keytab)
{
    krb5_keytab id = NULL;
    krb5_kt_cursor cursor;
    krb5_keytab_entry entry;
    krb5_error_code code;
    bool cursor_valid = false;

    /* Initialize return values in the case of an error. */
    *princ = NULL;
    *keytab = NULL;

    /* Open the keytab file and optionally find its first principal. */
    code = krb5_kt_resolve(kc->ctx, path, &id);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot open keytab %s", path);
    if (principal != NULL) {
        code = krb5_parse_name(kc->ctx, principal, princ);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot parse principal %s", principal);
            goto fail;
        }
    } else {
        code = krb5_kt_start_seq_get(kc->ctx, id, &cursor);
        if (code == 0) {
            cursor_valid = true;
            code = krb5_kt_next_entry(kc->ctx, id, &entry, &cursor);
        }
        if (code != 0) {
            error_set(ctx, kc, code, "cannot read keytab %s", path);
            goto fail;
        }
        code = krb5_copy_principal(kc->ctx, entry.principal, princ);
        if (code != 0)
            error_set(ctx, kc, code, "cannot copy principal");
        krb5_kt_free_entry(kc->ctx, &entry);
        if (code != 0)
            goto fail;
        krb5_kt_end_seq_get(kc->ctx, id, &cursor);
        cursor_valid = false;
    }
    *keytab = id;
    return WA_ERR_NONE;

fail:
    if (cursor_valid)
        krb5_kt_end_seq_get(kc->ctx, id, &cursor);
    if (id != NULL)
        krb5_kt_close(kc->ctx, id);
    return WA_ERR_KRB5;
}


/*
 * Free the contents of the webauth_krb5 context that hold separately
 * allocated memory.  Don't free anything pool-allocated, since that will be
 * taken care of by pool cleanup.  This function is registered as an APR pool
 * cleanup function.
 */
static apr_status_t
cleanup(void *data)
{
    struct webauth_krb5 *kc = data;

    if (kc->cc != NULL)
        krb5_cc_destroy(kc->ctx, kc->cc);
    if (kc->princ != NULL)
        krb5_free_principal(kc->ctx, kc->princ);
    if (kc->ctx != NULL)
        krb5_free_context(kc->ctx);
    return APR_SUCCESS;
}


/*
 * Create a new webauth_krb5 context.  Any contents of the provided context
 * are overwritten.
 *
 * Every webauth_krb5 context gets its own sub-pool of the WebAuth context
 * pool, which is used for any memory allocated by the WebAuth Kerberos code.
 * We additionally register webauth_krb5_free as a cleanup handler called by
 * APR when the pool is destroyed so that everything is properly freed when
 * the pool is deleted and no explicit free is required.
 */
int
webauth_krb5_new(struct webauth_context *ctx, struct webauth_krb5 **kc)
{
    apr_pool_t *pool;
    krb5_error_code code;

    if (apr_pool_create(&pool, ctx->pool) != APR_SUCCESS) {
        wai_error_set(ctx, WA_ERR_APR, "cannot create new APR pool");
        return WA_ERR_APR;
    }
    *kc = apr_pcalloc(pool, sizeof(struct webauth_krb5));
    (*kc)->pool = pool;
    code = krb5_init_context(&(*kc)->ctx);
    if (code != 0)
        return error_set(ctx, NULL, code, "cannot create Kerberos context");
    apr_pool_cleanup_register(pool, *kc, cleanup, apr_pool_cleanup_null);
    return WA_ERR_NONE;
}


/*
 * Frees the webauth_krb5 context including any memory allocated within that
 * context.  Since we've registered a pool cleanup function, all we have to do
 * is trigger pool destruction of our sub-pool.
 */
void
webauth_krb5_free(struct webauth_context *ctx UNUSED, struct webauth_krb5 *kc)
{
    apr_pool_destroy(kc->pool);
}


/*
 * Set up the ticket cache that will be used to store the credentials
 * associated with a webauth_krb5 context.  This is shared by all the
 * webauth_krb5_init_via_* and webauth_import_cred functions.  Uses a memory
 * cache if no cache identifier is given.
 */
static int
setup_cache(struct webauth_context *ctx, struct webauth_krb5 *kc,
            const char *cache)
{
    krb5_error_code code;

    /* apr_psprintf doesn't support %p. */
    if (cache == NULL)
        cache = apr_psprintf(kc->pool, "MEMORY:%#lx", (unsigned long) kc);
    code = krb5_cc_resolve(kc->ctx, cache, &kc->cc);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot create Kerberos cache %s",
                         cache);
    code = krb5_cc_initialize(kc->ctx, kc->cc, kc->princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot initialize Kerberos cache %s",
                         cache);
    return WA_ERR_NONE;
}


/*
 * Translate a Kerberos error code from a krb5_get_init_creds* function into
 * an appropriate WebAuth code, setting the WebAuth error message at the same
 * time.  Returns the WebAuth status code that we set.
 */
static int
translate_error(struct webauth_context *ctx, krb5_error_code code)
{
    switch (code) {
    case KRB5KRB_AP_ERR_BAD_INTEGRITY:
    case KRB5KDC_ERR_PREAUTH_FAILED:
    case KRB5KDC_ERR_C_PRINCIPAL_UNKNOWN:
        ctx->code = WA_ERR_LOGIN_FAILED;
        break;
    case KRB5KDC_ERR_KEY_EXP:
        ctx->code = WA_ERR_CREDS_EXPIRED;
        break;
    case KRB5KDC_ERR_POLICY:
    case KRB5KDC_ERR_NAME_EXP:
        ctx->code = WA_ERR_USER_REJECTED;
        break;
    default:
        ctx->code = WA_ERR_KRB5;
        break;
    }
    return ctx->code;
}


/*
 * Initialize a context from an existing ticket cache.  If the cache name is
 * NULL, uses krb5_cc_default to determine the ticket cache.
 */
int
webauth_krb5_init_via_cache(struct webauth_context *ctx,
                            struct webauth_krb5 *kc, const char *cache)
{
    krb5_error_code code;

    if (cache != NULL) {
        code = krb5_cc_resolve(kc->ctx, cache, &kc->cc);
        if (code != 0)
            return error_set(ctx, kc, code, "cannot open cache %s", cache);
    } else {
        code = krb5_cc_default(kc->ctx, &kc->cc);
        if (code != 0)
            return error_set(ctx, kc, code, "cannot get default cache");
    }
    code = krb5_cc_get_principal(kc->ctx, kc->cc, &kc->princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot get principal from cache");
    return WA_ERR_NONE;
}


/*
 * Initialize a context from a keytab and obtain a TGT.  Optionally takes a
 * cache name, which if NULL indicates to use a memory cache.
 */
int
webauth_krb5_init_via_keytab(struct webauth_context *ctx,
                             struct webauth_krb5 *kc,
                             const char *keytab,
                             const char *principal,
                             const char *cache)
{
    krb5_creds creds;
    krb5_get_init_creds_opt *opts;
    krb5_keytab kt;
    krb5_error_code code;
    int status = WA_ERR_NONE;

    /* Initialize arguments and setup ticket cache. */
    status = open_keytab(ctx, kc, keytab, principal, &kc->princ, &kt);
    if (status != WA_ERR_NONE)
        return status;
    status = setup_cache(ctx, kc, cache);
    if (status != WA_ERR_NONE) {
        krb5_kt_close(kc->ctx, kt);
        return status;
    }

    /*
     * Set the credential options.
     *
     * FIXME: We should set some initial credential options here similar to in
     * webauth_krb5_init_via_password.
     */
    code = krb5_get_init_creds_opt_alloc(kc->ctx, &opts);
    if (code != 0) {
        krb5_kt_close(kc->ctx, kt);
        return error_set(ctx, kc, code, "cannot allocate credential options");
    }
    krb5_get_init_creds_opt_set_default_flags(kc->ctx, "webauth", NULL, opts);

    /*
     * Obtain credentials and translate the error, if any, into an appropriate
     * WebAuth error code.
     */
    code = krb5_get_init_creds_keytab(kc->ctx, &creds, kc->princ, kt, 0, NULL,
                                      opts);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot authenticate with keytab %s", keytab);
        status = translate_error(ctx, code);
    }
    krb5_get_init_creds_opt_free(kc->ctx, opts);
    krb5_kt_close(kc->ctx, kt);
    if (status != WA_ERR_NONE)
        return status;

    /* Add the credentials to the cache. */
    code = krb5_cc_store_cred(kc->ctx, kc->cc, &creds);
    krb5_free_cred_contents(kc->ctx, &creds);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot store credentials in cache");
    return WA_ERR_NONE;
}


/*
 * Obtain a credentials from a user's password, verifying it with the provided
 * keytab and server principal if given.  If no keytab is given or if a
 * specific target principal is requested via get_principal, we do not verify
 * the TGT, and server_principal_out is not set.  Optionally takes a cache
 * name, which if NULL indicates to use a memory cache.
 */
int
webauth_krb5_init_via_password(struct webauth_context *ctx,
                               struct webauth_krb5 *kc,
                               const char *username, const char *password,
                               const char *get_principal,
                               const char *keytab,
                               const char *server_principal,
                               const char *cache,
                               char **server_principal_out)
{
    krb5_creds creds;
    krb5_get_init_creds_opt *opts;
    krb5_error_code code;
    int status;

    /* Initialize arguments and set up ticket cache. */
    code = krb5_parse_name(kc->ctx, username, &kc->princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot parse principal %s", username);
    status = setup_cache(ctx, kc, cache);
    if (status != WA_ERR_NONE)
        return status;

    /* Set the credential options. */
    code = krb5_get_init_creds_opt_alloc(kc->ctx, &opts);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot allocate credential options");
    krb5_get_init_creds_opt_set_default_flags(kc->ctx, "webauth", NULL, opts);
    if (get_principal == NULL)
        krb5_get_init_creds_opt_set_forwardable(opts, 1);
    else {
        krb5_get_init_creds_opt_set_forwardable(opts, 0);
        krb5_get_init_creds_opt_set_proxiable(opts, 0);
        krb5_get_init_creds_opt_set_renew_life(opts, 0);
    }

    /*
     * Obtain credentials and translate the error, if any, into an appropriate
     * WebAuth error code.
     */
    code = krb5_get_init_creds_password(kc->ctx, &creds, kc->princ,
                                        (char *) password, NULL, NULL, 0,
                                        (char *) get_principal, opts);
    krb5_get_init_creds_opt_free(kc->ctx, opts);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot authenticate as %s", username);
        return translate_error(ctx, code);
    }

    /* Verify the credentials if possible. */
    if (get_principal == NULL && keytab != NULL) {
        krb5_principal princ = NULL;
        krb5_keytab kt = NULL;
        char *name;

        status = open_keytab(ctx, kc, keytab, server_principal, &princ, &kt);
        if (status != WA_ERR_NONE) {
            krb5_free_cred_contents(kc->ctx, &creds);
            return status;
        }
        code = krb5_verify_init_creds(kc->ctx, &creds, princ, kt, NULL, NULL);
        if (code != 0)
            error_set(ctx, kc, code, "credential verification failed for %s",
                      username);
        if (code == 0 && server_principal_out != NULL) {
            code = krb5_unparse_name(kc->ctx, princ, &name);
            if (code == 0) {
                *server_principal_out = apr_pstrdup(kc->pool, name);
                krb5_free_unparsed_name(kc->ctx, name);
            } else {
                error_set(ctx, kc, code, "cannot unparse server principal");
            }
        }
        krb5_kt_close(kc->ctx, kt);
        krb5_free_principal(kc->ctx, princ);
        if (code != 0) {
            krb5_free_cred_contents(kc->ctx, &creds);
            return WA_ERR_KRB5;
        }
    }

    /* Add the credentials to the cache. */
    code = krb5_cc_store_cred(kc->ctx, kc->cc, &creds);
    krb5_free_cred_contents(kc->ctx, &creds);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot store credentials in cache");
    return WA_ERR_NONE;
}


/*
 * Prepare a context from obtained credentials.  This uses existing
 * credentials to determine the principal and store that principal in the
 * webauth_krb5 context, but doesn't store any credentials.  It contains the
 * common code between webauth_prepare_via_cred and webauth_import_cred.
 */
static int
prepare_from_creds(struct webauth_context *ctx, struct webauth_krb5 *kc,
                   krb5_creds *creds, const char *cache)
{
    krb5_error_code code;
    int status;

    code = krb5_copy_principal(kc->ctx, creds->client, &kc->princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot copy principal");
    status = setup_cache(ctx, kc, cache);
    if (status != WA_ERR_NONE)
        return status;
    return WA_ERR_NONE;
}


/*
 * Initialize a context from a passed, delegated credential, but do
 * not import the credential.
 */
int
webauth_krb5_prepare_via_cred(struct webauth_context *ctx,
                              struct webauth_krb5 *kc, const void *cred,
                              size_t length, const char *cache)
{
    krb5_creds creds;
    int status;

    status = decode_creds(ctx, kc, cred, length, &creds);
    if (status != WA_ERR_NONE)
        return status;
    return prepare_from_creds(ctx, kc, &creds, cache);
}


/*
 * Import a credential that was exported with webauth_krb5_export_cred into a
 * webauth_krb5 context.  If the webauth_krb5 context has not yet been
 * initialized, it will be initialized using the provided ticket cache
 * identifier.  If the cache parameter is NULL and the context is not yet
 * initialized, a memory cache will be used.
 */
int
webauth_krb5_import_cred(struct webauth_context *ctx, struct webauth_krb5 *kc,
                         const void *cred, size_t cred_len, const char *cache)
{
    krb5_creds creds;
    krb5_error_code code;
    int status;

    /* Decode the credential. */
    status = decode_creds(ctx, kc, cred, cred_len, &creds);
    if (status != WA_ERR_NONE)
        return status;

    /* If the webauth_krb5 context is not initialized, do that now. */
    if (kc->cc == NULL) {
        status = prepare_from_creds(ctx, kc, &creds, cache);
        if (status != WA_ERR_NONE)
            return status;
    }

    /*
     * Add the creds to the cache.  We have to be careful about memory
     * management here, since only the principals are allocated by the
     * Kerberos libraries.  Everything else in the creds struct is in pool
     * storage and we'll segfault if we try to free it.
     */
    code = krb5_cc_store_cred(kc->ctx, kc->cc, &creds);
    if (creds.client != NULL)
        krb5_free_principal(kc->ctx, creds.client);
    if (creds.server != NULL)
        krb5_free_principal(kc->ctx, creds.server);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot store credentials in cache");
    return WA_ERR_NONE;
}


/*
 * Export a credential into the encoded form that we put into tokens, used for
 * delegating credentials or storing credentials in cookies.  If server is
 * NULL, export the TGT for the principal's realm.
 */
int
webauth_krb5_export_cred(struct webauth_context *ctx, struct webauth_krb5 *kc,
                         const char *server, void **ticket, size_t *length,
                         time_t *expiration)
{
    krb5_creds in, *out;
    const char *realm;
    krb5_error_code code;
    int status = WA_ERR_KRB5;

    memset(&in, 0, sizeof(in));
    code = krb5_cc_get_principal(kc->ctx, kc->cc, &in.client);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot get client principal");
        goto done;
    }
    if (server == NULL) {
        realm = krb5_principal_get_realm(kc->ctx, in.client);
        if (realm == NULL) {
            status = WA_ERR_INVALID_CONTEXT;
            wai_error_set(ctx, status, "no realm");
            goto done;
        }
        code = krb5_build_principal_ext(kc->ctx, &in.server,
                                        strlen(realm), realm,
                                        KRB5_TGS_NAME_SIZE, KRB5_TGS_NAME,
                                        strlen(realm), realm, 0);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot build krbtgt principal");
            goto done;
        }
    } else {
        code = krb5_parse_name(kc->ctx, server, &in.server);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot parse principal %s", server);
            goto done;
        }
    }
    code = krb5_get_credentials(kc->ctx, 0, kc->cc, &in, &out);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot get credentials");
        goto done;
    }
    status = encode_creds(ctx, kc, out, ticket, length, expiration);
    krb5_free_creds(kc->ctx, out);

done:
    krb5_free_cred_contents(kc->ctx, &in);
    return status;
}


/*
 * Canonicalize a principal name and return the results in newly-allocated
 * pool memory.
 *
 * Principal canonicalization is controlled by the third argument.  If it's
 * WA_KRB5_CANON_NONE, do no canonicalization.  If it's WA_KRB5_CANON_LOCAL,
 * run the principal through krb5_aname_to_localname first to try to generate
 * a local username and fall through a fully-qualified name.  If it's
 * WA_KRB5_CANON_STRIP, strip the realm from the principal, whatever it may
 * be.
 */
static int
canonicalize_principal(struct webauth_context *ctx, struct webauth_krb5 *kc,
                       krb5_principal princ, char **principal,
                       enum webauth_krb5_canon canon)
{
    char *name;
    char lname[BUFSIZ];
    krb5_error_code code = 0;

    switch (canon) {
    case WA_KRB5_CANON_LOCAL:
        code = krb5_aname_to_localname(kc->ctx, princ, sizeof(lname), lname);
        if (code == 0) {
            *principal = apr_pstrdup(kc->pool, lname);
            return WA_ERR_NONE;
        }
        /* Fall through. */
    case WA_KRB5_CANON_NONE:
        code = krb5_unparse_name(kc->ctx, princ, &name);
        break;
    case WA_KRB5_CANON_STRIP:
        code = krb5_unparse_name_flags(kc->ctx, princ,
                                       KRB5_PRINCIPAL_UNPARSE_NO_REALM, &name);
        break;
    }
    if (code != 0)
        return error_set(ctx, kc, code, "cannot unparse principal");
    *principal = apr_pstrdup(kc->pool, name);
    krb5_free_unparsed_name(kc->ctx, name);
    return WA_ERR_NONE;
}


/*
 * Get the principal from a context.  This is mostly a wrapper around
 * canonicalize_principal.
 */
int
webauth_krb5_get_principal(struct webauth_context *ctx,
                           struct webauth_krb5 *kc, char **principal,
                           enum webauth_krb5_canon canon)
{
    if (kc->princ == NULL) {
        wai_error_set(ctx, WA_ERR_INVALID_CONTEXT,
                      "Kerberos context not initialized");
        return WA_ERR_INVALID_CONTEXT;
    }
    return canonicalize_principal(ctx, kc, kc->princ, principal, canon);
}


/*
 * Get the authentication realm from the context.  Stores the newly allocated
 * string in realm and returns WA_ERR_NONE on success, or another error code
 * on failure.
 */
int
webauth_krb5_get_realm(struct webauth_context *ctx, struct webauth_krb5 *kc,
                       char **realm)
{
    const char *result;

    if (kc->princ == NULL) {
        wai_error_set(ctx, WA_ERR_INVALID_CONTEXT,
                      "Kerberos context not initialized");
        return WA_ERR_INVALID_CONTEXT;
    }
    result = krb5_principal_get_realm(kc->ctx, kc->princ);
    if (result == NULL) {
        wai_error_set(ctx, WA_ERR_INVALID_CONTEXT, "no realm");
        return WA_ERR_INVALID_CONTEXT;
    }
    *realm = apr_pstrdup(kc->pool, result);
    return WA_ERR_NONE;
}


/*
 * Get the full ticket cache designator from the context.  Stores the newly
 * allocated string in cache and returns WA_ERR_NONE on success, or another
 * error code on failure.
 */
int
webauth_krb5_get_cache(struct webauth_context *ctx, struct webauth_krb5 *kc,
                       char **cache)
{
    krb5_error_code code;
    char *result;

    if (kc->cc == NULL) {
        wai_error_set(ctx, WA_ERR_INVALID_CONTEXT,
                      "Kerberos context not initialized");
        return WA_ERR_INVALID_CONTEXT;
    }
    code = krb5_cc_get_full_name(kc->ctx, kc->cc, &result);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot get cache name");
    *cache = apr_pstrdup(kc->pool, result);
    krb5_free_string(kc->ctx, result);
    return WA_ERR_NONE;
}


/*
 * Create an encoded Kerberos request.  The request is stored in output in
 * newly allocated pool memory and the length is stored in length.
 * Optionally, also encrypts some data with the session key and stores the
 * encrypted data in out_data.  Returns a WA_ERR code.
 *
 * This is used as an authenticator from a WAS to the WebKDC.  The version
 * with encrypted data is used to request a webkdc-proxy token from the WebKDC
 * using a Kerberos TGT.
 */
int
webauth_krb5_make_auth_data(struct webauth_context *ctx,
                            struct webauth_krb5 *kc,
                            const char *server_principal,
                            void **req, size_t *length,
                            const void *in_data, size_t in_length,
                            void **out_data, size_t *out_length)
{
    krb5_creds increds;
    krb5_creds *outcreds = NULL;
    krb5_data out;
    krb5_auth_context auth = NULL;
    krb5_principal princ = NULL;
    krb5_error_code code;

    /* Clear our data. */
    memset(&out, 0, sizeof(out));
    memset(&increds, 0, sizeof(increds));

    /* Generate the request. */
    code = krb5_parse_name(kc->ctx, server_principal, &princ);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot parse principal %s",
                         server_principal);
    code = krb5_copy_principal(kc->ctx, princ, &increds.server);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot copy principal");
        goto done;
    }
    code = krb5_cc_get_principal(kc->ctx, kc->cc, &increds.client);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot get principal from cache");
        goto done;
    }
    code = krb5_get_credentials(kc->ctx, 0, kc->cc, &increds, &outcreds);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot get credentials for %s",
                  server_principal);
        goto done;
    }
    code = krb5_mk_req_extended(kc->ctx, &auth, 0, NULL, outcreds, &out);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot make request for principal %s",
                  server_principal);
        krb5_free_data_contents(kc->ctx, &out);
        goto done;
    }

    /* Copy the results into pool memory. */
    *req = apr_pmemdup(kc->pool, out.data, out.length);
    *length = out.length;
    krb5_free_data_contents(kc->ctx, &out);

    /* If there is data to encrypt, do so. */
    if (in_data != NULL && out_data != NULL) {
        krb5_data in;
        krb5_address laddr;

        /*
         * We cheat here and always use localhost as the address.  This is an
         * ugly hack, but then so is address checking, and we have other
         * security around use of the tokens.
         */
#ifdef HAVE_KRB5_MIT
        const krb5_octet address[4] = { 127, 0, 0, 1 };
        laddr.magic = KV5M_ADDRESS;
        laddr.addrtype = ADDRTYPE_INET;
        laddr.length = 4;
        laddr.contents = (void *) address;
#else
        const char address[4] = { 127, 0, 0, 1 };
        laddr.addr_type = KRB5_ADDRESS_INET;
        laddr.address.length = 4;
        laddr.address.data = (void *) address;
#endif
        code = krb5_auth_con_setflags(kc->ctx, auth, 0);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot set context flags");
            goto done;
        }
        code = krb5_auth_con_setaddrs(kc->ctx, auth, &laddr, NULL);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot set context addresses");
            goto done;
        }
        
        /* Do the data encryption. */
        in.data = (void *) in_data;
        in.length = in_length;
        code = krb5_mk_priv(kc->ctx, auth, &in, &out, NULL);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot encrypt data");
            goto done;
        }
        *out_data = apr_pmemdup(kc->pool, out.data, out.length);
        *out_length = out.length;
        krb5_free_data_contents(kc->ctx, &out);
    }

done:
    if (auth != NULL)
        krb5_auth_con_free(kc->ctx, auth);
    if (outcreds != NULL)
        krb5_free_creds(kc->ctx, outcreds);
    krb5_free_principal(kc->ctx, princ);
    krb5_free_cred_contents(kc->ctx, &increds);
    return (code == 0) ? WA_ERR_NONE : WA_ERR_KRB5;
}


/*
 * Simpler version of webauth_krb5_make_auth_data without any data.  Most
 * callers will be able to use this.
 */
int
webauth_krb5_make_auth(struct webauth_context *ctx, struct webauth_krb5 *kc,
                       const char *server_principal, void **req,
                       size_t *length)
{
    return webauth_krb5_make_auth_data(ctx, kc, server_principal, req, length,
                                       NULL, 0, NULL, 0);
}


/*
 * Receive and decrypt a Kerberos request using a local keytab.  The principal
 * making the remote Kerberos request is stored in client_principal and the
 * server principal to which the request was addressed is stored in
 * out_server_principal.  Optionally, also decrypts some data with the session
 * key and stores the decrypted data in out_data.  Returns a WA_ERR code.
 */
int
webauth_krb5_read_auth_data(struct webauth_context *ctx,
                            struct webauth_krb5 *kc,
                            const void *req, size_t length,
                            const char *keytab,
                            const char *server_principal,
                            char **server, char **client,
                            enum webauth_krb5_canon canon,
                            const void *in_data, size_t in_length,
                            void **out_data, size_t *out_length)
{
    krb5_principal sprinc, cprinc;
    krb5_keytab kt;
    krb5_auth_context auth = NULL;
    krb5_data buf;
#ifdef HAVE_KRB5_MIT
    krb5_authenticator *ka = NULL;
#else
    krb5_authenticator ka = NULL;
#endif
    krb5_error_code code;
    int status;
    char *name;

    /* Initial setup. */
    status = open_keytab(ctx, kc, keytab, server_principal, &sprinc, &kt);
    if (status != WA_ERR_NONE)
        return status;
    auth = NULL;

    /* Read and analyze the request. */
    buf.data = (void *) req;
    buf.length = length;
    code = krb5_rd_req(kc->ctx, &auth, &buf, sprinc, kt, NULL, NULL);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot read authenticator");
    code = krb5_auth_con_getauthenticator(kc->ctx, auth, &ka);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot determine client identity");
        goto done;
    }
#ifdef HAVE_KRB5_MIT
    cprinc = ka->client;
#else
    cprinc = apr_palloc(kc->pool, sizeof(*cprinc));
    cprinc->name = ka->cname;
    cprinc->realm = ka->crealm;
#endif
    status = canonicalize_principal(ctx, kc, cprinc, client, canon);

    /* Decrypt the data if any. */
    if (in_data != NULL && out_data != NULL) {
        krb5_data in, out;
        krb5_address raddr;

        /*
         * We cheat here and always use localhost as the address.  This is an
         * ugly hack, but then so is address checking, and we have other
         * security around use of the tokens.
         */
#ifdef HAVE_KRB5_MIT
        const krb5_octet address[4] = { 127, 0, 0, 1 };
        raddr.magic = KV5M_ADDRESS;
        raddr.addrtype = ADDRTYPE_INET;
        raddr.length = 4;
        raddr.contents = (void *) address;
#else
        const char address[4] = { 127, 0, 0, 1 };
        raddr.addr_type = KRB5_ADDRESS_INET;
        raddr.address.length = 4;
        raddr.address.data = (void *) address;
#endif
        code = krb5_auth_con_setflags(kc->ctx, auth, 0);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot set context flags");
            goto done;
        }
        code = krb5_auth_con_setaddrs(kc->ctx, auth, NULL, &raddr);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot set context addresses");
            goto done;
        }

        /* Do the data decryption. */
        in.data = (void *) in_data;
        in.length = in_length;
        code = krb5_rd_priv(kc->ctx, auth, &in, &out, NULL);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot encrypt data");
            goto done;
        }
        *out_data = apr_pmemdup(kc->pool, out.data, out.length);
        *out_length = out.length;
        krb5_free_data_contents(kc->ctx, &out);
    }

    /* Determine the server name, if desired. */
    if (server != NULL) {
        code = krb5_unparse_name(kc->ctx, sprinc, &name);
        if (code != 0) {
            error_set(ctx, kc, code, "cannot unparse server principal");
            goto done;
        }
        *server = apr_pstrdup(kc->pool, name);
        krb5_free_unparsed_name(kc->ctx, name);
    }

done:
    if (auth != NULL)
        krb5_auth_con_free(kc->ctx, auth);
#ifdef HAVE_KRB5_MIT
    if (ka != NULL)
        krb5_free_authenticator(kc->ctx, ka);
#else
    if (ka != NULL)
        krb5_free_authenticator(kc->ctx, &ka);
#endif
    krb5_kt_close(kc->ctx, kt);
    krb5_free_principal(kc->ctx, sprinc);
    if (status == WA_ERR_NONE && code != 0)
        status = WA_ERR_KRB5;
    return status;
}


/*
 * Change a user's password, given context and the new password.  The user to
 * change should be already in the context, which should also have credentials
 * for kadmin/changepw in order to perform the change.
 */
int
webauth_krb5_change_password(struct webauth_context *ctx,
                             struct webauth_krb5 *kc,
                             const char *password)
{
    krb5_error_code code;
    int result_code = 0;
    krb5_data result_code_string, result_string;
    char *username = NULL;
#if HAVE_KRB5_MIT
    krb5_creds in, *out = NULL;
#endif

    /* Initialize the data we need to do the change. */
    memset(&result_code_string, 0, sizeof(krb5_data));
    memset(&result_string, 0, sizeof(krb5_data));
    code = krb5_unparse_name(kc->ctx, kc->princ, &username);
    if (code != 0)
        return error_set(ctx, kc, code, "cannot unparse principal name");

    /*
     * The actual change.  MIT Kerberos up to at least 1.9 has a bug in the
     * set_password implementation that causes it to misparse replies that are
     * larger than 256 bytes and return an incorrect error code, so for MIT
     * Kerberos we use the old change_password API instead.
     */
#if HAVE_KRB5_MIT
    memset(&in, 0, sizeof(in));
    code = krb5_copy_principal(kc->ctx, kc->princ, &in.client);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot copy principal");
        goto done;
    }
    code = krb5_build_principal(kc->ctx, &in.server,
                                krb5_princ_realm(kc->ctx, kc->princ)->length,
                                krb5_princ_realm(kc->ctx, kc->princ)->data,
                                "kadmin", "changepw", NULL);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot build kadmin/changepw principal");
        goto done;
    }
    code = krb5_get_credentials(kc->ctx, 0, kc->cc, &in, &out);
    if (code != 0) {
        error_set(ctx, kc, code, "cannot obtain kadmin/changepw credentials"
                  " for %s", username);
        goto done;
    }
    code = krb5_change_password(kc->ctx, out, (char *) password,
                                &result_code, &result_code_string,
                                &result_string);
#else
    code = krb5_set_password_using_ccache(kc->ctx, kc->cc, (char *) password,
                                          kc->princ, &result_code,
                                          &result_code_string, &result_string);
#endif /* !HAVE_KRB5_MIT */

    /* Everything from here on is just handling diagnostics and output. */
    if (code != 0) {
        error_set(ctx, kc, code, "cannot change password for %s", username);
        goto done;
    }
    if (result_code != 0) {
        wai_error_set(ctx, WA_ERR_KRB5, "password change failed for %s: (%d)"
                      " %.*s%s%.*s", username, result_code,
                      (int) result_code_string.length,
                      (char *) result_code_string.data,
                      result_string.length == 0 ? "" : ": ",
                      (int) result_string.length,
                      (char *) result_string.data);
        goto done;
    }

done:
    krb5_free_data_contents(kc->ctx, &result_string);
    krb5_free_data_contents(kc->ctx, &result_code_string);
    if (username != NULL)
        krb5_free_unparsed_name(kc->ctx, username);
#if HAVE_KRB5_MIT
    krb5_free_cred_contents(kc->ctx, &in);
    if (out != NULL)
        krb5_free_creds(kc->ctx, out);
#endif
    return (code == 0 && result_code == 0) ? WA_ERR_NONE : WA_ERR_KRB5;
}


/*
 * Simpler version of webauth_krb5_read_auth_data without any data.  Most
 * callers will be able to use this.
 */
int
webauth_krb5_read_auth(struct webauth_context *ctx, struct webauth_krb5 *kc,
                       const void *req, size_t length, const char *keytab,
                       const char *server_principal, char **client_principal,
                       enum webauth_krb5_canon canon)
{
    return webauth_krb5_read_auth_data(ctx, kc, req, length, keytab,
                                       server_principal, NULL,
                                       client_principal, canon, NULL, 0,
                                       NULL, 0);
}
