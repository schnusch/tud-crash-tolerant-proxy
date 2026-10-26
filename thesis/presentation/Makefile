TUD_BEAMER = https://github.com/tud-cd/tudcd-scr/releases/download/0.7.1/tudcdscr-sharelatex-beamer-0.7.1.zip

.PHONY: all clean present

all: main.pdf

clean:
	$(RM) -r main.pdf tmp demo-pres.tex .latexmkrc tex .unpacked

present: main.pdf
	pdfpc --duration=30 $<

main.pdf: main.tex .unpacked
	@mkdir -p $(@D)
	latexmk -g -output-directory=tmp -jobname=$(@F:.pdf=) $< < /dev/null
	mv tmp/$(@F) $@

.unpacked: tudcdscr-sharelatex-beamer-0.7.1.zip
	unzip -d $(@D) $<
	sed -e '/^\\setmonofont{NotoSansMono}\[$$/ a Scale=0.8,' -i tex/tudcdfonts.sty
	touch $@
tudcdscr-sharelatex-beamer-0.7.1.zip:
	wget -O $@.tmp $(TUD_BEAMER) || curl -Lfo $@.tmp $(TUD_BEAMER)
	echo 'db1624a75c37d5481029d90d587cfdf025ed645df0562113e6c377bbf74ddd9d  $@.tmp' | sha256sum -c
	mv $@.tmp $@
