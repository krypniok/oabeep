CC = gcc
CFLAGS = -g
LIBS = -lm -lopenal

binaural: binaural.c
	$(CC) $(CFLAGS) binaural.c $(LIBS) -o oabeep

.PHONY: clean

clean:
	rm -f oabeep

