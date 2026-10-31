# syntax=docker/dockerfile:1
#
# HOW TO RUN (from the repository root)
#   ./docker-run.sh build
#   docker build -t ghcr.io/ucrparlay/pab-tree:latest .    # the same, by hand
#
# Environment for the PaB-tree artifact evaluation (PPoPP 2027): Rocky Linux
# 8.10 and Red Hat's GCC 15.2.1 (gcc-toolset-15). The image holds the
# toolchain and the source, no binaries: everything compiles inside the
# container with -march=native, on the machine that runs it.
FROM rockylinux/rockylinux:8.10

LABEL org.opencontainers.image.source="https://github.com/ucrparlay/pab-tree" \
	org.opencontainers.image.description="PaB-tree artifact evaluation environment"

# EPEL has jemalloc, gperftools and R; their dependencies need PowerTools.
# tcmalloc, PAPI, TBB and libatomic are for Mass-tree and Bw-tree.
RUN dnf -y install dnf-plugins-core epel-release \
	&& dnf config-manager --set-enabled powertools \
	&& dnf -y install \
		gcc-toolset-15-gcc-c++ cmake make pkgconf-pkg-config \
		jemalloc-devel numactl numactl-devel \
		gperftools-devel papi-devel tbb-devel libatomic \
		python3.12 R-core fontconfig \
		bc time which wget unzip procps-ng findutils perf \
	&& dnf clean all \
	&& alternatives --set python3 /usr/bin/python3.12

# gcc-toolset-15 first on the PATH, as `scl enable gcc-toolset-15` puts it.
# Its libstdc++ is the system libstdc++.so.6 plus a static part, so the
# binaries need no LD_LIBRARY_PATH.
ENV PATH=/opt/rh/gcc-toolset-15/root/usr/bin:$PATH \
	CC=gcc \
	CXX=g++

# The packages the paper's plot scripts load: prebuilt EL8 binaries from
# Posit's CRAN mirror, frozen at one date. A missing package fails the build.
ARG CRAN=https://packagemanager.posit.co/cran/__linux__/centos8/2026-10-29
RUN Rscript -e ' \
	options(HTTPUserAgent = sprintf("R/%s R (%s)", getRversion(), \
		paste(getRversion(), R.version["platform"], R.version["arch"], \
		      R.version["os"]))); \
	pkgs <- c("dplyr", "tidyr", "ggplot2", "scales", "ggsci", "gtable", \
		  "latex2exp", "ggpubr", "zoo"); \
	install.packages(pkgs, repos = Sys.getenv("CRAN"), Ncpus = 4); \
	for (p in pkgs) library(p, character.only = TRUE)'

# The plots ask for "Linux Libertine"; CTAN ships it as "Linux Libertine O".
# Fixed mirrors: mirrors.ctan.org redirects to any mirror, some failing TLS.
RUN for m in https://mirrors.mit.edu/CTAN https://ctan.math.illinois.edu; do \
		curl -fsSL --retry 3 -o /tmp/libertine.zip "$m/fonts/libertine.zip" \
			&& break; \
	done \
	&& unzip -q -j /tmp/libertine.zip 'libertine/opentype/LinLibertine_*.otf' \
		-d /usr/share/fonts/libertine \
	&& rm /tmp/libertine.zip
COPY <<'EOF' /etc/fonts/local.conf
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
	<alias binding="same">
		<family>Linux Libertine</family>
		<prefer><family>Linux Libertine O</family></prefer>
	</alias>
</fontconfig>
EOF
RUN fc-cache -f && fc-match 'Linux Libertine' | grep -q 'Linux Libertine O'

# UTF-8, or R draws the plots' "×" as two dots
ENV LANG=C.UTF-8

# Defaults for the experiment scripts; docker-run.sh sets both.
ENV DATA_PREFIX=/data \
	INPUT_SIZE=1000000000

# The commit the image was built from, for the logs: the .git directory is
# not copied.
ARG GIT_SHA=unknown
ENV PAB_TREE_COMMIT=$GIT_SHA

WORKDIR /workspace/pab-tree
COPY . .

# docker-run.sh runs as the calling user, whose uid the image cannot know:
# anyone may build and write here.
RUN mkdir -p build scripts_ae/data scripts_ae/logs scripts_ae/plots \
	&& chmod -R a+rwX /workspace

CMD ["/bin/bash"]
