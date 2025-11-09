CC = gcc
CFLAGS = -g
LIBS = -lm -lopenal

oabeep: oabeep.c
	$(CC) $(CFLAGS) oabeep.c $(LIBS) -o oabeep

.PHONY: clean

clean:
	rm -f oabeep

.PHONY: push

push:
	@echo "==> cleaning build artefacts..."
	$(MAKE) clean
	rm -f *.bin *.img
	@echo "==> adding changes..."
	git add .
	@echo "==> committing..."
	git commit -m "$(if $(m),$(m),auto-push from make)" || echo "==> no changes to commit"
	@echo "==> force pushing..."
	git push origin master --force
