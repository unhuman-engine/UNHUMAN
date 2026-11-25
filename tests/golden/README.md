# Golden images

Reference frames for the visual regression step in
`.github/workflows/test-renderer.yml` ("Golden image (Sandbox, lavapipe)").

| File | Scene |
|---|---|
| `sandbox_box.png` | Sandbox, frame 30 at a fixed 1/60 s step, 1920x1080, lavapipe. Three FrameGraph passes: the scene into an RGBA16F target, ACES tonemap into the backbuffer, then the UI |

CI renders the same frame headless on lavapipe and compares it with
`uhe_imgdiff` (`tools/imgdiff`). A pixel differs when any channel is more
than 8 apart, and the step fails when more than 0.1% of pixels differ.

## When the step fails

Download the `golden-image` artifact from the run:

- `actual.png`: what the commit renders.
- `diff.png`: differing pixels in red over a dimmed copy of the reference.
- `sandbox.log`: the engine's log for the run.

If the change is a regression, fix the code. If the new image is intended
(a shader, lighting or UI change), check `actual.png` by eye and commit it
over the reference in the same PR, saying why in the commit.

The reference must come from CI's lavapipe, not a local GPU: drivers
rasterise and round differently, so a frame from another GPU fails the
comparison even though nothing is wrong.

## Rendering the frame locally

```sh
UHE_FIXED_TIMESTEP=0.016666667 UHE_CAPTURE_FRAME=30 UHE_CAPTURE_PATH=frame.png \
  bin/Debug-linux-x64/Sandbox
bin/Debug-linux-x64/uhe_imgdiff tests/golden/sandbox_box.png frame.png --diff diff.png
```
