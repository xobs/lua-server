CFLAGS ?= -g
LDFLAGS ?= -lfcgi -lpthread
LD = gcc
CC = gcc

all: fcgi-wrapper.o file.o
	$(LD) $(LDFLAGS) fcgi-wrapper.o file.o -o fcgi-wrapper

fcgi-wrapper.o: fcgi-wrapper.c
	$(CC) -c $(CFLAGS) fcgi-wrapper.c -o fcgi-wrapper.o

file.o: file.c
	$(CC) -c $(CFLAGS) file.c -o file.o

clean:
	rm -f fcgi-wrapper.o file.o fcgi-wrapper
