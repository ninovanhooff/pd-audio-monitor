# Choosing a format: what each one costs your music

A lower sample rate cuts off the top of the spectrum: 22.05 kHz keeps content up to about 11 kHz,
11.025 kHz up to about 5.5 kHz. The music that suffers is music whose character lives up high.

## 22.05 kHz: subtle, like FM radio

- Cymbals, hi-hats, ride, shakers and tambourine lose their sizzle and turn duller.
- The "air" on vocals and acoustic instruments goes. Sibilants stay clear but less crisp.
- Bright synths, noise risers and reverb tails lose some sparkle.

Most people only notice in a direct A/B comparison, mostly on headphones.

## 11.025 kHz: obvious, like AM radio or a phone call

- Consonants blur: "s" and "f" start to sound alike, and "t" and "k" lose their edge.
- Hi-hats become dull ticks; the snare loses its snap and keeps only its thud.
- Strings, brass and distorted guitar sound muffled, since their bite comes from upper harmonics.
- Chiptune and square-wave synths get rounded off, since much of their buzz is upper harmonics.

## The Playdate's upsampling

The Playdate brings lower rates back up to 44.1 kHz by linear interpolation
([`probes/playback-probe`](../probes/playback-probe/)). That leaves faint mirror images above the
cut-off, heard most on:

- clean high tones (flute, whistles, glockenspiel, bells, sine leads), which gain a faint
  out-of-key tone, a metallic shimmer;
- loud content close to the cut-off, especially at 11.025 kHz.

## What holds up even at 11.025 kHz

Bass lines and kick drums, the fundamental pitch of voices and instruments, piano and guitar in the
middle register, pads and ambient music, and lo-fi or deliberately dark mixes.

## Bit depth and codec

These degrade independently of the sample rate:

- **8-bit** adds hiss and grit in quiet passages, fades, piano decays and reverb tails, at any rate.
- **IMA ADPCM** has the most trouble with noisy, sharp high-frequency sounds like hi-hats and
  snares, where it adds roughness.
- **QOA** is not a Playdate format: it shows a game that decodes `.qoa` files itself. It takes less
  than ADPCM (about 3.3 bits a sample against 4). On synthetic signals it kept tones cleaner than ADPCM
  did and fell behind on quiet noise ([numbers](approaches.md#qoa)), so check reverb tails and fade-outs
  by ear. It has no code for zero, so digital silence turns into a faint ±1 LSB hiss, about −92 dBFS.

## Speaker or headphones

The Playdate's speaker is tiny and likely reproduces little at the top end, which would hide much of
the difference between 44.1 and 22.05 kHz. Headphones show it far more clearly.

## Hearing it

Material that shows the differences well: a track with busy hi-hats, a close-miked vocal with
strong "s" sounds, and anything with glockenspiel or flute.

- On the Playdate: switch formats with the d-pad in [`usb-stream`](../usb-stream/), or toggle
  against the bypass with Ⓐ in [`line-in-monitor`](../line-in-monitor/).
- On the Mac: render a file with `chain_render`, as in the preview command in the
  [README](../README.md#quick-start).
