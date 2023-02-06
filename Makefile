# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?=  -fno-common -g -ggdb
	SHOBJ_LDFLAGS ?= -shared -Bsymbolic
else
	SHOBJ_CFLAGS ?= -dynamic -fno-common -g -ggdb
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif
CXXFLAGS = -I$(RM_INCLUDE_DIR) -Wall -g -fPIC -lc -lm --std=c++20
CXX=gcc

all: ratelimits.so

ratelimits.so: ratelimits.o
	$(LD) -o $@ ratelimits.o $(SHOBJ_LDFLAGS) $(LIBS) -L$(RMUTIL_LIBDIR) -lrmutil -lc

clean:
	rm -rf *.xo *.so *.o

FORCE:
