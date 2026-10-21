FROM ghcr.io/caiiiycuk/coturn:4.18.0-3

USER root

RUN apt-get update && \
    apt-get install -y --no-install-recommends gettext-base curl openssl python3 && \
    rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
COPY publisher.py /usr/local/bin/publisher.py
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
