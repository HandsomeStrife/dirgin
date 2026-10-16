# Dirge of Cerberus — native audio decoder
# at2 (ATRAC3) / at3 (ATRAC3plus) headerless streams -> PCM WAV.
# Decode path links libavcodec's atrac3/atrac3plus (dev pkgs required).

CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
PKGS     = libavcodec libavutil libswresample
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm

BIN = dirge_atdec

# dirge_wd (SPU2 .wd sound banks -> PCM WAV) is self-contained: no libav deps,
# plain C + libm only.
WDBIN    = dirge_wd
WDCFLAGS = -O2 -std=c11 -Wall -Wextra

# dirge_sound (playback/mixing engine + db.bin sound-request API) is also
# self-contained: plain C11 + libm. dirge_sound_demo links it and drives a
# validation scene, shelling out to the decoder binaries above (read-only).
SNDLIB    = dirge_sound.o
SNDDEMO   = dirge_sound_demo
SNDCFLAGS = -O2 -std=c11 -Wall -Wextra

all: $(BIN) $(WDBIN) $(SNDDEMO)

$(BIN): dirge_atdec.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(WDBIN): dirge_wd.c
	$(CC) $(WDCFLAGS) -o $@ $< -lm

$(SNDLIB): dirge_sound.c dirge_sound.h
	$(CC) $(SNDCFLAGS) -c -o $@ dirge_sound.c

$(SNDDEMO): dirge_sound_demo.c $(SNDLIB) dirge_sound.h
	$(CC) $(SNDCFLAGS) -o $@ dirge_sound_demo.c $(SNDLIB) -lm

# quick self-check against the extracted assets
EXT ?= /srv/games/dirge-port/extract/_KEL
check: $(BIN)
	./$(BIN) $(EXT)/data/sound2/resat3/on_v132.at2 /tmp/_voice.wav
	./$(BIN) $(EXT)/data/etc/title.at3            /tmp/_title.wav
	./$(BIN) $(EXT)/data/sound2/resat3/bgm_136.at3 /tmp/_bgm.wav || true

# .wd bank self-check: list a bank and extract a couple of SFX to WAV
wdcheck: $(WDBIN)
	./$(WDBIN) $(EXT)/data/sound2/wave0000.wd | head -20
	./$(WDBIN) $(EXT)/data/sound2/wave0000.wd -o /tmp -i 15
	./$(WDBIN) $(EXT)/data/sound2/wave0000.wd -o /tmp -i 80

# mixer validation: resolve a few db.bin ids + mix a real decoded scene to WAV
sndcheck: $(SNDDEMO) $(BIN) $(WDBIN)
	./$(SNDDEMO) $(EXT)/data/sound2 . /srv/shared/dirge-port/sound_mix_sample.wav

clean:
	rm -f $(BIN) $(WDBIN) $(SNDLIB) $(SNDDEMO)

.PHONY: all check wdcheck sndcheck clean
