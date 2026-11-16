CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SDL_CONFIG ?= $(shell which sdl2-config 2>/dev/null || echo /opt/homebrew/bin/sdl2-config)
    EXTRA_CFLAGS := $(shell $(SDL_CONFIG) --cflags) -DUSE_SDL=1
    EXTRA_LIBS   := $(shell $(SDL_CONFIG) --libs)
else
    EXTRA_CFLAGS :=
    EXTRA_LIBS   :=
endif

TARGET := esp32c5-spectrum
SRCS   := main.c eng_font.c eng_font_aa.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(SRCS) -o $(TARGET) $(EXTRA_LIBS) -lm -lpthread

clean:
	rm -f $(TARGET)

.PHONY: all clean
