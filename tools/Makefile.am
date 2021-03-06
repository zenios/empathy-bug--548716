include $(top_srcdir)/tools/shave.mk

abs_top_builddir = @abs_top_builddir@

noinst_SCRIPTS = telepathy-glib-env

telepathy-glib-env: telepathy-glib-env.in Makefile
	sed -e 's![@]abs_top_builddir[@]!$(abs_top_builddir)!' $< > $@
	chmod +x $@

EXTRA_DIST = \
    c-constants-gen.py \
    check-coding-style.mk \
    check-c-style.sh \
    check-misc.sh \
    check-whitespace.sh \
    doc-generator.xsl \
    glib-client-gen.py \
    glib-client-marshaller-gen.py \
    glib-errors-enum-body-gen.py \
    glib-errors-enum-header-gen.py \
    glib-ginterface-gen.py \
    glib-gtypes-generator.py \
    glib-interfaces-gen.py \
    glib-signals-marshal-gen.py \
    gobject-foo.py \
    identity.xsl \
    lcov.am \
    libtpcodegen.py \
    libglibcodegen.py \
    make-version-script.py \
    telepathy.am \
    telepathy-glib-env.in \
    with-session-bus.sh

CLEANFILES = libtpcodegen.pyc libtpcodegen.pyo libglibcodegen.pyc libglibcodegen.pyo $(noinst_SCRIPTS)

all: $(EXTRA_DIST)

libglibcodegen.py: libtpcodegen.py
	$(QUIET_GEN)touch $@
c-constants-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-client-marshaller-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-errors-enum-body-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-errors-enum-header-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-ginterface-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-gtypes-generator.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-interfaces-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@
glib-signals-marshal-gen.py: libglibcodegen.py
	$(QUIET_GEN)touch $@

TELEPATHY_SPEC_SRCDIR = $(top_srcdir)/../telepathy-spec
maintainer-update-from-telepathy-spec:
	set -e && cd $(srcdir) && \
	for x in $(EXTRA_DIST); do \
		if test -f $(TELEPATHY_SPEC_SRCDIR)/tools/$$x; then \
			cp $(TELEPATHY_SPEC_SRCDIR)/tools/$$x $$x; \
		fi; \
	done
