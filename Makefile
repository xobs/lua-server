SOURCES=fcgi-wrapper.c file.c
OBJECTS=$(SOURCES:.c=.o)
EXEC=fcgi-wrapper
MY_CFLAGS += -Wall -Werror -O0 -g
MY_LIBS += -lfcgi -lpthread

all: $(OBJECTS)
	$(CC) $(LIBS) $(LDFLAGS) $(OBJECTS) $(MY_LIBS) -o $(EXEC)

clean:
	rm -f $(EXEC) $(OBJECTS)

.c.o:
	$(CC) -c $(CFLAGS) $(MY_CFLAGS) $< -o $@

