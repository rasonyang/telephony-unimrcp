dnl UNIMRCP_CHECK_POCKETSPHINX

AC_DEFUN([UNIMRCP_CHECK_POCKETSPHINX],
[  
    AC_MSG_NOTICE([PocketSphinx library configuration])

    AC_MSG_CHECKING([for PocketSphinx])
    AC_ARG_WITH(pocketsphinx,
                [  --with-pocketsphinx=PATH  prefix for installed PocketSphinx or
                          path to PocketSphinx build tree],
                [pocketsphinx_path=$withval],
                [pocketsphinx_path="/usr/local"]
                )

    found_pocketsphinx="no"
    pocketsphinx_config="lib/pkgconfig/pocketsphinx.pc"
    pocketsphinx_srcdir="src"
    for dir in $pocketsphinx_path ; do
        cd $dir && pocketsphinx_dir=`pwd` && cd - > /dev/null
        if test -f "$dir/$pocketsphinx_config"; then
            if test -n "$PKG_CONFIG"; then
                found_pocketsphinx="yes"
                UNIMRCP_POCKETSPHINX_INCLUDES="`$PKG_CONFIG --cflags $dir/$pocketsphinx_config`"
                UNIMRCP_POCKETSPHINX_LIBS="`$PKG_CONFIG --libs $dir/$pocketsphinx_config`"
                UNIMRCP_POCKETSPHINX_MODELS=
                pocketsphinx_version="`$PKG_CONFIG --modversion $dir/$pocketsphinx_config`"
                break
            else
                AC_MSG_ERROR(pkg-config is not available)
            fi
        fi
        if test -d "$dir/$pocketsphinx_srcdir"; then
            found_pocketsphinx="yes"
            UNIMRCP_POCKETSPHINX_INCLUDES="-I$pocketsphinx_dir/include"
            UNIMRCP_POCKETSPHINX_LIBS="$pocketsphinx_dir/$pocketsphinx_srcdir/libpocketsphinx/libpocketsphinx.la"
            UNIMRCP_POCKETSPHINX_MODELS="$pocketsphinx_dir/model"
            if test -n "$PKG_CONFIG"; then
                pocketsphinx_version="`$PKG_CONFIG --modversion $pocketsphinx_dir/pocketsphinx.pc`"
            fi
            break
        fi
    done

    if test x_$found_pocketsphinx != x_yes; then
        AC_MSG_ERROR(Cannot find PocketSphinx - looked for pocketsphinx-config:$pocketsphinx_config and srcdir:$pocketsphinx_srcdir in $pocketsphinx_path)
    else
        AC_MSG_RESULT([$found_pocketsphinx])
        AC_MSG_RESULT([$pocketsphinx_version])

case "$host" in
    *darwin*)
        UNIMRCP_POCKETSPHINX_LIBS="$UNIMRCP_POCKETSPHINX_LIBS -framework CoreFoundation -framework SystemConfiguration"
        ;;
esac

        AC_SUBST(UNIMRCP_POCKETSPHINX_INCLUDES)
        AC_SUBST(UNIMRCP_POCKETSPHINX_LIBS)
        AC_SUBST(UNIMRCP_POCKETSPHINX_MODELS)
    fi
])
