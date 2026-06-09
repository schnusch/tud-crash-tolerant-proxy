OBJS = \
	../obj/common/ipc.o \
	../obj/common/util.o \
	../obj/common/shared_memory.o \
	../obj/listener/cmdline.o \
	../obj/listener/fd_map.o

.PHONY: check clean

check: main
	valgrind ./main

clean:
	$(RM) -r main main.d

main: main.cc $(OBJS)
	$(MAKE) -C .. final_cppflags='$(final_cppflags)' $(patsubst ../%,%,$(OBJS))
	$(CXX) $(final_cppflags) $(final_cxxflags) -o $@ $< $(OBJS) -lgtest

../%.o: main.cc
	$(MAKE) -C .. final_cppflags='$(final_cppflags)' $(patsubst ../%,%,$@)

main.d: main.cc
	@mkdir -p $(@D)
	@printf 'DEP\t%s\n' $< >&2
	@$(CXX) $(final_cppflags) -MM -MF $@ -MT '$(@:.d=) $@' $<
-include main.d
