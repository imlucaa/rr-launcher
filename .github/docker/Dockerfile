# Dockerfile for the bare environment RR needs (dkp tools & necessary libraries installed into /opt). Used in CI

# Pinned: the 20261016 image dropped libogc's gettime/diff_msec, which update.c and wii-mbedtls still use.
FROM devkitpro/devkitppc:20260503

COPY install-libs.sh ./

RUN ./install-libs.sh
