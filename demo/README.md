# The demo page

A three-file web page to share as a **webpage share** (design §5.5) when showing Kelp to
someone: `index.html`, its `style.css` and `kelp.svg`. Point *Share… → Share webpage* at this
folder.

It is deliberately **self-contained**: no CDN, no webfont, no external image, not even a
protocol-relative URL. That is what a page shared this way can be — the preview serves a
received bundle offline, with `default-src 'self'` and no way to reach anywhere else (§5.1,
design §12.11), so a page that expects the internet arrives without it. Three separate files
are also on purpose: they prove that relative sub-resources (a stylesheet, an image) resolve
through the bundle's own objects.

It is not installed by the RPM and nothing in the app references it; it is a demo asset for
`src/tests/run-emu-share.sh` passes and for the phone. The page says what it is, so a recipient
reading it learns what just happened rather than being sold something.
