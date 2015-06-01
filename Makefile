PROGRAM = yamui
C_FILES := main.c os-update.c minui/graphics.c minui/graphics_fbdev.c minui/events.c minui/resources.c
OBJS := $(patsubst %.c, %.o, $(C_FILES))
CC = cc
CFLAGS = -Wall -DOVERSCAN_PERCENT=0 -I/usr/include/ -O2 -Wall
LDFLAGS = -lpng -lc -lz -lm
all: $(PROGRAM)
$(PROGRAM): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $(PROGRAM)
install: all
	install -m 755 -D $(PROGRAM) $(DESTDIR)/usr/bin/$(PROGRAM)
clean:
	rm -f *.o minui/*.o

