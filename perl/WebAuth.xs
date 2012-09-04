/* -*- c -*-
 * Perl XS bindings for the WebAuth library.
 *
 * This is an XS source file, suitable for processing by xsubpp, that
 * generates Perl bindings for the libwebauth library.
 *
 * Currently, both the library API and this set of bindings leave a lot to be
 * desired.  They could be much more object-oriented than they currently are,
 * and the library is very low-level.  There is some work done on conversion
 * to a more object-oriented approach, but it's very incomplete and currently
 * only includes WebAuth::Keyring and closely-related classes.
 *
 * All abnormal errors are handled as exceptions, generated via webauth_croak,
 * rather than through error returns.
 *
 * Wrap all CODE and PPCODE segments in this file longer than a single line in
 * braces to reduce Emacs's c-mode confusion when trying to understand XS
 * constructs.
 *
 * Written by Roland Schemers
 * Copyright 2003, 2005, 2006, 2008, 2009, 2010, 2011, 2012
 *     The Board of Trustees of the Leland Stanford Junior University
 *
 * See LICENSE for licensing terms.
 */

/* We cannot include config.h here because it conflicts with Perl. */
#include <portable/apr.h>

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include <webauth.h>
#include <webauth/basic.h>
#include <webauth/keys.h>
#include <webauth/krb5.h>
#include <webauth/tokens.h>

/*
 * These typedefs are needed for xsubpp to work its magic with type
 * translation to Perl objects.
 */
typedef struct webauth_context *                WebAuth;
typedef const struct webauth_key *              WebAuth__Key;
typedef const struct webauth_keyring_entry *    WebAuth__KeyringEntry;

/*
 * For WebAuth::Keyring and WebAuth::Krb5, we need to stash a copy of the
 * parent context somewhere so that we don't require it as an argument to all
 * methods.
 */
typedef struct {
    struct webauth_context *ctx;
    struct webauth_keyring *ring;
} *WebAuth__Keyring;
typedef struct {
    struct webauth_context *ctx;
    struct webauth_krb5 *kc;
} *WebAuth__Krb5;

/* Used to generate the Perl glue for WebAuth constants. */
#define IV_CONST(X) newCONSTSUB(stash, #X, newSViv(X))

/*
 * Encoding and decoding magic for tokens.
 *
 * There are significant memory management challenges for how to handle the
 * token structs as Perl objects directly.  (For example, the structs
 * allocated via webauth_token_decode would be allocated entirely differently
 * than values set from Perl.)  It's therefore easier to have the Perl
 * expression of the token structs be Perl hash tables, which we can bless and
 * wrap with getters and setters written in Perl instead of XS.
 *
 * In order to avoid vast amounts of duplicate code, we define coding tables
 * for each token.  This maps the attribute names in the form of the hash keys
 * used in the Perl object to offsets into the relevant token struct and types
 * of the C data structure.  We can then use some (scary) C code to handle the
 * transcoding from C structs to Perl objects generically for all token types.
 *
 * All TYPE_DATA entries in the table must be followed by TYPE_DATALEN
 * entries.
 */
enum token_mapping_type {
    TYPE_STRING,
    TYPE_TIME,
    TYPE_DATA,
    TYPE_DATALEN,
    TYPE_ULONG
};
struct token_mapping {
    const char *key;
    size_t offset;
    enum token_mapping_type type;
};

/* Used to make the tables more readable. */
#define M(s, n, t) { (#n), offsetof(struct s, n), TYPE_ ## t }

/* Used to store or extract data from the struct. */
#define DATA_STRING(d, o)       *(const char **)  ((char *) (d) + (o))
#define DATA_TIME(d, o)         *(time_t *)       ((char *) (d) + (o))
#define DATA_DATA(d, o)         *(const void **)  ((char *) (d) + (o))
#define DATA_DATALEN(d, o)      *(size_t *)       ((char *) (d) + (o))
#define DATA_ULONG(d, o)        *(unsigned long *)((char *) (d) + (o))

/* App tokens. */
struct token_mapping token_mapping_app[] = {
    M(webauth_token_app, subject,         STRING),
    M(webauth_token_app, last_used,       TIME),
    M(webauth_token_app, session_key,     DATA),
    M(webauth_token_app, session_key_len, DATALEN),
    M(webauth_token_app, initial_factors, STRING),
    M(webauth_token_app, session_factors, STRING),
    M(webauth_token_app, loa,             ULONG),
    M(webauth_token_app, creation,        TIME),
    M(webauth_token_app, expiration,      TIME),
    { NULL, 0, 0 }
};

/* Cred tokens. */
struct token_mapping token_mapping_cred[] = {
    M(webauth_token_cred, subject,    STRING),
    M(webauth_token_cred, type,       STRING),
    M(webauth_token_cred, service,    STRING),
    M(webauth_token_cred, data,       DATA),
    M(webauth_token_cred, data_len,   DATALEN),
    M(webauth_token_cred, creation,   TIME),
    M(webauth_token_cred, expiration, TIME),
    { NULL, 0, 0 }
};

/* Error tokens. */
struct token_mapping token_mapping_error[] = {
    M(webauth_token_error, code,     ULONG),
    M(webauth_token_error, message,  STRING),
    M(webauth_token_error, creation, TIME),
    { NULL, 0, 0 }
};

