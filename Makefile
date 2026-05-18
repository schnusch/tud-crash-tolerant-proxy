.PHONY: clean

notes.html: NOTES.md
	{ echo '<style>'; \
	  echo '  h1, h2, h3, h4, h5, h6 { break-after: avoid; }'; \
	  echo '  ul { padding-left: 1em; }'; \
	  echo '  p, pre { margin: 0; }'; \
	  echo '  blockquote { margin: 0; padding-left: 0.5em; border-left: 0.25em solid lightgray; }'; \
	  echo '  img { max-width: 100%; }'; \
	  echo '  @media print {'; \
	  echo '    body { font-size: 11pt; column-count: 2; column-fill: auto; }'; \
	  echo '  }'; \
	  echo '</style>'; \
	  echo; \
	  cat NOTES.md; \
	} | pandoc -o $@ -f markdown+gfm_auto_identifiers --lua-filter=contrib/pandoc-ext/diagram/diagram.lua --embed-resources

clean:
	$(RM) notes.html
