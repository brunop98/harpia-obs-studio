# Application version

For every meaningful Harpia application change, increment `HARPIA_VERSION_PATCH`
in `Version.hpp` before committing. This includes features and bug fixes; pure
documentation changes do not need a version bump.

Keep `Version.hpp` as the single source of truth. Window labels, About, startup
logs, Qt application metadata and Windows file/product versions must agree.
Windows resource versions are generated from this header at build time; do not
maintain a separate version in the resource file or build scripts.
