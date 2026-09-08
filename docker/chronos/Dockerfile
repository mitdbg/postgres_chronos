# syntax=docker/dockerfile:1.7

ARG POSTGRES_BASE_IMAGE=postgres:19beta3-bookworm

FROM ${POSTGRES_BASE_IMAGE} AS builder

RUN set -eux; \
	apt-get update; \
	apt-get install -y --no-install-recommends \
		bison \
		build-essential \
		flex \
		gettext \
		libcurl4-openssl-dev \
		libicu-dev \
		libkrb5-dev \
		libldap2-dev \
		liblz4-dev \
		libnuma-dev \
		libpam0g-dev \
		libreadline-dev \
		libssl-dev \
		libsystemd-dev \
		liburing-dev \
		libxml2-dev \
		libxslt1-dev \
		libzstd-dev \
		meson \
		ninja-build \
		perl \
		pkg-config \
		python3 \
		uuid-dev \
		zlib1g-dev \
	; \
	rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/postgresql
COPY . .

ARG BUILD_JOBS
RUN set -eux; \
	jobs="${BUILD_JOBS:-$(nproc)}"; \
	meson setup /tmp/postgres-build \
		--buildtype=release \
		--prefix=/usr/local/chronos-postgres \
		--libdir=lib \
		-Dcassert=false \
		-Ddocs=disabled \
		-Dextra_version=-chronos \
		-Dgssapi=enabled \
		-Dicu=enabled \
		-Dldap=enabled \
		-Dlibcurl=enabled \
		-Dlibnuma=enabled \
		-Dliburing=enabled \
		-Dlibxml=enabled \
		-Dlibxslt=enabled \
		-Dllvm=disabled \
		-Dlz4=enabled \
		-Dpam=enabled \
		-Dplperl=disabled \
		-Dplpython=disabled \
		-Dpltcl=disabled \
		-Dssl=openssl \
		-Dsystemd=enabled \
		-Dtap_tests=disabled \
		-Duuid=e2fs \
		-Dzstd=enabled; \
	meson compile -C /tmp/postgres-build -j "$jobs"; \
	meson install -C /tmp/postgres-build --destdir /tmp/postgres-root --strip; \
	test -x /tmp/postgres-root/usr/local/chronos-postgres/bin/postgres

FROM ${POSTGRES_BASE_IMAGE}

RUN set -eux; \
	apt-get update; \
	apt-get install -y --no-install-recommends \
		libcurl4 \
		libicu72 \
		libkrb5-3 \
		libldap-2.5-0 \
		liblz4-1 \
		libnuma1 \
		libpam0g \
		libreadline8 \
		libssl3 \
		libsystemd0 \
		liburing2 \
		libuuid1 \
		libxml2 \
		libxslt1.1 \
		libzstd1 \
		zlib1g \
	; \
	rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/postgres-root/usr/local/chronos-postgres /usr/local/chronos-postgres

RUN set -eux; \
	printf '%s\n' '/usr/local/chronos-postgres/lib' > /etc/ld.so.conf.d/chronos-postgres.conf; \
	ldconfig; \
	sed -ri "s!^#?(listen_addresses)\\s*=\\s*\\S+.*!\\1 = '*'!" /usr/local/chronos-postgres/share/postgresql.conf.sample; \
	grep -F "listen_addresses = '*'" /usr/local/chronos-postgres/share/postgresql.conf.sample; \
	/usr/local/chronos-postgres/bin/postgres --version

ENV PATH=/usr/local/chronos-postgres/bin:$PATH
ENV PG_VERSION=19beta3-chronos

ARG VCS_REF=unknown
LABEL org.opencontainers.image.title="Chronos for PostgreSQL 19" \
	org.opencontainers.image.description="PostgreSQL 19 with database branching based on the Chronos interval-versioning design" \
	org.opencontainers.image.revision="$VCS_REF" \
	org.opencontainers.image.version="19beta3-chronos" \
	org.opencontainers.image.licenses="PostgreSQL" \
	org.opencontainers.image.base.name="docker.io/library/postgres:19beta3-bookworm"

# ENTRYPOINT, CMD, STOPSIGNAL, EXPOSE, PGDATA, and the data volume are inherited
# from the official PostgreSQL image.
