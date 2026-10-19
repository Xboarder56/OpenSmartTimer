# ost-relay/1 reference relay
#
# Multi-stage build. The runtime image runs as a non-root user, mounts
# the SQLite database at /data, and expects TLS termination in front
# (nginx / Caddy — see OPERATOR.md § TLS termination).

# ─── builder ─────────────────────────────────────────────────────────
FROM python:3.12-slim@sha256:78387bc3881b8273120a12ebe6c1ab22b018ccc2c9adf565ae1ac9b536e184ea AS builder
WORKDIR /build

COPY relay/pyproject.toml relay/requirements.lock relay/README.md ./
# Pinned install: use the requirements set for exact deps, then
# install the package itself for the entrypoint. Two-step so the
# constraint file is authoritative and setuptools does not resolve
# unpinned versions from pyproject dependencies.
RUN pip install --no-cache-dir --only-binary=:all: --require-hashes --prefix=/install -r requirements.lock
COPY relay/src/ ./src/
RUN pip install --no-cache-dir --prefix=/install --no-deps .

# ─── runtime ─────────────────────────────────────────────────────────
FROM python:3.12-slim@sha256:78387bc3881b8273120a12ebe6c1ab22b018ccc2c9adf565ae1ac9b536e184ea AS runtime
WORKDIR /opt/ostrelay

RUN useradd --system --home /opt/ostrelay --shell /usr/sbin/nologin ostrelay \
    && mkdir -p /data \
    && chown -R ostrelay:ostrelay /opt/ostrelay /data

COPY --from=builder /install /usr/local
COPY relay/spa/ /opt/ostrelay/spa/
COPY relay/ops/database.py /opt/ostrelay/ops/database.py
COPY LICENSE LICENSE-ESPRESSIF-EXCEPTION NOTICE THIRD_PARTY_NOTICES.md /opt/ostrelay/licenses/
COPY third-party/inventory.json third-party/source-inputs.json /opt/ostrelay/licenses/third-party/
COPY third-party/notices/ /opt/ostrelay/licenses/third-party/notices/

# Build-context permissions must not prevent the runtime user reading assets.
RUN find /opt/ostrelay/spa /opt/ostrelay/licenses -type d -exec chmod 755 {} + \
    && find /opt/ostrelay/spa /opt/ostrelay/licenses -type f -exec chmod 644 {} +

ENV OSTRELAY_DB_PATH=/data/ostrelay.sqlite \
    OSTRELAY_LISTEN_HOST=0.0.0.0 \
    OSTRELAY_LISTEN_PORT=8080 \
    OSTRELAY_SPA_DIR=/opt/ostrelay/spa \
    PYTHONUNBUFFERED=1

USER ostrelay
EXPOSE 8080
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
    CMD python -c "import urllib.request,sys; \
        r=urllib.request.urlopen('http://127.0.0.1:8080/api/v1/health', timeout=3); \
        sys.exit(0 if r.status==200 else 1)"

ENTRYPOINT ["ostrelay"]
