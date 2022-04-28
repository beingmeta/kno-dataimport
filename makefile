PKGNAME		  = readstat
LIBNAME		  = creadstat
KNOCONFIG         = knoconfig
KNOBUILD          = knobuild

prefix		::= $(shell ${KNOCONFIG} prefix)
libsuffix	::= $(shell ${KNOCONFIG} libsuffix)
KNO_CFLAGS	::= -I. -fPIC $(shell ${KNOCONFIG} cflags)
KNO_LDFLAGS	::= -fPIC $(shell ${KNOCONFIG} ldflags)
KNO_LIBS	::= $(shell ${KNOCONFIG} libs)
CMODULES	::= $(DESTDIR)$(shell ${KNOCONFIG} cmodules)
INSTALLMODS	::= $(DESTDIR)$(shell ${KNOCONFIG} installmods)
LIBS		::= $(shell ${KNOCONFIG} libs)
LIB		::= $(shell ${KNOCONFIG} lib)
INCLUDE		::= $(shell ${KNOCONFIG} include)
KNO_VERSION	::= $(shell ${KNOCONFIG} version)
KNO_MAJOR	::= $(shell ${KNOCONFIG} major)
KNO_MINOR	::= $(shell ${KNOCONFIG} minor)
PKG_VERSION     ::= $(shell u8_gitversion ./etc/knomod_version)
PKG_MAJOR       ::= $(shell cat ./etc/knomod_version | cut -d. -f1)
FULL_VERSION    ::= ${KNO_MAJOR}.${KNO_MINOR}.${PKG_VERSION}
PATCHLEVEL      ::= $(shell u8_gitpatchcount ./etc/knomod_version)
PATCH_VERSION   ::= ${FULL_VERSION}-${PATCHLEVEL}

INSTALLS	::= "$(shell pwd)/installs"

SUDO            ::= $(shell which sudo)
INIT_CFLAGS     ::= ${CFLAGS} -Iinstalls/include
INIT_LDFAGS     ::= ${LDFLAGS} -Linstalls/lib
XCFLAGS	  	  = ${INIT_CFLAGS} ${READSTAT_CFLAGS} ${KNO_CFLAGS} ${BSON_CFLAGS} ${READSTAT_CFLAGS}
XLDFLAGS	  = ${INIT_LDFLAGS} ${KNO_LDFLAGS} ${BSON_LDFLAGS} ${READSTAT_LDFLAGS}

MKSO		  = $(CC) -shared $(LDFLAGS) $(LIBS)
SYSINSTALL        = /usr/bin/install -c
DIRINSTALL        = /usr/bin/install -d
MODINSTALL        = /usr/bin/install -m 0664
USEDIR        	  = /usr/bin/install -d
MSG		  = echo
MACLIBTOOL	  = $(CC) -dynamiclib -single_module -undefined dynamic_lookup \
			$(LDFLAGS)

GPGID           ::= ${OVERRIDE_GPGID:-FE1BC737F9F323D732AA26330620266BE5AFF294}
CODENAME	::= $(shell ${KNOCONFIG} codename)
REL_BRANCH	::= $(shell ${KNOBUILD} getbuildopt REL_BRANCH current)
REL_STATUS	::= $(shell ${KNOBUILD} getbuildopt REL_STATUS stable)
REL_PRIORITY	::= $(shell ${KNOBUILD} getbuildopt REL_PRIORITY medium)
ARCH            ::= $(shell ${KNOBUILD} getbuildopt BUILD_ARCH || uname -m)
APKREPO         ::= $(shell ${KNOBUILD} getbuildopt APKREPO /srv/repo/kno/apk)
APK_ARCH_DIR      = ${APKREPO}/staging/${ARCH}
RPMDIR		  = dist

STATICLIBS=installs/lib/libcsv.a installs/lib/libreadstat.a

default:
	@make ${STATICLIBS}
	@make creadstat.${libsuffix}

creadstat.o: creadstat.c makefile ${STATICLIBS}
	@echo XCFLAGS=${XCFLAGS}
	@$(CC) --save-temps $(XCFLAGS) -D_FILEINFO="\"$(shell u8_fileinfo ./$< $(dirname $(pwd))/)\"" -o $@ -c $<
	@$(MSG) CC "(CREADSTAT)" $@
creadstat.so: creadstat.o makefile
	@$(MKSO) -o $@ creadstat.o -Wl,-soname=$(@F).${FULL_VERSION} \
	          -Wl,--allow-multiple-definition \
	          -Wl,--whole-archive ${STATICLIBS} -Wl,--no-whole-archive \
		 $(XLDFLAGS)
	@$(MSG) MKSO "(CREADSTAT)" $@

creadstat.dylib: creadstat.o makefile ${STATICLIBS}
	@$(MACLIBTOOL) -install_name \
		`basename $(@F) .dylib`.${KNO_MAJOR}.dylib \
		$(DYLIB_FLAGS) $(BSON_LDFLAGS) $(READSTAT_LDFLAGS) \
		-o $@ creadstat.o 
	@$(MSG) MACLIBTOOL "(CREADSTAT)" $@

# Components

installs/lib/libcsv.a: libreadstat/.git
	cd libcsv; aclocal && automake && configure --prefix=${INSTALLS} && make && make install

installs/lib/libreadstat.a: libreadstat/.git
	cd libreadstat; ./autogen.sh && configure --prefix=${INSTALLS} && make && make install

libcsv/.git libreadstat/.git:
	git submodule update --init

staticlibs: ${STATICLIBS}
readstat.dylib readstat.so: staticlibs

scheme/readstat.zip: scheme/readstat/*.scm
	cd scheme; zip readstat.zip readstat -x "*~" -x "#*" -x "*.attic/*" -x ".git*"

install: install-cmodule install-scheme
suinstall doinstall:
	sudo make install

${CMODULES}:
	@${DIRINSTALL} ${CMODULES}

install-cmodule: ${CMODULES}
	${SUDO} u8_install_shared ${LIBNAME}.${libsuffix} ${CMODULES} ${FULL_VERSION} "${SYSINSTALL}"

${INSTALLMODS}/readstat:
	${SUDO} ${DIRINSTALL} $@

install-scheme-zip: ${INSTALLMODS}/readstat.zip
	${SUDO} ${MODINSTALL} scheme/readstat/*.scm ${INSTALLMODS}/readstat

install-scheme: ${INSTALLMODS}/readstat
	${SUDO} ${MODINSTALL} scheme/readstat/*.scm ${INSTALLMODS}/readstat

clean:
	rm -f *.o *.${libsuffix} *.${libsuffix}*
deep-clean: clean
	if test -f mongo-c-driver/Makefile; then cd mongo-c-driver; make clean; fi;
	rm -rf mongoc-build install
fresh: clean
	make
deep-fresh: deep-clean
	make

gitup gitup-trunk:
	git checkout trunk && git pull

TAGS: creadstat.c scheme/readstat/*.scm
	etags -o $@ $^


