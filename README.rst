Harpia Recorder
===============

A screen recorder and video editor built on libobs, the capture and encoding
core of OBS Studio <https://obsproject.com>.

This tree is OBS Studio slimmed down to what the recorder needs: libobs, its
graphics back-ends, and a minimal set of plugins (screen and audio capture,
webcam, the H.264/H.265/AV1 encoders, the ffmpeg muxer and the crop filter).
The OBS Studio frontend, scripting, streaming stack, other plugins and
translations have been removed, so stock OBS Studio no longer builds from it.

The recorder itself lives in ``harpia/`` -- see ``harpia/README.md``.

Like OBS Studio, it is distributed under the GNU General Public License v2
(or any later version) -- see the accompanying COPYING file. libobs and the
plugins are the work of the OBS Studio contributors (see AUTHORS).
