PREREQS = \
	../obj/common/ipc.o \
	../obj/common/util.o \
	../obj/common/shared_memory.o \
	../obj/listener/cmdline.o \
	../obj/listener/fd_info.o \
	../obj/listener/worker_process.o \
	../obj/worker/atomic_send_recv.o

OBJS = \
	atomic.o \
	cmdline.o \
	ipc.o \
	util.o \
	worker.o

CPPFLAGS =
final_cppflags := $(CPPFLAGS) $(shell pkgconf --cflags gtest)

CXXFLAGS =
final_cxxflags = $(CXXFLAGS)

LDFLAGS =
final_ldflags := $(LDFLAGS) $(shell pkgconf --libs gtest)

.PHONY: check clean

check: main
	$(MAKE) -C .. final_cppflags='$(final_cppflags) -DUSE_LIBCRASH' bin/worker
	valgrind ./main

clean:
	$(RM) -r main *.d *.o

main: main.o $(OBJS) $(PREREQS)
	$(MAKE) -C .. final_cppflags='$(final_cppflags) -DUSE_LIBCRASH' $(patsubst ../%,%,$(PREREQS))
	$(CXX) $(final_cppflags) $(final_cxxflags) -o $@ $< $(OBJS) $(PREREQS) $(final_ldflags)

../%.o: main.cc
	$(MAKE) -C .. final_cppflags='$(final_cppflags) -DUSE_LIBCRASH' $(patsubst ../%,%,$@)

%.o: %.cc
	$(CXX) $(final_cppflags) $(final_cxxflags) -c -o $@ $<

%.d: %.cc
	@mkdir -p $(@D)
	@printf 'DEP\t%s\n' $< >&2
	@$(CXX) $(final_cppflags) -MM -MF $@ -MT '$(@:.d=.o) $@' $<
ifneq ($(MAKECMDGOALS),clean)
-include main.d $(WORKER_OBJS:.o=.d)
endif
