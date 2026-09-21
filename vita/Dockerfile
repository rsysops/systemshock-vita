# The plain (full) vitasdk/vitasdk tag already ships every prebuilt package
# this project links against (sdl2, sdl2_mixer, libvita2d, libjpeg-turbo,
# libpng, libwebp, zlib, libvorbis, libogg, libmikmod, libmodplug,
# libxmp-lite, opusfile, opus, flac, mpg123 -- see CMakeLists.txt VITA_LIBS).
# Re-installing any of them with vdpm just churns pacman's conflict resolver
# (e.g. sdl2 vs. the also-preinstalled sdl2_vitagl), so this image does
# nothing beyond pinning the base and setting the workdir the source gets
# bind-mounted into.
FROM vitasdk/vitasdk:2026.08

WORKDIR /workspace
