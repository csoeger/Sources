GIT_VERSION := $(shell $(top_srcdir)/git-version-gen $(top_srcdir)/.tarball-git-version)

doxyrun:
	if [ -z "$(DOXYGEN)" ]; then \
		which -s doxygen || (echo Please add 'doxygen' to your PATH; exit 1;); \
		export DOXYGEN=`which -s doxygen`; \
	fi
	if [ -z "$(DOXYGEN_CONFIG)" ]; then \
		echo Please set 'DOXYGEN_CONFIG' env. var!; \
		exit 1; \
	fi
	if [ ! -r "$(DOXYGEN_CONFIG)" ]; then \
		echo Please make the doxygen configuration readable: "$(DOXYGEN_CONFIG)"!; \
		exit 1; \
	fi
	export abs_top_srcdir="$(abs_top_srcdir)"; \
	export abs_top_builddir="$(abs_top_builddir)"; \
	export git_version="$(GIT_VERSION)"; \
 	echo Running "$(DOXYGEN)" "$(DOXYGEN_CONFIG)" with abs_top_srcdir: "$(abs_top_srcdir)", abs_top_builddir: "$(abs_top_builddir)", GIT_VERSION: "$(git_version)"...; \
	$(DOXYGEN) $(DOXYGEN_CONFIG) ; \
 	echo Finished running "$(DOXYGEN)" "$(DOXYGEN_CONFIG)" with abs_top_srcdir: "$(abs_top_srcdir)", abs_top_builddir: "$(abs_top_builddir)", GIT_VERSION: "$(git_version)"...;

if HAVE_DOXYGEN

## doxyfile.stamp:
html:
	$(MAKE) doxyrun abs_top_srcdir="$(abs_top_srcdir)" abs_top_builddir="$(abs_top_builddir)" \
	DOXYGEN_CONFIG="$(abs_srcdir)/Doxyfile.in" DOXYGEN="$(DOXYGEN)"
##	echo Timestamp > doxyfile.stamp

## CLEANFILES = doxyfile.stamp

# all-local: doxyfile.stamp

## html: doxyfile.stamp

clean-local:
	rm -rf html
endif

EXTRA_DIST = Doxyfile.in logo.png DoxygenLayout.xml header.html footer.html stylesheet.css
