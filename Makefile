# Build PSP Doom inside the pspdev Docker toolchain.
#
#   make            build PARAM.SFO, EBOOT.PBP, dvemgr.prx and relaunch.prx
#   make doom       build only EBOOT.PBP
#   make dvemgr     build only dvemgr.prx
#   make relaunch   build only relaunch.prx
#   make sfo        regenerate source/xmb/PARAM.SFO
#   make snd0       encode source/xmb/bgm.wav to SND0.AT3 (XMB music)
#   make clean      remove build artifacts and root EBOOT.PBP/*.prx
#   make shell      interactive shell in the toolchain container
#   make pull       pull/update the toolchain image
#   make deploy     rsync game files to the PSP memory stick (deploy.sh)
#   make run        build, then launch EBOOT.PBP in PPSSPP

IMAGE       ?= pspdev/pspdev:latest
PLATFORM    ?= linux/amd64
DOCKER      ?= docker
WINE        ?= wine
FFMPEG      ?= ffmpeg
PPSSPP      ?= PPSSPPSDL

# XMB assets (icon, background, music, PARAM.SFO) live here.
ASSETS = source/xmb
# Sony's encoder (from ATRACTool-Reloaded portable), run on the host through wine.
# Fetched on first use by source/tools/fetch-atractool.sh.
AT3TOOL ?= source/tools/ATRACTool-Rel-Portable/res/psp_at3tool.exe
# XMB plays SND0.AT3 from a fixed buffer: keep it <=30s / <=500KB.
SND0_SECONDS ?= 29.5
SND0_FADE    ?= 1.5

# Flags needed for this 2007 codebase with a modern GCC.
COMPAT_CFLAGS ?= -std=gnu99 -fcommon -Dstrcmpi=strcasecmp -Wno-error=implicit-function-declaration \
                 -Wno-error=incompatible-pointer-types -Wno-error=int-conversion

RUN = $(DOCKER) run --rm --platform $(PLATFORM) \
      -v "$(CURDIR)":/src -w /src $(IMAGE)

.PHONY: all sfo doom dvemgr relaunch snd0 clean shell pull deploy run

all: sfo doom dvemgr relaunch

sfo:
	$(RUN) make -C source/src ../xmb/PARAM.SFO

doom: $(ASSETS)/SND0.AT3
	$(RUN) make -C source/src EXTRA_CFLAGS='$(COMPAT_CFLAGS)'
	mv -f source/src/EBOOT.PBP EBOOT.PBP

dvemgr:
	$(RUN) make -C source/dvemgr EXTRA_CFLAGS='$(COMPAT_CFLAGS)'
	mv -f source/dvemgr/dvemgr.prx dvemgr.prx

relaunch:
	$(RUN) make -C source/relaunch EXTRA_CFLAGS='$(COMPAT_CFLAGS)'
	mv -f source/relaunch/relaunch.prx relaunch.prx

snd0: $(ASSETS)/SND0.AT3

# ffmpeg trims/fades to 16-bit 44.1kHz stereo PCM (at3tool's required input),
# at3tool encodes ATRAC3plus 128kbps (what the XMB plays) with a whole-file loop.
# at3tool gets relative paths: wine can't take the unix /tmp path directly.
$(ASSETS)/SND0.AT3: $(ASSETS)/bgm.wav Makefile
	[ -f $(AT3TOOL) ] || source/tools/fetch-atractool.sh
	$(FFMPEG) -hide_banner -loglevel error -y -i $< -t $(SND0_SECONDS) \
	  -af "afade=t=out:st=$(shell echo "$(SND0_SECONDS) - $(SND0_FADE)" | bc):d=$(SND0_FADE)" \
	  -ar 44100 -ac 2 -c:a pcm_s16le $(ASSETS)/.snd0.wav
	WINEDEBUG=-all $(WINE) $(AT3TOOL) -e -wholeloop $(ASSETS)/.snd0.wav $@
	rm -f $(ASSETS)/.snd0.wav

clean:
	$(RUN) sh -c 'make -C source/src clean; make -C source/dvemgr clean; make -C source/relaunch clean'
	rm -f EBOOT.PBP dvemgr.prx relaunch.prx $(ASSETS)/SND0.AT3 $(ASSETS)/PARAM.SFO $(ASSETS)/.snd0.wav

shell:
	$(DOCKER) run --rm -it --platform $(PLATFORM) \
	  -v "$(CURDIR)":/src -w /src $(IMAGE) bash

pull:
	$(DOCKER) pull --platform $(PLATFORM) $(IMAGE)

run: doom
	$(PPSSPP) "$(CURDIR)/EBOOT.PBP"
