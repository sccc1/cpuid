CFLAGS+=-g
CPPFLAGS?=
LDFLAGS?=
ifneq (,$(findstring arch=i386,$(CFLAGS)))
CISA=-m32
endif
CFL=$(CPPFLAGS) $(CFLAGS) $(CISA) -Wall -Wshadow -Wcast-align -Wredundant-decls -Wbad-function-cast -Wcast-qual -Wwrite-strings -Waggregate-return -Wstrict-prototypes -Wmissing-prototypes -D_FILE_OFFSET_BITS=64 -DVERSION=$(VERSION)

PACKAGE=cpuid
VERSION=20180519
RELEASE=1

PROG=$(PACKAGE)

SRC_TAR=$(PACKAGE)-$(VERSION).src.tar.gz
i386_TAR=$(PACKAGE)-$(VERSION).i386.tar.gz
x86_64_TAR=$(PACKAGE)-$(VERSION).x86_64.tar.gz
TARS=$(SRC_TAR) $(i386_TAR) $(x86_64_TAR)
SRC_RPM=$(PACKAGE)-$(VERSION)-$(RELEASE).src.rpm
i386_RPM=$(PACKAGE)-$(VERSION)-$(RELEASE).i386.rpm
x86_64_RPM=$(PACKAGE)-$(VERSION)-$(RELEASE).x86_64.rpm
RPMS=$(SRC_RPM) $(i386_RPM) $(x86_64_RPM)
i386_DEBUG_RPM=$(PACKAGE)-debuginfo-$(VERSION)-$(RELEASE).i386.rpm
x86_64_DEBUG_RPM=$(PACKAGE)-debuginfo-$(VERSION)-$(RELEASE).x86_64.rpm
DEBUG_RPMS=$(i386_DEBUG_RPM) $(x86_64_DEBUG_RPM)

SRCS=cpuid.c

OTHER_SRCS=Makefile $(PROG).man cpuinfo2cpuid \
           $(PACKAGE).proto.spec $(PACKAGE).spec \
           ChangeLog FUTURE LICENSE
OTHER_BINS=$(PROG).man cpuinfo2cpuid.man

REL_DIR=../$(shell date +%Y-%m-%d)
WEB_DIR=/toad2/apps.mine/www/www/$(PROG)

BUILDROOT=

default: $(PROG) $(PROG).man.gz cpuinfo2cpuid cpuinfo2cpuid.man.gz

$(PROG): cpuid.c Makefile
	$(CC) $(CFL) $(LDFLAGS) -o $@ cpuid.c

$(PROG).man.gz: $(PROG).man
	gzip < $< > $@

cpuinfo2cpuid.man: cpuinfo2cpuid Makefile
	pod2man -r "$(VERSION)" -c "" $< > $@

cpuinfo2cpuid.man.gz: cpuinfo2cpuid.man
	gzip < $< > $@

install: $(PROG) $(PROG).man.gz cpuinfo2cpuid cpuinfo2cpuid.man.gz
	install -D -s -m 755 $(PROG)              $(BUILDROOT)/usr/bin/$(PROG)
	install -D    -m 444 $(PROG).man.gz       $(BUILDROOT)/usr/share/man/man1/$(PROG).1.gz
	install -D    -m 755 cpuinfo2cpuid        $(BUILDROOT)/usr/bin/cpuinfo2cpuid
	install -D    -m 444 cpuinfo2cpuid.man.gz $(BUILDROOT)/usr/share/man/man1/cpuinfo2cpuid.1.gz

clean:
	rm -f $(PROG) $(PROG).i386 $(PROG).x86_64
	rm -f $(PROG).man.gz
	rm -f cpuinfo2cpuid.man cpuinfo2cpuid.man.gz
	rm -f $(PACKAGE).spec
	rm -f $(TARS)
	rm -f $(RPMS)
	rm -f $(DEBUG_RPMS)
	rm -f $(PACKAGE)-*.src.tar.gz $(PACKAGE)-*.i386.tar.gz $(PACKAGE)-*.x86_64.tar.gz
	rm -f $(PACKAGE)-*.src.rpm $(PACKAGE)-*.i386.rpm $(PACKAGE)-*.x86_64.rpm
	rm -f $(PACKAGE)-debuginfo-*.i386.rpm $(PACKAGE)-debuginfo-*.x86_64.rpm

# Todd's Development rules

$(PROG).i386: cpuid.c Makefile
	$(CC) -m32 -Wl,--hash-style=both $(CFL) $(LDFLAGS) -o $@ cpuid.c

$(PROG).x86_64: cpuid.c Makefile
	$(CC) -m64 $(CFL) $(LDFLAGS) -o $@ cpuid.c

todd: $(PROG).i386 $(PROG).x86_64
	rm -f ~/.bin/execs/i586/$(PROG)
	rm -f ~/.bin/execs/x86_64/$(PROG)
	cp -p $(PROG).i386   ~/.bin/execs/i586/$(PROG)
	cp -p $(PROG).x86_64 ~/.bin/execs/x86_64/$(PROG)
	chmod 777 ~/.bin/execs/i586/$(PROG)
	chmod 777 ~/.bin/execs/x86_64/$(PROG)
	(cd ~/.bin/execs; prop i586/$(PROG) x86_64/$(PROG))

