/*
 * Utility functions for tests that use Kerberos.
 *
 * Currently only provides kerberos_setup(), which assumes a particular set of
 * data files in either the SOURCE or BUILD directories and, using those,
 * obtains Kerberos credentials, sets up a ticket cache, and sets the
 * environment variable pointing to the Kerberos keytab to use for testing.
 *
 * The canonical version of this file is maintained in the rra-c-util package,
 * which can be found at <http://www.eyrie.org/~eagle/software/rra-c-util/>.
 *
 * Written by Russ Allbery <rra@stanford.edu>
 * Copyright 2006, 2007, 2009, 2010, 2011
 *     The Board of Trustees of the Leland Stanford Junior University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <config.h>
#include <portable/krb5.h>
#include <portable/system.h>

#include <sys/stat.h>

#include <tests/tap/basic.h>
#include <tests/tap/kerberos.h>
#include <tests/tap/process.h>
#include <tests/tap/string.h>


/*
 * These variables hold the allocated strings for the principal, the
 * environment to point to a different Kerberos ticket cache, keytab, and
 * configuration file, and the temporary directories used.  We store them so
 * that we can free them on exit for cleaner valgrind output, making it easier
 * to find real memory leaks in the tested programs.
 */
static char *principal = NULL;
static char *krb5ccname = NULL;
static char *krb5_ktname = NULL;
static char *krb5_config = NULL;
static char *tmpdir_ticket = NULL;
static char *tmpdir_conf = NULL;


/*
 * Report a Kerberos error and bail out.
 */
void
bail_krb5(krb5_context ctx, krb5_error_code code, const char *format, ...)
{
    const char *k5_msg = NULL;
    char *message;
    va_list args;

    if (ctx != NULL)
        k5_msg = krb5_get_error_message(ctx, code);
    va_start(args, format);
    bvasprintf(&message, format, args);
    va_end(args);
    if (k5_msg == NULL)
        bail("%s", message);
    else
        bail("%s: %s", message, k5_msg);
}


/*
 * Report a Kerberos error as a diagnostic to stderr.
 */
void
diag_krb5(krb5_context ctx, krb5_error_code code, const char *format, ...)
{
    const char *k5_msg = NULL;
    char *message;
    va_list args;

    if (ctx != NULL)
        k5_msg = krb5_get_error_message(ctx, code);
    va_start(args, format);
    bvasprintf(&message, format, args);
    va_end(args);
    if (k5_msg == NULL)
        diag("%s", message);
    else
        diag("%s: %s", message, k5_msg);
    free(message);
    if (k5_msg != NULL)
        krb5_free_error_message(ctx, k5_msg);
}


/*
 * Clean up at the end of a test.  This removes the ticket cache and resets
 * and frees the memory allocated for the environment variables so that
 * valgrind output on test suites is cleaner.
 */
void
kerberos_cleanup(void)
{
    char *path;

    if (tmpdir_ticket != NULL) {
        basprintf(&path, "%s/krb5cc_test", tmpdir_ticket);
        unlink(path);
        free(path);
        test_tmpdir_free(tmpdir_ticket);
        tmpdir_ticket = NULL;
    }
    if (principal != NULL) {
        free(principal);
        principal = NULL;
    }
    putenv((char *) "KRB5CCNAME=");
    putenv((char *) "KRB5_KTNAME=");
    if (krb5ccname != NULL) {
        free(krb5ccname);
        krb5ccname = NULL;
    }
    if (krb5_ktname != NULL) {
        free(krb5_ktname);
        krb5_ktname = NULL;
    }
}


/*
 * Obtain Kerberos tickets for the principal specified in config/principal
 * using the keytab specified in config/keytab, both of which are presumed to
 * be in tests in either the build or the source tree.  Also sets KRB5_KTNAME
 * and KRB5CCNAME.
 *
 * Returns the contents of config/principal in newly allocated memory or NULL
 * if Kerberos tests are apparently not configured.  If Kerberos tests are
 * configured but something else fails, calls bail.
 */