/* Id tokens. */
struct token_mapping token_mapping_id[] = {
    M(webauth_token_id, subject,         STRING),
    M(webauth_token_id, auth,            STRING),
    M(webauth_token_id, auth_data,       DATA),
    M(webauth_token_id, auth_data_len,   DATALEN),
    M(webauth_token_id, initial_factors, STRING),
    M(webauth_token_id, session_factors, STRING),
    M(webauth_token_id, loa,             ULONG),
    M(webauth_token_id, creation,        TIME),
    M(webauth_token_id, expiration,      TIME),
    { NULL, 0, 0 }
};

/* Login tokens. */
struct token_mapping token_mapping_login[] = {
    M(webauth_token_login, username, STRING),
    M(webauth_token_login, password, STRING),
    M(webauth_token_login, otp,      STRING),
    M(webauth_token_login, creation, TIME),
    { NULL, 0, 0 }
};

/* Proxy tokens. */
struct token_mapping token_mapping_proxy[] = {
    M(webauth_token_proxy, subject,          STRING),
    M(webauth_token_proxy, type,             STRING),
    M(webauth_token_proxy, webkdc_proxy,     DATA),
    M(webauth_token_proxy, webkdc_proxy_len, DATALEN),
    M(webauth_token_proxy, initial_factors,  STRING),
    M(webauth_token_proxy, session_factors,  STRING),
    M(webauth_token_proxy, loa,              ULONG),
    M(webauth_token_proxy, creation,         TIME),
    M(webauth_token_proxy, expiration,       TIME),
    { NULL, 0, 0 }
};

/* Request tokens. */
struct token_mapping token_mapping_request[] = {
    M(webauth_token_request, type,            STRING),
    M(webauth_token_request, auth,            STRING),
    M(webauth_token_request, proxy_type,      STRING),
    M(webauth_token_request, state,           DATA),
    M(webauth_token_request, state_len,       DATALEN),
    M(webauth_token_request, return_url,      STRING),
    M(webauth_token_request, options,         STRING),
    M(webauth_token_request, initial_factors, STRING),
    M(webauth_token_request, session_factors, STRING),
    M(webauth_token_request, loa,             ULONG),
    M(webauth_token_request, command,         STRING),
    M(webauth_token_request, creation,        TIME),
    { NULL, 0, 0 }
};

/* WebKDC proxy tokens. */
struct token_mapping token_mapping_webkdc_proxy[] = {
    M(webauth_token_webkdc_proxy, subject,         STRING),
    M(webauth_token_webkdc_proxy, proxy_type,      STRING),
    M(webauth_token_webkdc_proxy, proxy_subject,   STRING),
    M(webauth_token_webkdc_proxy, data,            DATA),
    M(webauth_token_webkdc_proxy, data_len,        DATALEN),
    M(webauth_token_webkdc_proxy, initial_factors, STRING),
    M(webauth_token_webkdc_proxy, loa,             ULONG),
    M(webauth_token_webkdc_proxy, creation,        TIME),
    M(webauth_token_webkdc_proxy, expiration,      TIME),
    { NULL, 0, 0 }
};

/* WebKDC service tokens. */
struct token_mapping token_mapping_webkdc_service[] = {
    M(webauth_token_webkdc_service, subject,         STRING),
    M(webauth_token_webkdc_service, session_key,     DATA),
    M(webauth_token_webkdc_service, session_key_len, DATALEN),
    M(webauth_token_webkdc_service, creation,        TIME),
    M(webauth_token_webkdc_service, expiration,      TIME),
    { NULL, 0, 0 }
};


/*
 * Decode a token into a Perl hash.  This function doesn't know what type of
 * token is being decoded; it just takes an array of struct token_mapping, a
 * pointer to the token struct, and a Perl HV, and uses the rules in
 * token_mapping to move data into the HV.
 */
static void
map_token_to_hash(struct token_mapping mapping[], const void *token, HV *hash)
{
    size_t i, length;
    struct token_mapping *map;
    SV *value;
    const char *string;
    const void *data;
    unsigned long number;

    for (i = 0; mapping[i].key != NULL; i++) {
        map = &mapping[i];
        value = NULL;
        switch (map->type) {
        case TYPE_STRING:
            string = DATA_STRING(token, map->offset);
            if (string != NULL)
                value = newSVpv(string, 0);
            break;
        case TYPE_TIME:
            number = DATA_TIME(token, map->offset);
            if (number != 0)
                value = newSViv(number);
            break;
        case TYPE_DATA:
            data = DATA_DATA(token, map->offset);
            if (data != NULL) {
                length = DATA_DATALEN(token, mapping[i + 1].offset);
                value = newSVpvn(data, length);
            }
            break;
        case TYPE_DATALEN:
            /* Handled as part of TYPE_DATA. */
            break;
        case TYPE_ULONG:
            number = DATA_ULONG(token, map->offset);
            if (number != 0)
                value = newSViv(DATA_ULONG(token, map->offset));
            break;
        }
        if (value != NULL)
            if (hv_store(hash, map->key, strlen(map->key), value, 0) == NULL)
                croak("cannot store %s in hash", map->key);
    }
}


/*
 * Encode a Perl hash into a token struct.  This function doesn't know what
 * type of token is being encoded; it just takes an array of struct
 * token_mapping, a pointer to the token struct, and a Perl HV, and uses the
 * rules in token_mapping to move data from the HV into the struct.
 *
 * This does not check for attributes in the HV that don't correspond to valid
 * members of the struct.
 */
