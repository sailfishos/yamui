PROGRAM = yamui
C_FILES := main.c os-update.c minui/graphics.c minui/graphics_fbdev.c minui/events.c minui/resources.c
OBJS := $(patsubst %.c, %.o, $(C_FILES))
CC = cc
CFLAGS = -Wall -DOVERSCAN_PERCENT=0 -I/usr/include/ -O2 -Wall
LDFLAGS = -lpng -lc -lz -lm

SCREENSAVERD = yamui-screensaverd
CFLAGS_SCREENSAVERD = -W -Wall -ansi -pedantic -O2
C_FILES_SCREENSAVERD := yamui-screensaverd.c
OBJS_SCREENSAVERD := $(patsubst %.c, %.o, $(C_FILES_SCREENSAVERD))

all: $(PROGRAM) $(SCREENSAVERD)

$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $(PROGRAM)

$(SCREENSAVERD): $(OBJS_SCREENSAVERD)
	$(CC) $(CFLAGS_SCREENSAVERD) $(OBJS_SCREENSAVERD) -o $(SCREENSAVERD)

install: all
	strip $(PROGRAM) $(SCREENSAVERD)
	install -m 755 -D $(PROGRAM) $(DESTDIR)/usr/bin/$(PROGRAM)
	install -m 755 -D $(SCREENSAVERD) $(DESTDIR)/usr/bin/$(SCREENSAVERD)

clean:
	rm -f *.o minui/*.o
