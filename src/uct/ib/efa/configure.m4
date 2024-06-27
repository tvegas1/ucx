#
# Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
#
# See file LICENSE for terms.
#

#
# EFA provider Support
#
AC_ARG_WITH([efa],
            [AC_HELP_STRING([--with-efa=(DIR)], [Enable EFA device support (default is guess)])],
            [], [with_efa=guess])

have_efa=no
have_efa_rdma_read=no

AS_IF([test "x$with_ib" = "xno"], [with_efa=no])

AS_IF([test "x$with_efa" != xno],
      [AS_IF([test "x$with_efa" = xguess -o "x$with_efa" = xyes -o "x$with_efa" = x],
             [efa_dir=/usr],
             [efa_dir="$with_efa"])

       AS_IF([test -d "$efa_dir/lib64"], [suff="64"], [suff=""])

       save_LDFLAGS="$LDFLAGS"
       save_CPPFLAGS="$CPPFLAGS"
       save_LIBS="$LIBS"

       LDFLAGS="-L$efa_dir/lib$suff $LDFLAGS"
       CPPFLAGS="-I$efa_dir/include $CPPFLAGS"

       have_efa=yes
       AC_CHECK_HEADER([infiniband/efadv.h], [], [have_efa=no])
       AC_CHECK_LIB([efa], [efadv_query_device], [], [have_efa=no])
       AC_CHECK_DECL([EFADV_DEVICE_ATTR_CAPS_RDMA_READ], [have_efa_rdma_read=yes], [],
                      [#include <infiniband/efadv.h>])

       AS_IF([test "x$have_efa" = xyes],
             [AS_IF([test "x$efa_dir" != x/usr],
                    [AC_SUBST(EFA_CFLAGS, ["-I$efa_dir/include"])
                     AC_SUBST(EFA_CPPFLAGS, ["-I$efa_dir/include"])
                     AC_SUBST(EFA_LDFLAGS, ["-L$efa_dir/lib$suff"])])
              AC_SUBST(EFA_LIBS, [-lefa])
              AC_DEFINE([HAVE_EFA], 1, [EFA DV support])
              uct_ib_modules="${uct_ib_modules}:efa"
             ],
             [AS_IF([test "x$with_efa" != xguess],
                    [AC_MSG_ERROR(EFA provider support request but libefa or headers are not found)])])

       LDFLAGS="$save_LDFLAGS"
       CPPFLAGS="$save_CPPFLAGS"
       LIBS="$save_LIBS"
      ])


#
# For automake
#
AM_CONDITIONAL([HAVE_EFA],  [test "x$have_efa" = xyes])
AM_CONDITIONAL([HAVE_EFA_RDMA_READ],  [test "x$have_efa_rdma_read" = xyes])

AC_CONFIG_FILES([src/uct/ib/efa/Makefile
                 src/uct/ib/efa/ucx-ib-efa.pc])
