PKG_CONFIG = pkg-config

WLR_CFLAGS = $(shell $(PKG_CONFIG) --cflags wlroots-0.19)
WLR_LIBS = $(shell $(PKG_CONFIG) --libs wlroots-0.19)

CFLAGS = -Wall -Wextra -g $(WLR_CFLAGS) -DWLR_USE_UNSTABLE
LDFLAGS = $(WLR_LIBS) -lwayland-server -lxkbcommon -lm

SRC = main.c server.c output.c seat.c config.c
OBJ = $(SRC:.c=.o)
TARGET = prot

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c server.h config.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	sudo ./$(TARGET)

.PHONY: all clean run