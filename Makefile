CFLAGS ?= -g
LDFLAGS ?= -lfcgi -lpthread
LD = gcc
CC = gcc

all: fcgi-wrapper.o
	$(LD) $(LDFLAGS) fcgi-wrapper.o -o fcgi-wrapper

fcgi-wrapper.o: fcgi-wrapper.c
	$(CC) -c $(CFLAGS) fcgi-wrapper.c -o fcgi-wrapper.o

clean:
	rm -f fcgi-wrapper.o fcgi-wrapper
