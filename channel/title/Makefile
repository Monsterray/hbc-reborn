CHANNEL_VERSION = 1.6.2

TOOLS = ../../pywii/pywii-tools
PYWII = ../../pywii/Common/pywii/wii.py
PYTHON ?= python3
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

retail/cetk: cetk.template Makefile $(TOOLS)/tikfix.py $(PYWII)
	@cp $< $@
	@"$(PYTHON)" "$(TOOLS)/tikfix.py" $@

dpki/cetk: retail/cetk Makefile $(TOOLS)/dpkisign.py $(PYWII)
	@"$(PYTHON)" "$(TOOLS)/dpkisign.py" -cetk $< $@ "$(CERTS)" $(DPKI_ISSUER_TIK)

retail/tmd: tmd.template 00000000 00000001 Makefile $(TOOLS)/tmdupdatecr.py $(TOOLS)/tmdvers.py $(PYWII)
	@cp $< $@
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
	rm -f retail/cetk dpki/cetk retail/tmd retail/tmd 00000000 00000001 channel_retail.wad channel_dpki.wad

.PHONY: all dpki check check-retail check-dpki clean

# A failed step must not leave a partial ticket, TMD, or WAD behind.
.DELETE_ON_ERROR:
