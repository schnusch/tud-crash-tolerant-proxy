NAME = crash-tolerant-proxy
SRCS = $(wildcard [0-9][0123456789]_*)
FILTERS = $(wildcard pandoc/*.lua)
HTML_ONLY_SRCS = \
	head.html
TEX_ONLY_SRCS = \
	head.tex \
	cd-hack.tex \
	declarations.tex \
	noto.tex \
	tikz/include/head.tex
TIKZ_SRCS = $(wildcard tikz/*.tex)
TIKZ_SVGS = $(patsubst %.tex,out/$(NAME)/%.svg,$(TIKZ_SRCS))
PDF_ATTACHMENTS = \
	tmp/tud-crash-tolerant-proxy.tar.gz

.PHONY: all pdf html clean

all: html pdf

clean:
	$(RM) -r out tmp

html: $(TIKZ_SVGS) out/$(NAME)/index.html

pdf: out/$(NAME).pdf

# final PDF

out/$(NAME).pdf: tmp/tex/$(NAME).pdf $(PDF_ATTACHMENTS)
	@mkdir -p $(@D)
	qpdf $< $(patsubst %,--add-attachment % --,$(PDF_ATTACHMENTS)) --linearize $@

tmp/tex/$(NAME).pdf: tmp/tex/$(NAME).tex $(patsubst %,tmp/html/%,$(TIKZ_SRCS))
	latexmk -g -xelatex -output-directory=$(@D) -jobname=$(@F:.pdf=) $< < /dev/null

# PDF attachments

tmp/tud-crash-tolerant-proxy.tar.gz:
	scp -p 141.76.48.110:$(@F) $@.tmp
	mv $@.tmp $@

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

tmp/tex/$(NAME).tex: $(TEX_ONLY_SRCS) defaults.yaml ieee.csl citations.bib $(FILTERS) $(SRCS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)

out/$(NAME)/index.html: $(HTML_ONLY_SRCS) defaults.yaml ieee.csl citations.bib $(FILTERS) $(SRCS)
	@mkdir -p $(@D)
	$(PANDOC_CMD)

# misc

ieee.csl:
	wget -O $@.tmp https://pandoc.org/demo/ieee.csl || curl -Lfo $@.tmp https://pandoc.org/demo/ieee.csl
	echo '9b023e1b62d7459fefe7f989fbc6be1d54638952ab2581111b5895de0aa444e1  $@.tmp' | sha256sum -c
	mv $@.tmp $@

# standalone TikZ images

out/$(NAME)/tikz/%.svg: tmp/html/tikz/%.xdv
	@mkdir -p $(@D)
	dvisvgm -o $@ --font-format=woff2,autohint $<
tmp/html/tikz/%.xdv: tmp/html/tikz/%.tex
	latexmk -g -xdv -output-directory=$(@D) -jobname=$(@F:.xdv=) $< < /dev/null

tmp/html/tikz/%.tex: tikz/%.tex tikz/include/head.tex noto.tex
	ls -dhlp $^
	@mkdir -p $(@D)
	{ echo '\def\pgfsysdriver{pgfsys-dvips.def}'; \
	  echo '\documentclass[11pt,tikz]{standalone}'; \
	  cat noto.tex tikz/include/head.tex; \
	  echo '\usepackage[hidelinks]{hyperref}'; \
	  echo '\begin{document}'; \
	  cat $<; \
	  echo '\end{document}'; \
	} > $@.tmp
	mv $@.tmp $@

# byte latency

tmp/html/tikz/fail-accept.tex: data/fail-accept-data.csv

tmp/html/tikz/fail-recv.tex: data/fail-recv-data.csv

tmp/html/tikz/first-byte.tex: \
		data/first-haproxy-data.csv \
		data/first-baseline-data.csv \
		data/first-proxy-data.csv

data/%-data.csv: data/%.csv tools/median.py
	./tools/median.py < $(@:-data.csv=.csv) > $@.tmp 3> $(@:-data.csv=-median.tex)
	mv $@.tmp $@

data/fail-accept.csv \
data/fail-recv.csv \
data/first-haproxy.csv \
data/first-baseline.csv \
data/first-proxy.csv:
	@mkdir -p $(@D)
	scp -p 141.76.48.110:tud-crash-tolerant-proxy/benchmark/$(@F) $@.tmp
	mv $@.tmp $@

# vegeta

tmp/mk/plots.mk: GNUmakefile
	@mkdir -p $(@D)
	{ echo nginx    transform_nop       green  120; \
	  echo haproxy  transform_nop       orange 120; \
	  echo baseline transform_nop       blue   120; \
	  echo proxy    transform_nop       red    120; \
	  echo haproxy  transform_headers   orange 120; \
	  echo baseline transform_headers   blue   120; \
	  echo proxy    transform_headers   red    120; \
	  echo baseline transform_expensive blue   600; \
	  echo proxy    transform_expensive red    600; \
	} | while read host xfrm color duration; do \
	  for parallel in 1 10 100 1000; do \
	    for size in 1K 1M 256M; do \
	      src="vegeta/$${xfrm}/$${host}/$${size}-$${parallel}c-$${duration}s"; \
	      bin="vegeta/$${xfrm}/$${src##*/}-bins.csv"; \
	      echo "download: ../benchmark/$${src}.csv.gz"; \
	      echo "../benchmark/$${src}.csv.gz:"; \
	      echo "	@mkdir -p \$$(@D)"; \
	      echo "	scp -p 141.76.48.110:tud-crash-tolerant-proxy/benchmark/$${src}.csv.gz \$$@.tmp"; \
	      echo "	mv \$$@.tmp \$$@"; \
	      [ -e "../benchmark/$${src}.csv.gz" ] || { echo "../benchmark/$${src}.csv.gz does not exist" >&2; : continue; }; \
	      echo "data/$${src}-slice.csv.gz: ../benchmark/$${src}.csv.gz"; \
	      echo "	@mkdir -p \$$(@D)"; \
	      echo "	gzip -d < '../benchmark/$${src}.csv.gz' \\"; \
	      echo "	| ./tools/slice.py latency_ns \\"; \
	      echo "	| gzip > \$$@.tmp"; \
	      echo "	mv \$$@.tmp \$$@"; \
	      echo "data/$${src}-cumu.csv \\"; \
	      echo "data/$${src}.tex: \\"; \
	      echo "		data/$${src}-slice.csv.gz \\"; \
	      echo "		tools/histogram.py"; \
	      echo "	@mkdir -p \$$(@D)"; \
	      echo "	./tools/histogram.py --color='$${color}' --downsample=1000 --multiply=0.000001 \\"; \
	      echo "		-H 'data/$${src}-cumu.csv' \\"; \
	      echo "		-Q 'data/$${src}.tex' \\"; \
	      echo "		latency_ns \\"; \
	      echo "		'data/$${src}-slice.csv.gz'"; \
	      echo "#	gzip -d < data/$${src}-slice.csv.gz \\"; \
	      echo "#	| wc -l \\"; \
	      echo "#		- \\"; \
	      echo "#		'data/$${src}-cumu.csv' \\"; \
	      echo "#		'data/$${src}.tex'"; \
	      echo "tmp/html/tikz/vegeta-$${xfrm}-$${size}-$${parallel}c.tex: \\"; \
	      echo "		tikz/include/vegeta.tex \\"; \
	      echo "		data/$${src}.tex \\"; \
	      echo "		data/$${src}-cumu.csv \\"; \
	      echo "		data/$${bin}"; \
	      echo "data/$${bin}: data/$${src}-slice.csv.gz"; \
	      [ "$${host}" = proxy ] || printf '#'; \
	      echo "data/$${bin}:"; \
	      [ "$${host}" = proxy ] || printf '#'; \
	      echo "	./tools/bins.py --multiply=0.000001 -o \$$@ -c latency_ns \$$(foreach foo,\$$^,\$$(shell basename \`dirname \$$(foo)\`):\$$(foo))"; \
	      echo; \
	    done; \
	  done; \
	done > $@.tmp
	mv $@.tmp $@
ifneq ($(MAKECMDGOALS),clean)
-include tmp/mk/plots.mk
endif
