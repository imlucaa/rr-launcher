# Dockerfile for the bare environment RR needs (dkp tools & necessary libraries installed into /opt). Used in CI

# Pinned to the last image before libogc 3.0 (Jan 2026), which removed gettime/diff_msec that update.c, shutdown.c and wii-mbedtls still use.
FROM devkitpro/devkitppc:20251231

COPY install-libs.sh ./

RUN ./install-libs.sh
