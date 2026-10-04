CHANNEL_VERSION = 1.9.6

TOOLS = ../../pywii/pywii-tools
PYWII = ../../pywii/Common/pywii/wii.py
PYTHON ?= python3
# The title ID: OHBC, or LULZ to install over the original Homebrew Channel.
TITLE ?= OHBC
CERTS = $(CURDIR)/dpki/certs
DPKI_ISSUER_TIK = Root-CA00000002-XS00000006
DPKI_ISSUER_TMD = Root-CA00000002-CP00000007

all: channel_retail.wad

dpki: channel_dpki.wad

channel_retail.wad: Makefile $(TOOLS)/wadpack.py $(PYWII) retail/cetk retail/tmd 00000000 00000001 footer
	@"$(PYTHON)" "$(TOOLS)/wadpack.py" $@ retail/ .

channel_dpki.wad: Makefile $(TOOLS)/wadpack.py $(PYWII) dpki/cetk dpki/tmd 00000000 00000001 footer
	@"$(PYTHON)" "$(TOOLS)/wadpack.py" -dpki $@ dpki/ .

00000000: ../banner/channel.imet footer Makefile
	@cat footer $< > $@

00000001: ../channelapp/channelapp-channel.dol Makefile
	@cp $< $@

# Rewritten only when TITLE changes, so the ticket and TMD follow it.
title.stamp: FORCE
	@echo '$(TITLE)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

retail/cetk: cetk.template title.stamp Makefile $(TOOLS)/retitle.py $(PYWII)
	@cp $< $@
	@"$(PYTHON)" "$(TOOLS)/retitle.py" tik $@ $(TITLE)

dpki/cetk: retail/cetk Makefile $(TOOLS)/dpkisign.py $(PYWII)
	@"$(PYTHON)" "$(TOOLS)/dpkisign.py" -cetk $< $@ "$(CERTS)" $(DPKI_ISSUER_TIK)

retail/tmd: tmd.template title.stamp 00000000 00000001 Makefile $(TOOLS)/retitle.py $(TOOLS)/tmdupdatecr.py $(TOOLS)/tmdvers.py $(PYWII)
	@cp $< $@
	@"$(PYTHON)" "$(TOOLS)/retitle.py" tmd $@ $(TITLE)
	@"$(PYTHON)" "$(TOOLS)/tmdupdatecr.py" $@ "$(CURDIR)"
	@"$(PYTHON)" "$(TOOLS)/tmdvers.py" $@ $(CHANNEL_VERSION)

dpki/tmd: retail/tmd Makefile $(TOOLS)/dpkisign.py $(PYWII)
	@"$(PYTHON)" "$(TOOLS)/dpkisign.py" -tmd $< $@ "$(CERTS)" $(DPKI_ISSUER_TMD)

check: check-retail

check-retail: all
	@echo ===== RETAIL =====
	@"$(PYTHON)" "$(TOOLS)/tikinfo.py" retail/cetk retail/certs
	@"$(PYTHON)" "$(TOOLS)/tmdinfo.py" retail/tmd retail/certs

check-dpki: dpki
	@echo ===== DPKI =====
	@"$(PYTHON)" "$(TOOLS)/tikinfo.py" -dpki dpki/cetk dpki/certs
	@"$(PYTHON)" "$(TOOLS)/tmdinfo.py" -dpki dpki/tmd dpki/certs

clean:
	rm -f title.stamp retail/cetk dpki/cetk retail/tmd retail/tmd 00000000 00000001 channel_retail.wad channel_dpki.wad

.PHONY: all dpki check check-retail check-dpki clean FORCE
FORCE:

# A failed step must not leave a partial ticket, TMD, or WAD behind.
.DELETE_ON_ERROR:
