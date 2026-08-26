# native_sim end-to-end stream test

Builds the outpost into a small Zephyr application whose *only* stdout writer
is the outpost UART, runs it, and decodes the captured bytes with
`scripts/decode_outpost.py` against the `outpost-manifest.json` the same build
produced.

    ./run.sh

Asserts, rather than reports: a header frame decodes; every frame passes its
CRC; every record kind the build has enabled appears; the manifest resolves
real thread and marker names; and a manifest with the wrong `build_id` is
refused rather than applied.
