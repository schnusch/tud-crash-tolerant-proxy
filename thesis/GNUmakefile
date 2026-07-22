NAME = crash-tolerant-proxy
SRCS = $(wildcard [0-9][0123456789]_*)
FILTERS = $(wildcard pandoc/*.lua)
HTML_ONLY_SRCS = \
	head.html
TEX_ONLY_SRCS = \
	head.tex \
	cd-hack.tex \
	declarations.tex \
	tikz/head.tex
TIKZ_SRCS = $(filter-out tikz/head.tex,$(wildcard tikz/*.tex))
TIKZ_SVGS = $(patsubst %.tex,out/$(NAME)/%.svg,$(TIKZ_SRCS))
PDF_ATTACHMENTS = \
	tmp/tex/$(NAME)-html.tar.gz \
	tmp/tex/$(NAME)-source.tar.gz

.PHONY: all pdf html clean

all: html pdf

clean:
	$(RM) -r out tmp

html: out/$(NAME)/index.html

pdf: out/$(NAME).pdf

# final PDF

out/$(NAME).pdf: tmp/tex/$(NAME).pdf $(PDF_ATTACHMENTS)
	@mkdir -p $(@D)
	qpdf $< $(patsubst %,--add-attachment % --,$(PDF_ATTACHMENTS)) --linearize $@

tmp/tex/$(NAME).pdf: tmp/tex/$(NAME).tex $(TIKZ_SRCS)
	latexmk -g -xelatex -output-directory=$(@D) -jobname=$(@F:.pdf=) $< < /dev/null

# PDF attachments

TAR_CMD = tar --create --dereference --owner=root:0 --group=root:0 --file=$(@:.gz=.tmp)
GZ_CMD = (zopfli $(@:.gz=.tmp) || gzip -9 $(@:.gz=.tmp)) && mv $(@:.gz=.tmp.gz) $@

tmp/tex/$(NAME)-html.tar.gz: out/$(NAME)/index.html
	$(TAR_CMD) --transform='s@^[^/]*/[^/]*@$(NAME)-html@' -- $(<D)
	@mkdir -p $(@D)
	$(GZ_CMD)

tmp/tex/$(NAME)-source.tar.gz: $(sort GNUmakefile defaults.yaml $(FILTERS) $(SRCS) $(HTML_ONLY_SRCS) $(TEX_ONLY_SRCS) $(TIKZ_SRCS))
	$(TAR_CMD) --transform='s@^@$(NAME)-source/@' -- $^
	@mkdir -p $(@D)
	$(GZ_CMD)

# Pandoc conversion

PANDOC_CMD = \
	printf '%s\n' --- "date: $$(date +%F)" ... \
	| pandoc \
		--standalone \
		--embed-resources \
		--output=$@ \
		--defaults=defaults.yaml \
		--bibliography=citations.bib \
		$(patsubst %,--lua-filter=%,$(FILTERS)) \
		--include-in-header=$< \
		- $(SRCS)

tmp/tex/$(NAME).tex: $(TEX_ONLY_SRCS) defaults.yaml citations.bib $(FILTERS) $(SRCS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)

out/$(NAME)/index.html: $(HTML_ONLY_SRCS) defaults.yaml citations.bib $(FILTERS) $(SRCS) $(TIKZ_SVGS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)

# standalone TikZ images

out/$(NAME)/tikz/%.svg: tmp/html/tikz/%.xdv
	@mkdir -p $(@D)
	dvisvgm -o $@ --font-format=woff2,autohint $<
tmp/html/tikz/%.xdv: tmp/html/tikz/%.tex
	latexmk -g -xdv -output-directory=$(@D) -jobname=$(@F:.xdv=) $< < /dev/null

tmp/html/tikz/%.tex: tikz/%.tex tikz/head.tex noto.tex
	@mkdir -p $(@D)
	{ echo '\def\pgfsysdriver{pgfsys-dvips.def}'; \
	  echo '\documentclass[11pt,tikz]{standalone}'; \
	  cat tikz/head.tex noto.tex; \
	  echo '\begin{document}'; \
	  cat $<; \
	  echo '\end{document}'; \
	} > $@.tmp
	mv $@.tmp $@
