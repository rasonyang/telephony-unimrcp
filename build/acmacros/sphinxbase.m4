dnl UNIMRCP_CHECK_SPHINXBASE

AC_DEFUN([UNIMRCP_CHECK_SPHINXBASE],
[  
    AC_MSG_NOTICE([SphinxBase library configuration])

    AC_MSG_CHECKING([for SphinxBase])
    AC_ARG_WITH(sphinxbase,
                [  --with-sphinxbase=PATH  prefix for installed SphinxBase or
                          path to SphinxBase build tree],
                [sphinxbase_path=$withval],
                [sphinxbase_path="/usr/local"]
                )

    found_sphinxbase="no"
    sphinxbase_config="lib/pkgconfig/sphinxbase.pc"
    sphinxbase_srcdir="src"
    for dir in $sphinxbase_path ; do
        cd $dir && sphinxbase_dir=`pwd` && cd - > /dev/null
        if test -f "$dir/$sphinxbase_config"; then
            if test -n "$PKG_CONFIG"; then
                found_sphinxbase="yes"
                UNIMRCP_SPHINXBASE_INCLUDES="`$PKG_CONFIG --cflags $dir/$sphinxbase_config`"
                UNIMRCP_SPHINXBASE_LIBS="`$PKG_CONFIG --libs $dir/$sphinxbase_config`"
                sphinxbase_version="`$PKG_CONFIG --modversion $dir/$sphinxbase_config`"
                break
            else
                AC_MSG_ERROR(pkg-config is not available)
            fi
        fi
        if test -d "$dir/$sphinxbase_srcdir"; then
            found_sphinxbase="yes"
            UNIMRCP_SPHINXBASE_INCLUDES="-I$sphinxbase_dir/include"
            UNIMRCP_SPHINXBASE_LIBS="$sphinxbase_dir/$sphinxbase_srcdir/libsphinxbase/libsphinxbase.la $sphinxbase_dir/$sphinxbase_srcdir/libsphinxad/libsphinxad.la"
            if test -n "$PKG_CONFIG"; then
                sphinxbase_version="`$PKG_CONFIG --modversion $sphinxbase_dir/sphinxbase.pc`"
            fi
            break
        fi
    done

    if test x_$found_sphinxbase != x_yes; then
        AC_MSG_ERROR(Cannot find SphinxBase - looked for sphinxbase-config:$sphinxbase_config and srcdir:$sphinxbase_srcdir in $sphinxbase_path)
    else
        AC_MSG_RESULT([$found_sphinxbase])
        AC_MSG_RESULT([$sphinxbase_version])

case "$host" in
    *darwin*)
        UNIMRCP_SPHINXBASE_LIBS="$UNIMRCP_SPHINXBASE_LIBS -framework CoreFoundation -framework SystemConfiguration"
        ;;
esac

        AC_SUBST(UNIMRCP_SPHINXBASE_INCLUDES)
        AC_SUBST(UNIMRCP_SPHINXBASE_LIBS)
    fi
])