# Release rules

$(PACKAGE).spec: $(PACKAGE).proto.spec
	@(echo "%define version $(VERSION)"; \
	  echo "%define release $(RELEASE)"; \
	  cat $<) > $@

$(SRC_TAR): $(SRCS) $(OTHER_SRCS)
	@echo "Tarring source"
	@rm -rf $(PACKAGE)-$(VERSION)
	@mkdir $(PACKAGE)-$(VERSION)
	@ls -1d $(SRCS) $(OTHER_SRCS) | cpio -pdmuv $(PACKAGE)-$(VERSION)
	@tar cvf - $(PACKAGE)-$(VERSION) | gzip -c >| $(SRC_TAR)
	@rm -rf $(PACKAGE)-$(VERSION)

$(i386_TAR): $(PROG).i386 $(OTHER_BINS)
	@echo "Tarring i386 binary"
	@rm -rf $(PACKAGE)-$(VERSION)
	@mkdir $(PACKAGE)-$(VERSION)
	@ls -1d $(PROG).i386 $(OTHER_BINS) | cpio -pdmuv $(PACKAGE)-$(VERSION)
	@mv $(PACKAGE)-$(VERSION)/$(PROG).i386 $(PACKAGE)-$(VERSION)/$(PROG)
	@(cd $(PACKAGE)-$(VERSION); strip $(PROG))
	@tar cvf - $(PACKAGE)-$(VERSION) | gzip -c >| $(i386_TAR)
	@rm -rf $(PACKAGE)-$(VERSION)

$(x86_64_TAR): $(PROG).x86_64 $(OTHER_BINS)
	@echo "Tarring x86_64 binary"
	@rm -rf $(PACKAGE)-$(VERSION)
	@mkdir $(PACKAGE)-$(VERSION)
	@ls -1d $(PROG).x86_64 $(OTHER_BINS) | cpio -pdmuv $(PACKAGE)-$(VERSION)
	@mv $(PACKAGE)-$(VERSION)/$(PROG).x86_64 $(PACKAGE)-$(VERSION)/$(PROG)
	@(cd $(PACKAGE)-$(VERSION); strip $(PROG))
	@tar cvf - $(PACKAGE)-$(VERSION) | gzip -c >| $(x86_64_TAR)
	@rm -rf $(PACKAGE)-$(VERSION)

src_tar: $(SRC_TAR)

tar tars: $(TARS)

$(i386_RPM) $(i386_DEBUG_RPM) $(SRC_RPM): $(SRC_TAR) $(PACKAGE).spec
	@echo "Building i386 RPMs"
	@rm -rf build
	@mkdir build
	@rpmbuild -ba --target i386 \
	          --buildroot "${PWD}/build" \
	          --define "_builddir ${PWD}/build" \
	          --define "_rpmdir ${PWD}" \
	          --define "_srcrpmdir ${PWD}" \
	          --define "_sourcedir ${PWD}" \
	          --define "_specdir ${PWD}" \
	          --define "__check_files ''" \
	          --define "_rpmfilename %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
	          $(PACKAGE).spec
	@rm -rf build

$(x86_64_RPM) $(x86_64_DEBUG_RPM): $(SRC_TAR) $(PACKAGE).spec
	@echo "Building x86_64 RPMs"
	@rm -rf build
	@mkdir build
	@rpmbuild -ba --target x86_64 \
	          --buildroot "${PWD}/build" \
	          --define "_builddir ${PWD}/build" \
	          --define "_rpmdir ${PWD}" \
	          --define "_srcrpmdir ${PWD}" \
	          --define "_sourcedir ${PWD}" \
	          --define "_specdir ${PWD}" \
	          --define "__check_files ''" \
	          --define "_rpmfilename %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
	          $(PACKAGE).spec
	@rm -rf build

rpm rpms: $(RPMS)

# Todd's release rules

release: $(PROG) $(PROG).i386 $(PROG).x86_64 $(TARS) $(RPMS)
	if [ -d $(REL_DIR) ]; then                         \
	   echo "Makefile: $(REL_DIR) already exists" >&2; \
	   exit 1;                                         \
	fi
	mkdir $(REL_DIR)
	cp -p $(PROG) $(PROG).i386 $(PROG).x86_64 $(SRCS) $(OTHER_SRCS) $(REL_DIR)
	mv $(TARS) $(RPMS) $(REL_DIR)
	if [ -e $(i386_DEBUG_RPM) ]; then   \
	   mv $(i386_DEBUG_RPM) $(REL_DIR); \
	fi
	if [ -e $(x86_64_DEBUG_RPM) ]; then  \
	   mv $(x86_64_DEBUG_RPM) $(REL_DIR); \
	fi
	chmod -w $(REL_DIR)/*
	cp -f -p $(REL_DIR)/*.tar.gz $(REL_DIR)/*.rpm $(WEB_DIR)
	rm -f $(PACKAGE).spec

rerelease:
	rm -rf $(REL_DIR)
	$(MAKE) -$(MAKEFLAGS) release