const char *
kerberos_setup(void)
{
    char *path, *name, *krbtgt;
    const char *realm;
    krb5_error_code code;
    krb5_context ctx;
    krb5_ccache ccache;
    krb5_principal kprinc;
    krb5_keytab keytab;
    krb5_get_init_creds_opt *opts;
    krb5_creds creds;

    /* If we were called before, clean up after the previous run. */
    if (principal != NULL)
        kerberos_cleanup();

    /* Find the keytab file. */
    path = test_file_path("config/keytab");
    if (path == NULL)
        return NULL;

    /*
     * Determine the principal corresponding to that keytab.  We copy the
     * memory to ensure that it's allocated in the right memory domain on
     * systems where that may matter (like Windows).
     */
    code = krb5_init_context(&ctx);
    if (code != 0)
        bail_krb5(ctx, code, "error initializing Kerberos");
    kprinc = kerberos_keytab_principal(ctx, path);
    code = krb5_unparse_name(ctx, kprinc, &name);
    if (code != 0)
        bail_krb5(ctx, code, "error unparsing name");
    krb5_free_principal(ctx, kprinc);
    principal = bstrdup(name);
    krb5_free_unparsed_name(ctx, name);

    /* Set the KRB5CCNAME and KRB5_KTNAME environment variables. */
    tmpdir_ticket = test_tmpdir();
    basprintf(&krb5ccname, "KRB5CCNAME=%s/krb5cc_test", tmpdir_ticket);
    basprintf(&krb5_ktname, "KRB5_KTNAME=%s", path);
    putenv(krb5ccname);
    putenv(krb5_ktname);

    /* Now do the Kerberos initialization. */
    code = krb5_cc_default(ctx, &ccache);
    if (code != 0)
        bail_krb5(ctx, code, "error setting ticket cache");
    code = krb5_parse_name(ctx, principal, &kprinc);
    if (code != 0)
        bail_krb5(ctx, code, "error parsing principal %s", principal);
    realm = krb5_principal_get_realm(ctx, kprinc);
    basprintf(&krbtgt, "krbtgt/%s@%s", realm, realm);
    code = krb5_kt_resolve(ctx, path, &keytab);
    if (code != 0)
        bail_krb5(ctx, code, "cannot open keytab %s", path);
    code = krb5_get_init_creds_opt_alloc(ctx, &opts);
    if (code != 0)
        bail_krb5(ctx, code, "cannot allocate credential options");
    krb5_get_init_creds_opt_set_default_flags(ctx, NULL, realm, opts);
    krb5_get_init_creds_opt_set_forwardable(opts, 0);
    krb5_get_init_creds_opt_set_proxiable(opts, 0);
    code = krb5_get_init_creds_keytab(ctx, &creds, kprinc, keytab, 0, krbtgt,
                                      opts);
    if (code != 0)
        bail_krb5(ctx, code, "cannot get Kerberos tickets");
    code = krb5_cc_initialize(ctx, ccache, kprinc);
    if (code != 0)
        bail_krb5(ctx, code, "error initializing ticket cache");
    code = krb5_cc_store_cred(ctx, ccache, &creds);
    if (code != 0)
        bail_krb5(ctx, code, "error storing credentials");
    krb5_cc_close(ctx, ccache);
    krb5_free_cred_contents(ctx, &creds);
    krb5_kt_close(ctx, keytab);
    krb5_free_principal(ctx, kprinc);
    krb5_get_init_creds_opt_free(ctx, opts);
    krb5_free_context(ctx);
    free(krbtgt);
    test_file_path_free(path);

    /*
     * Register the cleanup function as an atexit handler so that the caller
     * doesn't have to worry about cleanup.
     */
    if (atexit(kerberos_cleanup) != 0)
        sysdiag("cannot register cleanup function");

    /* Return the principal. */
    return principal;
}


/*
 * Read a principal and password from config/password in the test suite
 * configuration and return it as a newly allocated kerberos_password struct.
 * Returns NULL if no configuration is present, and calls bail if there are
 * errors reading the configuration.  Free the result with
 * kerberos_config_password_free.
 */
struct kerberos_password *
kerberos_config_password(void)
{
    char *path;
    char buffer[BUFSIZ];
    struct kerberos_password *config;
    FILE *file;

    config = bmalloc(sizeof(struct kerberos_password));
    path = test_file_path("config/password");
    if (path == NULL)
        return NULL;
    file = fopen(path, "r");
    if (file == NULL)
        sysbail("cannot open %s", path);
    if (fgets(buffer, sizeof(buffer), file) == NULL)
        bail("cannot read %s", path);
    if (buffer[strlen(buffer) - 1] != '\n')
        bail("no newline in %s", path);
    buffer[strlen(buffer) - 1] = '\0';
    config->principal = bstrdup(buffer);
    if (fgets(buffer, sizeof(buffer), file) == NULL)
        bail("cannot read password from %s", path);
    fclose(file);
    if (buffer[strlen(buffer) - 1] != '\n')
        bail("password too long in %s", path);
    buffer[strlen(buffer) - 1] = '\0';
    config->password = bstrdup(buffer);
    test_file_path_free(path);

    /*
     * Strip the realm from the principal and set realm and username.  This
     * is not strictly correct; it doesn't cope with escaped @-signs.  But
     * it's rather unlikely someone would use such a thing as a test
     * principal.
     */
    config->username = bstrdup(config->principal);
    config->realm = strchr(config->username, '@');
    if (config->realm == NULL)
        bail("test principal has no realm");
    *config->realm = '\0';
    config->realm++;
    return config;
}


/*
 * Free a kerberos_password struct.  This knows about the allocation strategy
 * used by kerberos_config_password and frees accordingly.
 */
void
kerberos_config_password_free(struct kerberos_password *config)
{
    free(config->principal);
    free(config->username);
    free(config->password);
    free(config);
}


/*
 * Find the principal of the first entry of a keytab and return it.  The
 * caller is responsible for freeing the result with krb5_free_principal.
 * Exit on error.
 */
krb5_principal
kerberos_keytab_principal(krb5_context ctx, const char *path)
{
    krb5_keytab keytab;
    krb5_kt_cursor cursor;
    krb5_keytab_entry entry;
    krb5_principal princ;
    krb5_error_code status;

    status = krb5_kt_resolve(ctx, path, &keytab);
    if (status != 0)
        bail_krb5(ctx, status, "error opening %s", path);
    status = krb5_kt_start_seq_get(ctx, keytab, &cursor);
    if (status != 0)
        bail_krb5(ctx, status, "error reading %s", path);
    status = krb5_kt_next_entry(ctx, keytab, &entry, &cursor);
    if (status == 0) {
        status = krb5_copy_principal(ctx, entry.principal, &princ);
        if (status != 0)
            bail_krb5(ctx, status, "error copying principal from %s", path);
        krb5_kt_free_entry(ctx, &entry);
    }
    if (status != 0)
        bail("no principal found in keytab file %s", path);
    krb5_kt_end_seq_get(ctx, keytab, &cursor);
    krb5_kt_close(ctx, keytab);
    return princ;
}


/*
 * Clean up the krb5.conf file generated by kerberos_generate_conf and free
 * the memory used to set the environment variable.  This doesn't fail if the
 * file and variable are already gone, allowing it to be harmlessly run
 * multiple times.
 *
 * Normally called via an atexit handler.
 */
void
kerberos_cleanup_conf(void)
{
    char *path;

    if (tmpdir_conf != NULL) {
        basprintf(&path, "%s/krb5.conf", tmpdir_conf);
        unlink(path);
        free(path);
        test_tmpdir_free(tmpdir_conf);
        tmpdir_conf = NULL;
    }
    putenv((char *) "KRB5_CONFIG=");
    if (krb5_config != NULL) {
        free(krb5_config);
        krb5_config = NULL;
    }
}


/*
 * Generate a krb5.conf file for testing and set KRB5_CONFIG to point to it.
 * The [appdefaults] section will be stripped out and the default realm will
 * be set to the realm specified, if not NULL.  This will use config/krb5.conf
 * in preference, so users can configure the tests by creating that file if
 * the system file isn't suitable.
 *
 * Depends on data/generate-krb5-conf being present in the test suite.
 */
void
kerberos_generate_conf(const char *realm)
{
    char *path;
    const char *argv[3];

    path = test_file_path("data/generate-krb5-conf");
    if (path == NULL)
        bail("cannot find generate-krb5-conf");
    argv[0] = path;
    argv[1] = realm;
    argv[2] = NULL;
    run_setup(argv);
    test_file_path_free(path);
    tmpdir_conf = test_tmpdir();
    basprintf(&krb5_config, "KRB5_CONFIG=%s/krb5.conf", tmpdir_conf);
    putenv(krb5_config);
    if (atexit(kerberos_cleanup_conf) != 0)
        sysdiag("cannot register cleanup function");
}
