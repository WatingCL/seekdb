FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/
RUN make -C /src/minivectordb

RUN mkdir -p /data
VOLUME ["/data"]

ENTRYPOINT ["/src/minivectordb/minivectordb"]
CMD ["--data-dir", "/data"]
