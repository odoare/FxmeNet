# FxmeNet user manual

One LaTeX source, two outputs: a PDF, and a multi-page HTML site to put on
fx-mechanics.com. This is a draft: every gap is marked `[TODO: ...]` in the
text (red in the PDF, yellow in the HTML).

## Building

Needs a TeX Live with `latexmk`, `pdflatex` and `make4ht` (package `tex4ht`),
plus the packages `koma-script`, `tcolorbox`, `listings`, `booktabs`,
`microtype` and `xstring` (all in TeX Live's usual collections).

```
make pdf     # build/fxmenet-manual.pdf
make html    # build/html/index.html and the pages beside it
make all
make clean
```

For the site, copy the whole `build/html` folder. The pages are named
`indexch1.html`, `indexch2.html`... (tex4ht's naming; the first page is
`index.html`), the images and `fxmenet.css` are beside them, and every link
is relative, so the folder can sit at any address.

## Layout

```
fxmenet-manual.tex   main file: title page, the list of chapters
fxmenet.sty          style shared by both outputs (colours, call-outs, \todo)
fxmenet.cfg          make4ht settings (HTML only)
fxmenet.css          the site's style (HTML only)
chapters/            one file per chapter, 01 to 13
```

The logos are `../FxmeNetLogo3.jpg` and `../fxmenet-logo.png` (the doc
folder). They are copied beside the HTML pages by `make html`.

## Writing

- Plain prose; parentheses rather than dashes; no `--` in prose, because the
  HTML would turn it into an en dash.
- `\fxme` for the name, `\cmd{...}` for commands and options, `\file{...}`
  for file names (it breaks lines at slashes; not for titles), `\ui{...}` for
  a control of the editor, `\key{...}` for a key, `\tier{0}` for tier tags.
- Call-outs: `note`, `tip` and `warning` environments.
- `\todo{...}` marks a gap. `\fxdraftfalse` in `fxmenet.sty` hides them all
  for a release.
- Chapters are numbered, including the last three (Reference, Network
  requirements, Glossary): tex4ht gives wrong numbers to the targets of
  references in LaTeX appendices, so there is no `\appendix`.
- Cross-references with `\ref{ch:...}` and `\ref{sec:...}`.

## Still to do

- Screenshots of the plugin window, the Routing tab and `fxmenet-top`
  (the places are marked), saved in `doc/manual/images/` (already on the
  graphics path).
- The final FxmeNet compatible mark (chapter 9): the two logos in `doc` are
  drafts, the JPEG has an opaque background; a transparent PNG and an SVG
  are needed, with the conditions of use.
- Installation: release packages, where the plugin and server go on each
  system, signing and notarization on macOS, the server as a service.
- Check every statement marked `[TODO]`, and have someone who has not used
  FxmeNet follow chapter 2.
- Nicer page names for the site, and a search box or a site menu, if wanted.
