ifeq ($(WIN32), 1)
export PREFIX = i586-mingw32msvc-
export EXE := .exe
export LFLAGS = -lws2_32
else
export PREFIX =
# Native Windows compilers (MSYS2, Cygwin) add .exe to their output.
export EXE := $(if $(findstring _NT,$(shell uname -s)),.exe)
export LFLAGS =
endif

empty :=
space := $(empty) $(empty)
escape = $(subst $(space),\$(space),$1)

export PYWII := $(CURDIR)/../../pywii/pywii-tools
export ALAMEDA := $(CURDIR)/../../pywii/Alameda
export PYTHON ?= python3
export PYTHONPATH := $(CURDIR)/../../pywii/Common:$(PYTHONPATH)

export PNG2TPL := "$(PYTHON)" $(call escape,$(CURDIR))/tools/png2tpl.py
export MKBNS := $(call escape,$(CURDIR))/tools/mkbns$(EXE)
export LZ77 := $(call escape,$(CURDIR))/tools/lz77$(EXE)
export ADDIMD5 := "$(PYTHON)" $(call escape,$(CURDIR))/tools/addimd5.py
export ARCPACK := "$(PYTHON)" "$(PYWII)/arcpack.py"
export WAV2RAW := "$(PYTHON)" $(call escape,$(CURDIR))/tools/wav2raw.py

all: channel.imet

channel.imet: build/data.arc names.txt tools/join-imet.py
	"$(PYTHON)" tools/join-imet.py $@ build/data.arc build/icon.arc build/banner.arc build/sound.bns names.txt

build/data.arc : build/data/meta/icon.bin build/data/meta/banner.bin build/data/meta/sound.bin
	$(ARCPACK) $@ build/data

build/data/meta/icon.bin : icon/* tools/png2tpl.py $(LZ77)
	$(MAKE) -C icon -f ../Makefile.section TYPE=icon

build/data/meta/banner.bin : banner/* tools/png2tpl.py $(LZ77)
	$(MAKE) -C banner -f ../Makefile.section TYPE=banner

build/data/meta/sound.bin : build/sound.bns
	@[ ! -d build/data/meta ] && mkdir -p build/data/meta || true
	$(ADDIMD5) $< $@

build/sound.bns : build/wiibrew-banner-intro-part.raw build/wiibrew-banner-loop-part.raw $(MKBNS)
	@[ ! -d build ] && mkdir -p build || true
	$(MKBNS) build/wiibrew-banner-intro-part.raw $@ 1 build/wiibrew-banner-loop-part.raw

build/%.raw : sound/%.wav tools/wav2raw.py
	@[ ! -d build ] && mkdir -p build || true
	$(WAV2RAW) $< $@

testi : channel.imet
	"$(PYTHON)" "$(ALAMEDA)/Alameda.py" channel.imet icon
testb : channel.imet
	"$(PYTHON)" "$(ALAMEDA)/Alameda.py" channel.imet banner

$(MKBNS): tools/*.c
	$(MAKE) -C tools mkbns$(EXE)

$(LZ77): tools/*.c
	$(MAKE) -C tools lz77$(EXE)

clean:
	rm -f channel.imet
	rm -rf build
	$(MAKE) -C tools clean
