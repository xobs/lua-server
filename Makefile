SOURCES=fcgi-wrapper.c file.c lua.c
OBJECTS=$(SOURCES:.c=.o)
EXEC=fcgi-wrapper
MY_CFLAGS += -Wall -Werror -O0 -g -DPROJECT_DIR=\"/Users/smc/Sites/dev/luaed/projects\"

MY_LIBS += -lfcgi -lpthread -llua

all: $(OBJECTS)
	$(CC) $(LIBS) $(LDFLAGS) $(OBJECTS) $(MY_LIBS) -o $(EXEC)

clean:
	rm -f $(EXEC) $(OBJECTS)

.c.o:
	$(CC) -c $(CFLAGS) $(MY_CFLAGS) $< -o $@

