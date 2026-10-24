# Social video editing

## Trim silence

In **Full Editing**, open **Tools → Trim silence…**. The review window analyzes
audio and lists quiet ranges to remove. Uncheck any pause you want to keep.

- **Analyze selected clips only** uses the selected video/audio clips to find
  pauses, so background music on another track does not hide speech gaps.
- **Quiet threshold** defaults to −40 dB. Lower it to keep quieter speech.
- **Minimum pause** defaults to 0.50 seconds.
- **Keep pause** defaults to 0.15 seconds, preserving room around speech.
- Leading and trailing silence is kept unless its option is enabled.

**Preview trimmed timeline** plays the proposed result in the main preview.
Stopping, changing the reviewed cuts, or canceling restores the original model,
selection, and playhead. **Apply** removes the checked time ranges across every
track together. One Undo restores the whole edit. Original media files are
never rewritten.

The review explains edits it cannot preserve exactly, including affected locked
tracks, transition overlaps, editable generated narrations, certain time-driven
components/scripts, and cuts inside active fades. Those edits remain unapplied;
adjust the selected pauses or the affected clip first.

## Social safe areas

Choose **View → Social safe areas → TikTok / Instagram Reels / YouTube Shorts**.
Shaded margins estimate where platform controls and descriptions may cover the
picture. Keep important text and faces inside the dashed region.

The guides follow the output canvas and are remembered as a view preference.
They never appear in exported video. Use **Off** to hide them. Platform layouts
vary by device and change over time, so these are approximate composition guides.

## Animated captions

Select text clips and use **Inspector → Text → Animation**:

- **Whole phrase** keeps the caption static.
- **One word at a time** and **Build up word by word** retain the existing reveal modes.
- **Highlight current word** colors the word being spoken.
- **Highlight spoken words** progressively colors the phrase.

**Highlight colour** controls the animated color. The existing text-color toolbar
can give individual keywords their own colors; those return when a moving
highlight passes. **Subtle pop on each word** adds a short zoom pulse. Its
strength and duration can be adjusted in the Caption Pop component.

Generated subtitles follow their spoken word timestamps. Typed text uses an even
pace; editing a caption to a different word count redistributes timing over its
spoken span. Caption text, alternatives, keyframes, and timing data stay editable.
The controls apply to selected text clips or a selected caption lane, with each
choice recorded as one undo action. Preview and export use the same renderer.
