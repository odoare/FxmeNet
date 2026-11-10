# The FxmeNet manual. See README.md.
#
#   make pdf     build/fxmenet-manual.pdf
#   make html    build/html/index.html (and the pages beside it)
#   make clean   remove build/

MAIN  = fxmenet-manual
BUILD = build
IMAGES = ../FxmeNetLogo3.jpg ../fxmenet-logo.png
SRC   = $(MAIN).tex fxmenet.sty $(wildcard chapters/*.tex)

.PHONY: all pdf html clean

all: pdf html

pdf: $(BUILD)/$(MAIN).pdf

$(BUILD)/$(MAIN).pdf: $(SRC)
	latexmk -pdf -interaction=nonstopmode -halt-on-error -outdir=$(BUILD) $(MAIN).tex

html: $(BUILD)/html/index.html

$(BUILD)/html/index.html: $(SRC) fxmenet.cfg fxmenet.css
	mkdir -p $(BUILD)/html
	TEXINPUTS=..:$$TEXINPUTS: make4ht -u -f html5 -d $(BUILD)/html -c fxmenet.cfg -j index $(MAIN).tex "html5,2"
	cp fxmenet.css $(IMAGES) $(BUILD)/html/
	sed -i "s/alt='PIC'\\(.*\\)src='FxmeNetLogo3.jpg'/alt='FxmeNet logo'\\1src='FxmeNetLogo3.jpg'/; s/alt='PIC'\\(.*\\)src='fxmenet-logo.png'/alt='FxmeNet icon'\\1src='fxmenet-logo.png'/" $(BUILD)/html/*.html
	# make4ht leaves its working files beside the source: remove them.
	rm -f index.4ct index.4tc index.aux index.css index.dvi index.html index.idv index.lg index.log index.tmp index.xref
	rm -f indexap?.html indexch?.html indexch??.html indexli?.html

clean:
	rm -rf $(BUILD)
