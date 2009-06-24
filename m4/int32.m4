dnl int32.m4 -- Find an appropriate int32_t and uint32_t.
dnl
dnl The public macro exposed by this file is WEBAUTH_TYPE_INT32_T.  This macro
dnl locates the appropriate header files to include for int32_t and uint32_t
dnl or determines how to define those types, and then both includes the
dnl appropriate defines into the generated config.h.  It also defines
dnl HAVE_INT32_T and HAVE_UINT32_T as appropriate.

dnl if some other type has to be used to supply int32_t and uint32_t and
dnl defines the output variables WEBAUTH

dnl Used to build the type cache name.
AC_DEFUN([_WEBAUTH_TYPE_CACHE], translit([ac_cv_sizeof_$1], [ *], [_p]))

dnl A modified version of AC_CHECK_SIZEOF that doesn't always AC_DEFINE, but
dnl instead lets you execute shell code based on success or failure.  This is
dnl to avoid config.h clutter.
AC_DEFUN([_WEBAUTH_IF_SIZEOF],
[AC_MSG_CHECKING([size of $1])
AC_CACHE_VAL(_WEBAUTH_TYPE_CACHE([$1]),
[AC_TRY_RUN([#include <stdio.h>
main()
{
    FILE *f = fopen("conftestval", "w");
    if (!f) exit(1);
    fprintf(f, "%d\n", sizeof($1));
    exit(0);
}], _WEBAUTH_TYPE_CACHE([$1])=`cat conftestval`, _WEBAUTH_TYPE_CACHE([$1])=0,
ifelse([$2], , , _WEBAUTH_TYPE_CACHE([$1])=$2))
])dnl
AC_MSG_RESULT($_WEBAUTH_TYPE_CACHE([$1]))
if test x"$_WEBAUTH_TYPE_CACHE([$1])" = x"$3" ; then
    ifelse([$4], , :, [$4])
else
    ifelse([$5], , :, [$5])
fi
])

dnl This is the beginning of the macro called by the user.
AC_DEFUN([WEBAUTH_TYPE_INT32_T],
[

dnl Find a 32 bit type, by trying likely candidates.  First, check for the
dnl C9X int32_t, then look for something else with a size of four bytes.
_WEBAUTH_IF_SIZEOF(int, 4, 4, WEBAUTH_INT32=int,
    [_WEBAUTH_IF_SIZEOF(long, 4, 4, WEBAUTH_INT32=long,
        [_WEBAUTH_IF_SIZEOF(short, 2, 4, WEBAUTH_INT32=short)])])

dnl Now, check to see if we need to define int32_t and uint32_t ourselves.
dnl This has to be done after the probes for an appropriately sized integer
dnl type so that we can pass that type to AC_DEFINE_UNQUOTED.
AC_CHECK_TYPES(int32_t, ,
    [AC_DEFINE_UNQUOTED([int32_t], [$WEBAUTH_INT32],
        [Define to a 4-byte signed type if <inttypes.h> does not define.])])
AC_CHECK_TYPES(uint32_t, ,
    [AC_DEFINE_UNQUOTED([uint32_t], [unsigned $WEBAUTH_INT32],
        [Define to a 4-byte unsigned type if <inttypes.h> does not define.])])
])