static void
map_hash_to_token(struct token_mapping mapping[], HV *hash, const void *token)
{
    size_t i;
    struct token_mapping *map;
    SV **value;
    STRLEN length;

    for (i = 0; mapping[i].key != NULL; i++) {
        map = &mapping[i];
        value = hv_fetch(hash, map->key, strlen(map->key), 0);
        if (value == NULL)
            continue;
        switch (map->type) {
        case TYPE_STRING:
            DATA_STRING(token, map->offset) = SvPV_nolen(*value);
            break;
        case TYPE_TIME:
            DATA_TIME(token, map->offset) = SvIV(*value);
            break;
        case TYPE_DATA:
            DATA_DATA(token, map->offset) = SvPV(*value, length);
            DATA_DATALEN(token, mapping[i + 1].offset) = length;
            break;
        case TYPE_DATALEN:
            /* Handled as part of TYPE_DATA. */
            break;
        case TYPE_ULONG:
            DATA_ULONG(token, map->offset) = SvIV(*value);
            break;
        }
    }
}


/*
 * Turn a WebAuth error into a Perl exception.
 */
static void
webauth_croak(struct webauth_context *ctx, const char *detail, int s)
{
    HV *hv;
    SV *rv;

    hv = newHV();
    (void) hv_store(hv, "status", 6, newSViv(s), 0);
    (void) hv_store(hv, "message", 7,
                    newSVpv(webauth_error_message(ctx, s), 0), 0);
    if (detail != NULL)
        (void) hv_store(hv, "detail", 6, newSVpv(detail, 0), 0);
    if (CopLINE(PL_curcop)) {
        (void) hv_store(hv, "line", 4, newSViv(CopLINE(PL_curcop)), 0);
        (void) hv_store(hv, "file", 4, newSVpv(CopFILE(PL_curcop), 0), 0);
    }
    rv = newRV_noinc((SV *) hv);
    sv_bless(rv, gv_stashpv("WebAuth::Exception", TRUE));
    sv_setsv(get_sv("@", TRUE), sv_2mortal(rv));
    croak(Nullch);
}


/* XS code below this point. */

MODULE = WebAuth        PACKAGE = WebAuth    PREFIX = webauth_

PROTOTYPES: ENABLE


# Generate all the constant subs for all the exported WebAuth constants.
BOOT:
{
    HV *stash;

    /* Constant subs for WebAuth. */
    stash = gv_stashpv("WebAuth", TRUE);

    /* WebAuth error codes. */
    IV_CONST(WA_ERR_NONE);
    IV_CONST(WA_ERR_NO_ROOM);
    IV_CONST(WA_ERR_CORRUPT);
    IV_CONST(WA_ERR_NO_MEM);
    IV_CONST(WA_ERR_BAD_HMAC);
    IV_CONST(WA_ERR_RAND_FAILURE);
    IV_CONST(WA_ERR_BAD_KEY);
    IV_CONST(WA_ERR_FILE_OPENWRITE);
    IV_CONST(WA_ERR_FILE_WRITE);
    IV_CONST(WA_ERR_FILE_OPENREAD);
    IV_CONST(WA_ERR_FILE_READ);
    IV_CONST(WA_ERR_FILE_VERSION);
    IV_CONST(WA_ERR_NOT_FOUND);
    IV_CONST(WA_ERR_KRB5);
    IV_CONST(WA_ERR_INVALID_CONTEXT);
    IV_CONST(WA_ERR_LOGIN_FAILED);
    IV_CONST(WA_ERR_TOKEN_EXPIRED);
    IV_CONST(WA_ERR_TOKEN_STALE);
    IV_CONST(WA_ERR_CREDS_EXPIRED);
    IV_CONST(WA_ERR_USER_REJECTED);
    IV_CONST(WA_ERR_APR);
    IV_CONST(WA_ERR_UNIMPLEMENTED);
    IV_CONST(WA_ERR_INVALID);
    IV_CONST(WA_ERR_REMOTE_FAILURE);
    IV_CONST(WA_ERR_FILE_NOT_FOUND);

    /* Protocol error codes from the WebKDC. */
    IV_CONST(WA_PEC_SERVICE_TOKEN_EXPIRED);
    IV_CONST(WA_PEC_SERVICE_TOKEN_INVALID);
    IV_CONST(WA_PEC_PROXY_TOKEN_EXPIRED);
    IV_CONST(WA_PEC_PROXY_TOKEN_INVALID);
    IV_CONST(WA_PEC_INVALID_REQUEST);
    IV_CONST(WA_PEC_UNAUTHORIZED);
    IV_CONST(WA_PEC_SERVER_FAILURE);
    IV_CONST(WA_PEC_REQUEST_TOKEN_STALE);
    IV_CONST(WA_PEC_REQUEST_TOKEN_INVALID);
    IV_CONST(WA_PEC_GET_CRED_FAILURE);
    IV_CONST(WA_PEC_REQUESTER_KRB5_CRED_INVALID);
    IV_CONST(WA_PEC_LOGIN_TOKEN_STALE);
    IV_CONST(WA_PEC_LOGIN_TOKEN_INVALID);
    IV_CONST(WA_PEC_LOGIN_FAILED);
    IV_CONST(WA_PEC_PROXY_TOKEN_REQUIRED);
    IV_CONST(WA_PEC_LOGIN_CANCELED);
    IV_CONST(WA_PEC_LOGIN_FORCED);
    IV_CONST(WA_PEC_USER_REJECTED);
    IV_CONST(WA_PEC_CREDS_EXPIRED);
    IV_CONST(WA_PEC_MULTIFACTOR_REQUIRED);
    IV_CONST(WA_PEC_MULTIFACTOR_UNAVAILABLE);
    IV_CONST(WA_PEC_LOGIN_REJECTED);
    IV_CONST(WA_PEC_LOA_UNAVAILABLE);
    IV_CONST(WA_PEC_AUTH_REJECTED);

    /* Key types. */
    IV_CONST(WA_KEY_AES);

    /* Key sizes. */
    IV_CONST(WA_AES_128);
    IV_CONST(WA_AES_192);
    IV_CONST(WA_AES_256);

    /* Key usages. */
    IV_CONST(WA_KEY_DECRYPT);
    IV_CONST(WA_KEY_ENCRYPT);

    /* Principal canonicalization methods. */
    IV_CONST(WA_KRB5_CANON_NONE);
    IV_CONST(WA_KRB5_CANON_LOCAL);
    IV_CONST(WA_KRB5_CANON_STRIP);
}


