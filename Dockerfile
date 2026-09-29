FROM gcc:13 AS builder

WORKDIR /src
COPY runtime.c .
RUN gcc -Wall -Wextra -O2 -std=gnu11 -static -o runtime runtime.c


FROM alpine:3.19
COPY --from=builder /src/runtime /usr/local/bin/runtime

RUN mkdir -p /opt/alpine-rootfs && \
    for d in bin etc home lib media mnt opt root run sbin srv sys tmp usr var; do \
        if [ -d "/$d" ]; then cp -a "/$d" /opt/alpine-rootfs/ 2>/dev/null || true; fi; \
    done && \
    mkdir -p /opt/alpine-rootfs/proc /opt/alpine-rootfs/dev

ENTRYPOINT ["/usr/local/bin/runtime"]