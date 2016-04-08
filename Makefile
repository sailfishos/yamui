PROGRAM = yamui
MINUI_C_FILES := minui/graphics.c minui/graphics_fbdev.c minui/events.c minui/resources.c
C_FILES := main.c os-update.c $(MINUI_C_FILES)
OBJS := $(patsubst %.c, %.o, $(C_FILES))
CC = cc
CFLAGS = -Wall -DOVERSCAN_PERCENT=0 -I/usr/include/ -O2 -W -ansi
LDFLAGS = -lpng -lc -lz -lm

OBJS_COMMON := yamui-tools.o

SCREENSAVERD = yamui-screensaverd
CFLAGS_SCREENSAVERD = -W -Wall -ansi -pedantic -O2
C_FILES_SCREENSAVERD := yamui-screensaverd.c $(MINUI_C_FILES)
OBJS_SCREENSAVERD := $(patsubst %.c, %.o, $(C_FILES_SCREENSAVERD))

POWERKEY = yamui-powerkey
CFLAGS_POWERKEY = -W -Wall -ansi -pedantic -O2
C_FILES_POWERKEY := yamui-powerkey.c
OBJS_POWERKEY := $(patsubst %.c, %.o, $(C_FILES_POWERKEY))

all: $(PROGRAM) $(SCREENSAVERD) $(POWERKEY)

$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $(PROGRAM)

$(SCREENSAVERD): $(OBJS_SCREENSAVERD) $(OBJS_COMMON)
	$(CC) $(CFLAGS_SCREENSAVERD) $(OBJS_SCREENSAVERD) $(OBJS_COMMON) $(LDFLAGS) -o $(SCREENSAVERD)

$(POWERKEY): $(OBJS_POWERKEY) $(OBJS_COMMON)
	$(CC) $(CFLAGS_POWERKEY) $(OBJS_POWERKEY) $(OBJS_COMMON) -o $(POWERKEY)

install: all
	strip $(PROGRAM) $(SCREENSAVERD)
	install -m 755 -D $(PROGRAM) $(DESTDIR)/usr/bin/$(PROGRAM)
	install -m 755 -D $(SCREENSAVERD) $(DESTDIR)/usr/bin/$(SCREENSAVERD)
	install -m 755 -D $(POWERKEY) $(DESTDIR)/usr/bin/$(POWERKEY)

clean:
	rm -f *.o minui/*.o $(PROGRAM) $(SCREENSAVERD) $(POWERKEY)