WebAuth
new(class)
    const char *class
  PROTOTYPE: ;$
  PREINIT:
    struct webauth_context *ctx;
    int status;
  CODE:
{
    status = webauth_context_init(&ctx, NULL);
    if (status != WA_ERR_NONE)
        webauth_croak(NULL, "webauth_context_init", status);
    RETVAL = ctx;
}
  OUTPUT:
    RETVAL


void
DESTROY(self)
   WebAuth self
 CODE:
{
   if (self != NULL)
       webauth_context_free(self);
}


const char *
webauth_error_message(self, status)
    WebAuth self
    int status
  PROTOTYPE: $$
  CODE:
    RETVAL = webauth_error_message(self, status);
  OUTPUT:
    RETVAL


void
webauth_hex_encode(self, input)
    WebAuth self
    SV * input
  PROTOTYPE: $$
  CODE:
{
    STRLEN n_input;
    size_t out_len, out_max;
    int s;
    char *p_input;

    p_input = SvPV(input, n_input);
    out_max = webauth_hex_encoded_length(n_input);

    ST(0) = sv_2mortal(NEWSV(0, out_max));
    s = webauth_hex_encode(p_input, n_input, SvPVX(ST(0)), &out_len, out_max);
    if (s != WA_ERR_NONE)
        webauth_croak(NULL, "webauth_hex_encode", s);
    SvCUR_set(ST(0), out_len);
    SvPOK_only(ST(0));
}


void
webauth_hex_decode(self, input)
    WebAuth self
    SV * input
  PROTOTYPE: $
  PPCODE:
{
    STRLEN n_input;
    size_t out_len, out_max;
    int s;
    char *p_input, *buff;
    SV *output;
    buff = NULL;

    p_input = SvPV(input, n_input);
    s = webauth_hex_decoded_length(n_input, &out_max);
    if (s != WA_ERR_NONE)
        webauth_croak(NULL, "webauth_hex_decoded_length", s);
    buff = malloc(out_max);
    if (buff == NULL)
        croak("can't create buffer");
    s = webauth_hex_decode(p_input, n_input, buff, &out_len, out_max);
    if (s != WA_ERR_NONE) {
        if (buff != NULL)
            free(buff);
        webauth_croak(NULL, "webauth_hex_decode", s);
    }

    EXTEND(SP,1);
    output = sv_newmortal();
    sv_setpvn(output, buff, out_len);
    PUSHs(output);

    if (buff != NULL)
        free(buff);
}


void
webauth_attrs_encode(self, attrs)
    WebAuth self
    SV *attrs
  PROTOTYPE: $$
  PPCODE:
{
    HV *h;
    SV *sv_val;
    size_t num_attrs, out_len, out_max;
    int s;
    char *key, *val;
    I32 klen;
    STRLEN vlen;
    WEBAUTH_ATTR_LIST *list;
    SV *output;

    if (!SvROK(attrs) || !(SvTYPE(SvRV(attrs)) == SVt_PVHV))
        croak("attrs must be reference to a hash");
    h = (HV *) SvRV(attrs);

    num_attrs = hv_iterinit(h);
    list = webauth_attr_list_new(num_attrs);
    if (list == NULL)
        croak("can't malloc attr list");

    while ((sv_val = hv_iternextsv(h, &key, &klen)) != NULL) {
        val = SvPV(sv_val, vlen);
        webauth_attr_list_add(list, key, val, vlen, WA_F_NONE);
    }

    out_max = webauth_attrs_encoded_length(list);
    output = sv_2mortal(NEWSV(0, out_max));
    s = webauth_attrs_encode(list, SvPVX(output), &out_len, out_max);
    webauth_attr_list_free(list);
    if (s != WA_ERR_NONE)
        webauth_croak(NULL, "webauth_attrs_encode", s);
    else {
        SvCUR_set(output, out_len);
        SvPOK_only(output);
    }

    EXTEND(SP,1);
    PUSHs(output);
}


