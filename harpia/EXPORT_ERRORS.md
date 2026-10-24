# Timeline export errors

When export fails, the dialog shows an error code and a reason. Use **Show
Details** for the full report or **Copy details** to copy it. Reports are also
saved in the session log, available from **Help → Error logs**.

The report includes the output frame and timeline time, canvas resolution and
frame rate, memory required for one RGBA canvas, and the number of open video
decoders and loaded stills. Effect failures also identify the track, clip and
component when available. Track and clip numbers start at one.

| Code | What happened |
| --- | --- |
| `TL-CANVAS-SIZE` | The render canvas has invalid dimensions. |
| `TL-CANVAS-ALLOC` | Qt could not allocate the main canvas image. Available memory may be insufficient, or the dimensions may exceed image limits. |
| `TL-PAINTER-INIT` | Qt could not begin or resume drawing into an image. |
| `TL-EFFECT-ALLOC` | An image copy needed by an effect could not be allocated. |
| `TL-EFFECT-FRAME` | A Pixel or Composite component returned an empty image, or its fade processing failed. The reason names the component type and instance. |
| `TL-TRANSITION-ALLOC` | One of the two transition layers could not be allocated. |
| `TL-TRANSITION-FRAME` | Blending the transition layers returned an empty image. |
| `TL-RENDER-ALLOC` | The color-conversion context, encoder frame or packet could not be allocated. |
| `TL-ENCODER-ALLOC` | FFmpeg could not allocate the encoder image; its error code and text are included. |
| `TL-SHADER-FRAME` | The post-processing shader returned an empty image. |
| `TL-RGBA-CONVERT` | Converting the rendered image to RGBA pixels failed. |
| `TL-FRAME-EMPTY` | The renderer returned an empty image without a more specific reason. |

For allocation errors, try reducing the **project canvas resolution** or closing
other applications that use significant memory. One 3840 × 2160 RGBA image
needs about 31.6 MiB; effects, transitions, decoded sources and the encoder need
additional buffers. Choosing a smaller export size alone does not shrink the
project canvas used for composition.

For effect failures, try disabling the named effect and exporting again. An
empty effect image can indicate an allocation failure, an image-conversion
failure or a component problem; the code identifies where rendering stopped,
not an unverified system-level cause.

The older message, “Could not compose the timeline frame,” did not retain the
failed stage. It cannot establish the cause of an earlier failure by itself.
Missing source frames have separate behavior and do not explain that specific
message: they were skipped by the compositor. These diagnostics do not change
that existing behavior or the GPU fallback behavior.
