# customization variables

# Location where the finished executables will be installed, e.g. /usr/local
PREFIX = /usr/local

# Paths of the finished executables relative to $(PREFIX).
LISTENER = bin/crash-tolerant-proxy
WORKER = libexec/crash-tolerant-proxy-worker

# Pre-processor flags
# Use $(*FLAGS) to replace default flags and $(EXTRA_*FLAGS) to append.
CPPFLAGS = -D_GNU_SOURCE -DLIBCRASH_SIGNAL=SIGUSR2
EXTRA_CPPFLAGS =
final_cppflags = $(CPPFLAGS) $(EXTRA_CPPFLAGS)

# Compiler flags
# Use $(*FLAGS) to replace default flags and $(EXTRA_*FLAGS) to append.
CFLAGS = -g -O2 -Wall -Wextra -Wpedantic
EXTRA_CFLAGS = #-fsanitize=address,undefined
final_cflags = $(CFLAGS) $(EXTRA_CFLAGS) -std=c99 -Werror=vla \
	-DLISTENER_PATH='"$(PREFIX)/$(LISTENER)"' \
	-DWORKER_PATH='"$(PREFIX)/$(WORKER)"'

LIBCRASH_FLAVOR = nop

# Linker flags
# Use $(*FLAGS) to replace default flags and $(EXTRA_*FLAGS) to append.
LDFLAGS = -Llibcrash/$(LIBCRASH_FLAVOR) -lcrash # -Wl,-rpath,'$$ORIGIN/../libcrash/$(LIBCRASH_FLAVOR)'
EXTRA_LDFLAGS = #-fsanitize=address,undefined
final_ldflags = $(LDFLAGS) $(EXTRA_LDFLAGS)

# internal varibales

COMMON_SRCS = $(shell find common -xtype f -name '*.c')

LISTENER_SRCS = $(shell find listener -xtype f -name '*.c') $(COMMON_SRCS)
LISTENER_OBJS = $(patsubst %.c,obj/%.o,$(LISTENER_SRCS))

WORKER_SRCS = $(shell find worker -xtype f -name '*.c') \
	$(COMMON_SRCS) \
	thirdparty/picohttpparser/picohttpparser.c
WORKER_OBJS = $(patsubst %.c,obj/%.o,$(WORKER_SRCS))

MAKE_LIBCRASH = $(MAKE) \
	-C libcrash \
	CPPFLAGS='$(final_cppflags)' \
	CFLAGS='$(final_cflags)'

# *.o are located in obj/, executables in bin/.

.PHONY: all check clean install

all: bin/listener bin/worker
	$(MAKE_LIBCRASH) $@

check:
	$(MAKE) -C tests CPPFLAGS='$(final_cppflags)' LDFLAGS='$(filter -lbacktrace -lsystemd,$(final_ldflags))' $@

doc: Doxyfile $(LISTENER_SRCS) $(WORKER_SRCS)
	{ cat Doxyfile && echo 'OUTPUT_DIRECTORY = ./$@.tmp'; } | doxygen -
	$(RM) -r $@
	mv $@.tmp $@

clean:
	$(RM) -r bin doc obj notes.html *.tmp vgcore.*
	$(MAKE) -C tests $@

install: bin/listener bin/worker
	install -Dm 755 bin/listener $(DESTDIR)$(PREFIX)/$(LISTENER)
	install -Dm 755 bin/worker $(DESTDIR)$(PREFIX)/$(WORKER)
	install -Dm 755 libcrash/$(LIBCRASH_FLAVOR)/libcrash.so $(DESTDIR)$(PREFIX)/lib/libcrash.so

bin/launcher: obj/launcher.o obj/common/util.o
	@mkdir -p $(@D)
	$(CC) -o $@ $^ $(filter -lbacktrace,$(final_ldflags))

bin/listener: $(LISTENER_OBJS)
	$(MAKE_LIBCRASH) $(LIBCRASH_FLAVOR)/libcrash.so
	@mkdir -p $(@D)
	$(CC) -o $@ $(LISTENER_OBJS) $(final_ldflags)

bin/worker: $(WORKER_OBJS)
	$(MAKE_LIBCRASH) $(LIBCRASH_FLAVOR)/libcrash.so
	@mkdir -p $(@D)
	$(CC) -o $@ $(WORKER_OBJS) $(final_ldflags)

obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(final_cppflags) $(final_cflags) -c -o $@ $<

notes.html: NOTES.md
	{ echo '<style>'; \
	  echo '  h1, h2, h3, h4, h5, h6 { break-after: avoid; }'; \
	  echo '  ul { padding-left: 1em; }'; \
	  echo '  p, pre { margin: 0; }'; \
	  echo '  blockquote { margin: 0; padding-left: 0.5em; border-left: 0.25em solid lightgray; }'; \
	  echo '  img { max-width: 100%; }'; \
	  echo '  @media print {'; \
	  echo '    body { font-size: 11pt; column-count: 2; column-fill: auto; }'; \
	  echo '  }'; \
	  echo '</style>'; \
	  echo; \
	  cat NOTES.md; \
	} | pandoc -o $@ -f markdown+gfm_auto_identifiers --lua-filter=thirdparty/pandoc-ext/diagram/diagram.lua --embed-resources

# https://www.gnu.org/software/make/manual/html_node/Automatic-Prerequisites.html
obj/%.d: %.c
	@mkdir -p $(@D)
	@printf 'DEP\t%s\n' $< >&2
	@$(CC) $(final_cppflags) -MM -MF $@ -MT '$(@:.d=.o) $@' $<
ifneq ($(MAKECMDGOALS),clean)
-include $(LISTENER_OBJS:.o=.d) $(WORKER_OBJS:.o=.d) obj/launcher.d
endif