void
webauth_attrs_decode(self, buffer)
    WebAuth self
    SV *buffer
  PROTOTYPE: $$
  PPCODE:
{
    size_t n_input;
    char *p_input;
    WEBAUTH_ATTR_LIST *list;
    size_t i;
    int s;
    HV *hv;
    SV *copy = sv_2mortal(newSVsv(buffer));
    SV *output;

    p_input = SvPV(copy, n_input);
    s = webauth_attrs_decode(p_input, n_input, &list);
    if (s != WA_ERR_NONE)
        webauth_croak(NULL, "webauth_attrs_decode", s);

    hv = newHV();
    for (i = 0; i < list->num_attrs; i++)
        (void) hv_store(hv, list->attrs[i].name, strlen(list->attrs[i].name),
                        newSVpvn(list->attrs[i].value, list->attrs[i].length),
                        0);
    webauth_attr_list_free(list);
    output = sv_2mortal(newRV_noinc((SV*)hv));
    EXTEND(SP,1);
    PUSHs(output);
}


WebAuth::Key
key_create(self, type, size, key_material = NULL)
    WebAuth self
    enum webauth_key_type type
    enum webauth_key_size size
    const unsigned char *key_material
  PROTOTYPE: $$$;$
  PREINIT:
    struct webauth_key *key;
    int status;
  CODE:
{
    status = webauth_key_create(self, type, size, key_material, &key);
    if (status != WA_ERR_NONE)
        webauth_croak(self, "webauth_key_create", status);
    RETVAL = key;
}
  OUTPUT:
    RETVAL


WebAuth::Keyring
keyring_new(self, ks)
    WebAuth self
    SV *ks
  PROTOTYPE: $$
  PREINIT:
    WebAuth__Keyring ring;
  CODE:
{
    ring = malloc(sizeof(WebAuth__Keyring));
    if (ring == NULL)
        croak("cannot allocate memory");
    if (sv_isobject(ks) && sv_derived_from(ks, "WebAuth::Key")) {
        struct webauth_key *key;

        key = INT2PTR(struct webauth_key *, SvIV((SV *) SvRV(ks)));
        ring->ring = webauth_keyring_from_key(self, key);
    } else {
        ring->ring = webauth_keyring_new(self, SvIV(ks));
    }
    ring->ctx = self;
    RETVAL = ring;
}
  OUTPUT:
    RETVAL


WebAuth::Keyring
keyring_decode(self, data)
    WebAuth self
    SV *data
  PROTOTYPE: $$
  PREINIT:
    WebAuth__Keyring ring;
    int status;
    const char *encoded;
    STRLEN length;
  CODE:
{
    ring = malloc(sizeof(WebAuth__Keyring));
    if (ring == NULL)
        croak("cannot allocate memory");
    encoded = SvPV(data, length);
    status = webauth_keyring_decode(self, encoded, length, &ring->ring);
    if (status != WA_ERR_NONE)
        webauth_croak(self, "webauth_keyring_decode", status);
    ring->ctx = self;
    RETVAL = ring;
}
  OUTPUT:
    RETVAL


WebAuth::Keyring
keyring_read(self, file)
    WebAuth self
    const char *file
  PROTOTYPE: $$
  PREINIT:
    WebAuth__Keyring ring;
    int status;
  CODE:
{
    ring = malloc(sizeof(WebAuth__Keyring));
    if (ring == NULL)
        croak("cannot allocate memory");
    status = webauth_keyring_read(self, file, &ring->ring);
    if (status != WA_ERR_NONE)
        webauth_croak(self, "webauth_keyring_read", status);
    ring->ctx = self;
    RETVAL = ring;
}
  OUTPUT:
    RETVAL


SV *
token_decode(self, input, ring)
    WebAuth self
    SV *input
    WebAuth::Keyring ring
  PROTOTYPE: $$$
  PREINIT:
    const char *encoded;
    struct webauth_token *token;
    int status;
    HV *hash;
    SV *object;
  CODE:
{
    encoded = SvPV_nolen(input);
    status = webauth_token_decode(self, WA_TOKEN_ANY, encoded, ring->ring,
                                  &token);
    if (status != WA_ERR_NONE)
        webauth_croak(self, "webauth_token_decode", status);
    hash = newHV();
    object = newRV_noinc((SV *) hash);
    switch (token->type) {
    case WA_TOKEN_APP:
        sv_bless(object, gv_stashpv("WebAuth::Token::App", GV_ADD));
        map_token_to_hash(token_mapping_app, &token->token.app, hash);
        break;
    case WA_TOKEN_CRED:
        sv_bless(object, gv_stashpv("WebAuth::Token::Cred", GV_ADD));
        map_token_to_hash(token_mapping_cred, &token->token.cred, hash);
        break;
    case WA_TOKEN_ERROR:
        sv_bless(object, gv_stashpv("WebAuth::Token::Error", GV_ADD));
        map_token_to_hash(token_mapping_error, &token->token.error, hash);
        break;
    case WA_TOKEN_ID:
        sv_bless(object, gv_stashpv("WebAuth::Token::Id", GV_ADD));
        map_token_to_hash(token_mapping_id, &token->token.id, hash);
        break;
    case WA_TOKEN_LOGIN:
        sv_bless(object, gv_stashpv("WebAuth::Token::Login", GV_ADD));
        map_token_to_hash(token_mapping_login, &token->token.login, hash);
        break;
    case WA_TOKEN_PROXY:
        sv_bless(object, gv_stashpv("WebAuth::Token::Proxy", GV_ADD));
        map_token_to_hash(token_mapping_proxy, &token->token.proxy, hash);
        break;
    case WA_TOKEN_REQUEST:
        sv_bless(object, gv_stashpv("WebAuth::Token::Request", GV_ADD));
        map_token_to_hash(token_mapping_request, &token->token.request, hash);
        break;
    case WA_TOKEN_WEBKDC_PROXY:
        sv_bless(object, gv_stashpv("WebAuth::Token::WebKDCProxy", GV_ADD));
        map_token_to_hash(token_mapping_webkdc_proxy,
                          &token->token.webkdc_proxy, hash);
        break;
    case WA_TOKEN_WEBKDC_SERVICE:
        sv_bless(object, gv_stashpv("WebAuth::Token::WebKDCService", GV_ADD));
        map_token_to_hash(token_mapping_webkdc_service,
                          &token->token.webkdc_service, hash);
        break;
    case WA_TOKEN_UNKNOWN:
    case WA_TOKEN_ANY:
    default:
        croak("unknown token type %d", token->type);
        break;
    }

    /*
     * Stash a reference to the context in the generated hash.  XS will have
     * automatically unwrapped a struct webauth_context from an SV for us in
     * the preamble, but we want to reuse the SV used by Perl, so store the
     * contents of our first argument directly.
     */
    if (hv_stores(hash, "ctx", ST(0)) == NULL)
        croak("cannot store context in hash");
    SvREFCNT_inc(ST(0));
    RETVAL = object;
}
  OUTPUT:
    RETVAL


