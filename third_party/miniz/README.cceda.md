# miniz dependency

Upstream: https://github.com/richgel999/miniz
Version: 3.1.2
Commit: 77d0dce8627735138c51770d1799a1ef48f2117d
License: MIT; see LICENSE and source notices.

Source files are unmodified, downloaded from that commit. CCEDA supplies only
miniz_export.h for static linking. CMake builds with MINIZ_NO_STDIO and
MINIZ_NO_ZLIB_COMPATIBLE_NAMES. All archive I/O goes through CCEDA callbacks;
miniz filesystem extraction APIs are disabled. No network is required to build.
