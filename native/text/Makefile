# Dirge of Cerberus - native text/dialogue subsystem
# Build: make        (needs zlib: libz-dev)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  := -lz

BINS := doctext docfont docrender

all: $(BINS)

doctext: doctext.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

docfont: docfont.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

docrender: docrender.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# --- demo / validation targets --------------------------------------------
EXTRACT := ../../extract/_KEL
ETC     := $(EXTRACT)/data/etc

# decrypt a Japanese save-label and an English/Japanese tutorial file
demo-text: doctext
	./doctext $(EXTRACT)/noPath/06800020.bin   # JP save labels (Shift-JIS)
	./doctext $(EXTRACT)/noPath/0c7ee100.bin   # EN tutorial
	./doctext $(EXTRACT)/noPath/0c7ee1c8.bin   # JP tutorial

# decode the font atlases (kelfnt0..4) to fonttex/
demo-font: docfont
	mkdir -p fonttex
	./docfont $(ETC)/fontdata2.rfd fonttex    # kelfnt4  (CLEAN)
	./docfont $(ETC)/fontdata1.bin fonttex    # kelfnt0..3 (PARTIAL)

# render real game glyphs + the hero sample PNG
demo-render: docrender demo-font
	./docrender fonttex/kelfnt4.gray 512 256 16 \
	  128,129,130,131,132,133,134,135,136,137,138,139 fonttex/real_font_line.png
	python3 -I render_sample.py fonttex/real_font_line.png /srv/shared/dirge-port/text_sample.png

# render REAL decrypted dialogue with CORRECT glyphs via the reversed SJIS->cell map
demo-dialogue: demo-font
	python3 -I render_dialogue.py
	mkdir -p /srv/shared/dirge-port
	cp out/13_dialogue_native.png /srv/shared/dirge-port/13_dialogue_native.png

clean:
	rm -f $(BINS)

.PHONY: all clean demo-text demo-font demo-render demo-dialogue