WebAuth::Krb5
krb5_new(self)
    WebAuth self
  PROTOTYPE: $
  PREINIT:
    struct webauth_krb5 *kc = NULL;
    WebAuth__Krb5 krb5;
    int status;
    SV *output;
  CODE:
{
    krb5 = malloc(sizeof(WebAuth__Krb5));
    if (krb5 == NULL)
        croak("cannot allocate memory");
    status = webauth_krb5_new(self, &krb5->kc);
    if (status != WA_ERR_NONE)
        webauth_croak(self, "webauth_krb5_new", status);
    krb5->ctx = self;
    RETVAL = krb5;
}
  OUTPUT:
    RETVAL


MODULE = WebAuth  PACKAGE = WebAuth::Key

enum webauth_key_type
type(self)
    WebAuth::Key self
  PROTOTYPE: $
  CODE:
    RETVAL = self->type;
  OUTPUT:
    RETVAL

enum webauth_key_size
length(self)
    WebAuth::Key self
  PROTOTYPE: $
  CODE:
    RETVAL = self->length;
  OUTPUT:
    RETVAL

SV *
data(self)
    WebAuth::Key self
  PROTOTYPE: $
  CODE:
    RETVAL = newSVpvn((const void *) self->data, self->length);
  OUTPUT:
    RETVAL


MODULE = WebAuth  PACKAGE = WebAuth::Keyring

void
DESTROY(self)
    WebAuth::Keyring self
  PROTOTYPE: $
  CODE:
    free(self);


void
add(self, creation, valid_after, key)
    WebAuth::Keyring self
    time_t creation
    time_t valid_after
    WebAuth::Key key
  PROTOTYPE: $$$$
  PREINIT:
    int s;
  PPCODE:
{
    webauth_keyring_add(self->ctx, self->ring, creation, valid_after, key);
    XSRETURN_YES;
}


WebAuth::Key
best_key(self, usage, hint)
    WebAuth::Keyring self
    enum webauth_key_usage usage
    time_t hint
  PROTOTYPE: $$$
  PREINIT:
    const struct webauth_key *key;
    int s;
  CODE:
{
    s = webauth_keyring_best_key(self->ctx, self->ring, usage, hint, &key);
    if (s == WA_ERR_NONE)
        RETVAL = key;
    else if (s == WA_ERR_NOT_FOUND)
        XSRETURN_UNDEF;
    else
        webauth_croak(self->ctx, "webauth_keyring_best_key", s);
}
  OUTPUT:
    RETVAL


