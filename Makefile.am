noinst_LTLIBRARIES =

include Makefile.tests
include data/init/Makefile.include
include src/mongo-glib/Makefile.include
include src/neo/Makefile.include
include src/postal/Makefile.include
include src/push-glib/Makefile.include
include src/redis-glib/Makefile.include
include src/tools/Makefile.include
include tests/Makefile.include

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}
DISTCHECK_CONFIGURE_FLAGS = --enable-silent-rules --enable-debug=yes

postaldocdir = ${datadir}/doc/postal
postaldoc_DATA =
postaldoc_DATA += AUTHORS
postaldoc_DATA += COPYING
postaldoc_DATA += NEWS
postaldoc_DATA += README
postaldoc_DATA += README.md

sysconf_DATA =
sysconf_DATA += data/conf/postald.conf

EXTRA_DIST += $(postaldoc_DATA)
EXTRA_DIST += $(sysconf_DATA)

dist-hook:
	@if test -d "$(srcdir)/.git"; then                              \
          (cd "$(srcdir)" &&                                            \
           $(top_srcdir)/missing --run git log --stat ) > ChangeLog.tmp \
           && mv -f ChangeLog.tmp $(top_distdir)/ChangeLog              \
           || (rm -f ChangeLog.tmp;                                     \
               echo Failed to generate ChangeLog >&2);                  \
	else                                                            \
	  echo A git checkout is required to generate a ChangeLog >&2;  \
	fi

uninstall-local:
	-rm -r $(postaldocdir)
