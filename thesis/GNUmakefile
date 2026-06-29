NAME = crash-tolerant-proxy
SRCS = $(wildcard [0-9][0123456789]_*)
FILTERS = $(wildcard pandoc/*.lua)
HTML_ONLY_SRCS = \
	head.html
TEX_ONLY_SRCS = \
	head.tex \
	cd-hack.tex \
	declarations.tex
PDF_ATTACHMENTS = \
	tmp/$(NAME)-html.tar.gz \
	tmp/$(NAME)-source.tar.gz

.PHONY: all pdf html clean

all: html pdf

clean:
	$(RM) -r out tmp

html: out/$(NAME)/index.html

pdf: out/$(NAME).pdf

# final PDF

out/$(NAME).pdf: tmp/$(NAME).pdf $(PDF_ATTACHMENTS)
	@mkdir -p $(@D)
	qpdf $< $(patsubst %,--add-attachment % --,$(PDF_ATTACHMENTS)) --linearize $@

tmp/$(NAME).pdf: tmp/$(NAME).tex
	latexmk -g -xelatex -output-directory=$(@D) -jobname=$(@F:.pdf=) $< < /dev/null

# PDF attachments

TAR_CMD = tar --create --dereference --owner=root:0 --group=root:0 --file=$(@:.gz=.tmp)
GZ_CMD = (zopfli $(@:.gz=.tmp) || gzip -9 $(@:.gz=.tmp)) && mv $(@:.gz=.tmp.gz) $@

tmp/$(NAME)-html.tar.gz: out/$(NAME)/index.html
	$(TAR_CMD) --transform='s@^[^/]*/[^/]*@$(NAME)-html@' -- $(<D)
	@mkdir -p $(@D)
	$(GZ_CMD)

tmp/$(NAME)-source.tar.gz: $(sort GNUmakefile defaults.yaml $(FILTERS) $(SRCS) $(HTML_ONLY_SRCS) $(TEX_ONLY_SRCS))
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
		$(patsubst %,--lua-filter=%,$(FILTERS)) \
		--include-in-header=$< \
		- $(SRCS)

tmp/$(NAME).tex: $(TEX_ONLY_SRCS) defaults.yaml $(FILTERS) $(SRCS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)

out/$(NAME)/index.html: $(HTML_ONLY_SRCS) defaults.yaml $(FILTERS) $(SRCS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)