SV *
encode(self)
    WebAuth::Keyring self
  PROTOTYPE: $$
  PREINIT:
    int s;
    char *data;
    size_t length;
  CODE:
{
    s = webauth_keyring_encode(self->ctx, self->ring, &data, &length);
    if (s != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_keyring_encode", s);
    RETVAL = newSVpvn(data, length);
}
  OUTPUT:
    RETVAL


void
entries(self)
    WebAuth::Keyring self
  PROTOTYPE: $
  PREINIT:
    struct webauth_keyring *ring;
  PPCODE:
{
    ring = self->ring;
    if (GIMME_V == G_ARRAY) {
        struct webauth_keyring_entry *e;
        SV *entry;
        size_t i;

        for (i = 0; i < (size_t) ring->entries->nelts; i++) {
            e = &APR_ARRAY_IDX(ring->entries, i, struct webauth_keyring_entry);
            entry = sv_newmortal();
            sv_setref_pv(entry, "WebAuth::KeyringEntry", e);
            SvREADONLY_on(entry);
            XPUSHs(entry);
        }
    } else {
        ST(0) = newSViv(ring->entries->nelts);
        sv_2mortal(ST(0));
        XSRETURN(1);
    }
}


void
remove(self, n)
    WebAuth::Keyring self
    size_t n
  PROTOTYPE: $$
  PREINIT:
    int s;
  PPCODE:
{
    s = webauth_keyring_remove(self->ctx, self->ring, n);
    if (s != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_keyring_remove", s);
    XSRETURN_YES;
}


void
write(self, path)
    WebAuth::Keyring self
    char *path
  PROTOTYPE: $$
  PREINIT:
    int s;
  PPCODE:
{
    s = webauth_keyring_write(self->ctx, self->ring, path);
    if (s != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_keyring_write", s);
    XSRETURN_YES;
}


MODULE = WebAuth        PACKAGE = WebAuth::KeyringEntry

time_t
creation(self)
    WebAuth::KeyringEntry self
  PROTOTYPE: $
  CODE:
    RETVAL = self->creation;
  OUTPUT:
    RETVAL


time_t
valid_after(self)
    WebAuth::KeyringEntry self
  PROTOTYPE: $
  CODE:
    RETVAL = self->valid_after;
  OUTPUT:
    RETVAL


WebAuth::Key
key(self)
    WebAuth::KeyringEntry self
  PROTOTYPE: $
  CODE:
    RETVAL = self->key;
  OUTPUT:
    RETVAL


MODULE = WebAuth  PACKAGE = WebAuth::Krb5

void
DESTROY(self)
    WebAuth::Krb5 self
  PROTOTYPE: $
  CODE:
{
    webauth_krb5_free(self->ctx, self->kc);
    free(self);
}


void
init_via_cache(self, cache = NULL)
    WebAuth::Krb5 self
    const char *cache
  PROTOTYPE: $;$
  PREINIT:
    int status;
  CODE:
{
    status = webauth_krb5_init_via_cache(self->ctx, self->kc, cache);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_init_via_cache", status);
}


void
init_via_keytab(self, keytab, server = NULL, cache = NULL)
    WebAuth::Krb5 self
    const char *keytab
    const char *server
    const char *cache
  PROTOTYPE: $$;$$
  PREINIT:
    int status;
  CODE:
{
    if (server != NULL && server[0] == '\0')
       server = NULL;
    status = webauth_krb5_init_via_keytab(self->ctx, self->kc, keytab, server,
                                          cache);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_init_via_keytab", status);
}


const char *
init_via_password(self, username, password, principal = NULL, keytab = NULL, \
                  server = NULL, cache = NULL)
    WebAuth::Krb5 self
    const char *username
    const char *password
    const char *principal
    const char *keytab
    const char *server
    const char *cache
  PROTOTYPE: $$$;$$$$
  PREINIT:
    char *servername;
    int status;
  CODE:
{
    if (principal != NULL && principal[0] == '\0')
       principal = NULL;
    if (server != NULL && server[0] == '\0')
       server = NULL;
    status = webauth_krb5_init_via_password(self->ctx, self->kc, username,
                                            password, principal, keytab,
                                            server, cache, &servername);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_init_via_password", status);
    else if (principal == NULL && keytab != NULL)
        RETVAL = servername;
    else
        XSRETURN_UNDEF;
}
  OUTPUT:
    RETVAL


void
export_cred(self, principal = NULL)
    WebAuth::Krb5 self
    const char *principal
  PROTOTYPE: $;$
  PPCODE:
{
    void *cred;
    size_t cred_len;
    time_t expiration;
    SV *out;
    int status;

    status = webauth_krb5_export_cred(self->ctx, self->kc, principal, &cred,
                                      &cred_len, &expiration);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_export_cred", status);

    /*
     * Return just the exported cred in scalar context, and an array of the
     * cred and the expiration time in an array context.
     */
    out = sv_newmortal();
    sv_setpvn(out, cred, cred_len);
    if (GIMME_V == G_ARRAY) {
        EXTEND(SP, 2);
        PUSHs(out);
        PUSHs(sv_2mortal(newSViv(expiration)));
    } else {
        EXTEND(SP, 1);
        PUSHs(out);
    }
}


void
import_cred(self, cred, cache = NULL)
    WebAuth::Krb5 self
    SV *cred
    const char *cache
  PROTOTYPE: $$;$
  PREINIT:
    const void *data;
    size_t length;
    int status;
  CODE:
{
    data = SvPV(cred, length);
    status = webauth_krb5_import_cred(self->ctx, self->kc, data, length,
                                      cache);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_import_cred", status);
}


char *
get_principal(self, canon = 0)
    WebAuth::Krb5 self
    enum webauth_krb5_canon canon
  PROTOTYPE: $;$
  PREINIT:
    int status;
    char *principal;
  CODE:
{
    status = webauth_krb5_get_principal(self->ctx, self->kc, &principal,
                                        canon);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_get_principal", status);
    RETVAL = principal;
}
  OUTPUT:
    RETVAL


void
make_auth(self, server, data = NULL)
    WebAuth::Krb5 self
    const char *server
    SV *data
  PROTOTYPE: $$;$
  PPCODE:
{
    void *req, *out_data;
    SV *result, *out;
    size_t length, out_length;
    const void *in_data = NULL;
    size_t in_length = 0;
    int status;

    if (data != NULL)
        in_data = SvPV(data, in_length);
    status = webauth_krb5_make_auth_data(self->ctx, self->kc, server, &req,
                                         &length, in_data, in_length,
                                         &out_data, &out_length);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_make_auth_data", status);

    /*
     * Return just the authenticator in scalar context, and an array of the
     * authenticator and the encrypted data in array context, unless no data
     * was given.
     */
    result = sv_newmortal();
    sv_setpvn(result, req, length);
    if (data != NULL && GIMME_V == G_ARRAY) {
        EXTEND(SP, 2);
        PUSHs(result);
        out = sv_newmortal();
        sv_setpvn(out, out_data, out_length);
        PUSHs(out);
    } else {
        EXTEND(SP, 1);
        PUSHs(result);
    }
}


void
read_auth(self, request, keytab, server = NULL, canon = 0, data = NULL)
    WebAuth::Krb5 self
    SV *request
    const char *keytab
    const char *server
    enum webauth_krb5_canon canon
    SV *data
  PROTOTYPE: $$$;$$$
  PPCODE:
{
    const void *req;
    const void *in_data = NULL;
    void *out_data;
    size_t req_len, in_len, out_len;
    char *client;
    SV *out;
    int status;

    req = SvPV(request, req_len);
    if (data != NULL)
        in_data = SvPV(data, in_len);
    if (server != NULL && server[0] == '\0')
       server = NULL;
    status = webauth_krb5_read_auth_data(self->ctx, self->kc, req, req_len,
                                         keytab, server, NULL, &client, canon,
                                         in_data, in_len, &out_data, &out_len);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_read_auth_data", status);

    /*
     * Return just the client identity in scalar context, and an array of the
     * client identity and the decrypted data in array context, unless no data
     * was given.
     */
    if (data != NULL && GIMME_V == G_ARRAY)
        EXTEND(SP, 2);
    else
        EXTEND(SP, 1);
    PUSHs(sv_2mortal(newSVpv(client, 0)));
    if (data != NULL && GIMME_V == G_ARRAY) {
        out = sv_newmortal();
        sv_setpvn(out, out_data, out_len);
        PUSHs(out);
    }
}


void
change_password(self, password)
    WebAuth::Krb5 self
    const char *password
  PROTOTYPE: $$
  PREINIT:
    int status;
  CODE:
{
    status = webauth_krb5_change_password(self->ctx, self->kc, password);
    if (status != WA_ERR_NONE)
        webauth_croak(self->ctx, "webauth_krb5_change_password", status);
}


MODULE = WebAuth        PACKAGE = WebAuth::Token

const char *
encode(self, ring)
    SV *self
    WebAuth::Keyring ring
  PROTOTYPE: $$
  PREINIT:
    HV *hash;
    SV **ctx_sv;
    IV ctx_iv;
    struct webauth_context *ctx;
    struct webauth_token token;
    int status;
    const char *output;
  CODE:
{
    if (!sv_derived_from(self, "WebAuth::Token"))
        croak("self is not of type WebAuth::Token");
    hash = (HV *) SvRV(self);

    /*
     * Pull the context from the hash.  Our typemap wraps the context by
     * storing it as the IV of the scalar, so we undo that wrapping here.
     */
    ctx_sv = hv_fetch(hash, "ctx", strlen("ctx"), 0);
    if (ctx_sv == NULL)
        croak("no WebAuth context in WebAuth::Token object");
    ctx_iv = SvIV((SV *) SvRV(*ctx_sv));
    ctx = INT2PTR(struct webauth_context *, ctx_iv);

    /* Copy our hash contents to the appropriate struct. */
    memset(&token, 0, sizeof(token));
    if (sv_derived_from(self, "WebAuth::Token::App")) {
        token.type = WA_TOKEN_APP;
        map_hash_to_token(token_mapping_app, hash, &token.token.app);
    } else if (sv_derived_from(self, "WebAuth::Token::Cred")) {
        token.type = WA_TOKEN_CRED;
        map_hash_to_token(token_mapping_cred, hash, &token.token.cred);
    } else if (sv_derived_from(self, "WebAuth::Token::Error")) {
        token.type = WA_TOKEN_ERROR;
        map_hash_to_token(token_mapping_error, hash, &token.token.error);
    } else if (sv_derived_from(self, "WebAuth::Token::Id")) {
        token.type = WA_TOKEN_ID;
        map_hash_to_token(token_mapping_id, hash, &token.token.id);
    } else if (sv_derived_from(self, "WebAuth::Token::Login")) {
        token.type = WA_TOKEN_LOGIN;
        map_hash_to_token(token_mapping_login, hash, &token.token.login);
    } else if (sv_derived_from(self, "WebAuth::Token::Proxy")) {
        token.type = WA_TOKEN_PROXY;
        map_hash_to_token(token_mapping_proxy, hash, &token.token.proxy);
    } else if (sv_derived_from(self, "WebAuth::Token::Request")) {
        token.type = WA_TOKEN_REQUEST;
        map_hash_to_token(token_mapping_request, hash, &token.token.request);
    } else if (sv_derived_from(self, "WebAuth::Token::WebKDCProxy")) {
        token.type = WA_TOKEN_WEBKDC_PROXY;
        map_hash_to_token(token_mapping_webkdc_proxy, hash,
                          &token.token.webkdc_proxy);
    } else if (sv_derived_from(self, "WebAuth::Token::WebKDCService")) {
        token.type = WA_TOKEN_WEBKDC_SERVICE;
        map_hash_to_token(token_mapping_webkdc_service, hash,
                          &token.token.webkdc_service);
    } else {
        croak("self is not a supported WebAuth::Token::* object");
    }

    /* Do the actual encoding. */
    status = webauth_token_encode(ctx, &token, ring->ring, &output);
    if (status != WA_ERR_NONE)
        webauth_croak(ctx, "webauth_token_encode", status);
    RETVAL = output;
}
  OUTPUT:
    RETVAL
