FROM ghcr.io/caiiiycuk/coturn:4.18.0-2

USER root

RUN apt-get update && \
    apt-get install -y --no-install-recommends gettext-base curl openssl && \
    rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
