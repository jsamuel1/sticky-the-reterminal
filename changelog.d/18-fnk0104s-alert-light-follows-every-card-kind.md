### firmware: the FNK0104S alert light follows image, gallery and universe cards

The magenta alert light now stays correct for `image`, `gallery` and `universe`
cards, which used to clear it. It is decided at each commit under the display
lock, and a gallery page flip or universe repaint keeps it only while the same
page is on glass.

A universe push is one request: the merged roster is saved as one write after
the panel takes the page, and if that write is refused the push fails and the
page is rolled back. A malformed `tinys` is refused whole before anything is
claimed. The agent slug and roster persist as one NVS blob, adopted in RAM only
after flash accepted it; an outcome-unknown write is read back and reported.

A render that a newer card overtook returns `TINY_DISPLAY_SUPERSEDED`, so an
overtaken gallery page flip no longer paints "No photos here yet" over the
newer card.
