### grammar v27: message photos show on the glass, and a tap opens them full screen

A thread whose messages carry photos now draws each photo as a thumbnail under its message (black-and-white dither on the e-ink, four grays on the colour boards). Tap a thumbnail and the photo fills the glass; tap it again, or press UP/DOWN, and the thread comes back where you left it. Photos are fetched only from the tiny.technology media store and decoded on the device (baseline JPEG, up to 8 per card); one that cannot be shown says "photo unavailable". A photo-only message reads "photo", other attachments are named, and an inbox row whose last message was a photo reads "[photo]".

Card grammar: a `list` item may be `{"text","images":[url]}`, a new `photo` card shows one photo, and tap ids `p:<n>` / `p:back` drive the viewer. The owner dashboard's Messages card shows the same photos inline, with a full-screen lightbox on click. Firmware 0.33.0.
