CC       ?= gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE
LDLIBS    = -lm -lpthread

OBJS = main.o term.o noise.o cloud.o render.o prerender.o

clouds: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

main.o: main.c cloud.h prerender.h render.h term.h
term.o: term.c term.h
prerender.o: prerender.c prerender.h render.h cloud.h term.h
noise.o: noise.c noise.h
cloud.o: cloud.c cloud.h noise.h
render.o: render.c render.h cloud.h

# separate binary so debug objects never shadow the release build
SRCS = main.c term.c noise.c cloud.c render.c prerender.c
debug: $(SRCS)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	    $(CPPFLAGS) -o clouds-debug $(SRCS) $(LDLIBS)

clean:
	rm -f clouds clouds-debug $(OBJS)

.PHONY: clean debug
