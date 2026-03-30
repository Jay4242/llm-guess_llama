CC = gcc
CFLAGS = -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -g
LDFLAGS ?=
LIBS = -lcurl -ljansson -lraylib -lpthread
SOURCES = \
	guess_llama.c \
	game_state.c \
	llm_backend.c \
	storage.c \
	gameplay.c \
	game_setup.c \
	stable-diffusion.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: guess_llama

guess_llama: $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS) $(LIBS)

clean:
	rm -f guess_llama $(OBJECTS)
