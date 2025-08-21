FROM debian:bookworm-slim

RUN apt-get -y update && apt-get -y install \
	build-essential \
	gcc-arm-none-eabi \
	libc6-dev \
	cmake

COPY docker_build.sh /build.sh
RUN chmod +x /build.sh

ENTRYPOINT ["/bin/bash", "/build.sh"]
