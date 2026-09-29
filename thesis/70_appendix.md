```{=latex}
\printbibliography
\def\printbibliography{}%
```

# Appendix {.unnumbered .unlisted}

```{=latex}
\chead{\usekomafont{pagehead}Appendix}%
```

## No-op Transformation {.unnumbered .unlisted}

```{=latex}
\refstepcounter{chapter}%
```

### 1 KiB response {.unnumbered .unlisted}

![no-op, 1 KiB response, 1 concurrent connection](tikz/vegeta-transform_nop-1K-1c.tex)

![no-op, 1 KiB response, 10 concurrent connections](tikz/vegeta-transform_nop-1K-10c.tex)

![no-op, 1 KiB response, 100 concurrent connections](tikz/vegeta-transform_nop-1K-100c.tex){#nop-1K-100c}

![no-op, 1 KiB response, 1000 concurrent connections](tikz/vegeta-transform_nop-1K-1000c.tex)

```{=latex}
\clearpage
```

### 1 MiB response {.unnumbered .unlisted}

![no-op, 1 MiB response, 1 concurrent connection](tikz/vegeta-transform_nop-1M-1c.tex)

![no-op, 1 MiB response, 10 concurrent connections](tikz/vegeta-transform_nop-1M-10c.tex)

![no-op, 1 MiB response, 100 concurrent connections](tikz/vegeta-transform_nop-1M-100c.tex)

![no-op, 1 MiB response, 1000 concurrent connections](tikz/vegeta-transform_nop-1M-1000c.tex){#nop-1M-1000c}

```{=latex}
\clearpage
```

### 256 MiB {.unnumbered .unlisted}

![no-op, 256 MiB response, 1 concurrent connection](tikz/vegeta-transform_nop-256M-1c.tex)

![no-op, 256 MiB response, 10 concurrent connections](tikz/vegeta-transform_nop-256M-10c.tex)

![no-op, 256 MiB response, 100 concurrent connections](tikz/vegeta-transform_nop-256M-100c.tex)

## Static HTTP Header Transformation {.unnumbered .unlisted}

```{=latex}
\refstepcounter{chapter}%
```

### 1 KiB response {.unnumbered .unlisted}

![headers, 1 KiB response, 1 concurrent connection](tikz/vegeta-transform_headers-1K-1c.tex)

![headers, 1 KiB response, 10 concurrent connections](tikz/vegeta-transform_headers-1K-10c.tex)

![headers, 1 KiB response, 100 concurrent connections](tikz/vegeta-transform_headers-1K-100c.tex)

![headers, 1 KiB response, 1000 concurrent connections](tikz/vegeta-transform_headers-1K-1000c.tex)

```{=latex}
\clearpage
```

### 1 MiB response {.unnumbered .unlisted}

![headers, 1 MiB response, 1 concurrent connection](tikz/vegeta-transform_headers-1M-1c.tex)

![headers, 1 MiB response, 10 concurrent connections](tikz/vegeta-transform_headers-1M-10c.tex)

![headers, 1 MiB response, 100 concurrent connections](tikz/vegeta-transform_headers-1M-100c.tex)

![headers, 1 MiB response, 1000 concurrent connections](tikz/vegeta-transform_headers-1M-1000c.tex)

```{=latex}
\clearpage
```

### 256 MiB response {.unnumbered .unlisted}

![headers, 256 MiB response, 1 concurrent connection](tikz/vegeta-transform_headers-256M-1c.tex)

![headers, 256 MiB response, 10 concurrent connections](tikz/vegeta-transform_headers-256M-10c.tex)

![headers, 256 KiB response, 100 concurrent connections](tikz/vegeta-transform_headers-256M-100c.tex)

## Computationally Expensive Transformation {.unnumbered .unlisted}

```{=latex}
\refstepcounter{chapter}%
```

### 1 KiB response {.unnumbered .unlisted}

![SHA-256, 1 KiB response, 1 concurrent connection](tikz/vegeta-transform_expensive-1K-1c.tex)

![SHA-256, 1 KiB response, 10 concurrent connections](tikz/vegeta-transform_expensive-1K-10c.tex)

![SHA-256, 1 KiB response, 100 concurrent connections](tikz/vegeta-transform_expensive-1K-100c.tex)

```{=latex}
\clearpage
```

### 1 MiB response {.unnumbered .unlisted}

![SHA-256, 1 MiB response, 1 concurrent connection](tikz/vegeta-transform_expensive-1M-1c.tex)

![SHA-256, 1 MiB response, 10 concurrent connections](tikz/vegeta-transform_expensive-1M-10c.tex)

![SHA-256, 1 MiB response, 100 concurrent connections](tikz/vegeta-transform_expensive-1M-100c.tex)

```{=latex}
\clearpage
```

### 256 MiB response {.unnumbered .unlisted}

![SHA-256, 256 MiB response, 1 concurrent connection](tikz/vegeta-transform_expensive-256M-1c.tex)

![SHA-256, 256 MiB response, 10 concurrent connections](tikz/vegeta-transform_expensive-256M-10c.tex)

![SHA-256, 256 MiB response, 100 concurrent connections](tikz/vegeta-transform_expensive-256M-100c.tex)
