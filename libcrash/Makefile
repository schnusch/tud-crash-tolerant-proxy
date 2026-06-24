flavours = nop

.PHONY: all clean

all: $(flavours:=/libcrash.so)

clean:
	$(RM) -r $(flavours)

%/libcrash.so: %.c libcrash.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -fPIC -o $@ $< $(LDFLAGS)
