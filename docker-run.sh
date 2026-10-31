#!/bin/bash
#
# HOW TO RUN (paths are relative to this file, so run it from anywhere)
#   ./docker-run.sh help
#   ./docker-run.sh pull                                  # or: build
#   ./docker-run.sh run  --data-path /mnt/disk/pab-tree   # shell in the container
#   ./docker-run.sh full --data-path /mnt/disk/pab-tree   # every experiment
#
# Starts the artifact-evaluation container. Datasets live under --data-path,
# mounted at /data; results go to scripts_ae/{data,logs,plots}, mounted from
# the host. The container runs as the calling user, so the results stay yours.

set -euo pipefail

cd "$(dirname "$0")"

image="${PAB_TREE_IMAGE:-ghcr.io/ucrparlay/pab-tree:latest}"
container="pab-tree-ae"
workdir="/workspace/pab-tree"

red='\033[0;31m'
green='\033[0;32m'
yellow='\033[1;33m'
plain='\033[0m'

info() { echo -e "${green}[INFO]${plain} $1"; }
warn() { echo -e "${yellow}[WARN]${plain} $1"; }
error() { echo -e "${red}[ERROR]${plain} $1" >&2; }

usage() {
	cat <<EOF
Usage: $0 COMMAND [OPTIONS]

Commands:
    build       Build the image from this checkout
    pull        Pull the prebuilt image
    run         Start a shell in the container
    full        Run every experiment (scripts_ae/run.sh)
    shell       Open another shell in the running container
    stop        Stop the running container
    clean       Remove the results in scripts_ae/ and the image
    help        Show this message

Options for run and full:
    --data-path PATH    Host directory for the datasets, mounted at /data (required)
    --input-size N      Entries per synthetic dataset (default: 1000000000)
    --no-download       Do not download the real-world datasets
    --perf              Allow hardware counters (Fig. 7): run as root with SYS_ADMIN
    --with-masstree-bwtree
                        Also run Mass-tree and Bw-tree (Fig. 4, 5, 9)
    --cpus N            Limit the CPUs (default: all)
    --memory SIZE       Limit the memory, e.g. 512g (default: none)

Image: $image (set PAB_TREE_IMAGE to use another)

Examples:
    $0 pull
    $0 run --data-path /mnt/disk/pab-tree
    $0 run --data-path /mnt/disk/pab-tree --input-size 10000000
    $0 full --data-path /mnt/disk/pab-tree --perf

Details: docs/DOCKER_QUICK_REFERENCE.md, docs/ARTIFACT_EVALUATION.md
EOF
}

running() {
	[ -n "$(docker ps -q -f name="^${container}\$")" ]
}

build_image() {
	local sha
	sha=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
	info "Building $image at commit $sha"
	docker build --build-arg GIT_SHA="$sha" -t "$image" .
}

# A --perf run writes as root; hand the results back to the calling user.
# Not --data-path: it may hold other people's files.
give_back() {
	info "Returning the results to $(id -un)"
	docker run --rm -v "$PWD/scripts_ae:/ae" "$image" \
		chown -R "$(id -u):$(id -g)" /ae/data /ae/logs /ae/plots
}

# $1: run (an interactive shell) or full (every experiment)
run_container() {
	local mode=$1

	if [ -z "$data_path" ]; then
		error "--data-path is required: where the datasets go on this host"
		exit 1
	fi
	if running; then
		error "$container is already running; use: $0 shell"
		exit 1
	fi
	mkdir -p "$data_path" scripts_ae/data scripts_ae/logs scripts_ae/plots
	data_path=$(cd "$data_path" && pwd)

	local args=(
		--rm
		--name "$container"
		--cap-add=SYS_NICE # numactl -i all
		-e HOME=/tmp
		-e DATA_PREFIX=/data
		-e INPUT_SIZE="$input_size"
		-e DOWNLOAD="$download"
		-e WITH_MASSTREE_BWTREE="$with_masstree_bwtree"
		-v "$data_path:/data"
		-v "$PWD/scripts_ae/data:$workdir/scripts_ae/data"
		-v "$PWD/scripts_ae/logs:$workdir/scripts_ae/logs"
		-v "$PWD/scripts_ae/plots:$workdir/scripts_ae/plots"
	)
	if [ -t 0 ]; then
		args+=(-it)
	else
		args+=(-i)
	fi
	if [ "$perf" = 1 ]; then
		args+=(--cap-add=SYS_ADMIN --security-opt seccomp=unconfined)
		warn "--perf: the container runs as root"
	else
		args+=(-u "$(id -u):$(id -g)")
	fi
	if [ -n "$cpus" ]; then
		args+=(--cpus="$cpus")
	fi
	if [ -n "$memory" ]; then
		args+=(--memory="$memory")
	fi

	local prelude='if [ "$DOWNLOAD" = 1 ]; then
	if [ -x scripts_ae/download_data.sh ]; then
		scripts_ae/download_data.sh /data || exit 1
	else
		echo "[WARN] no scripts_ae/download_data.sh: real-world data not downloaded"
	fi
fi'
	local cmd='exec bash'
	if [ "$mode" = full ]; then
		cmd='if [ ! -x scripts_ae/run.sh ]; then
	echo "[ERROR] no scripts_ae/run.sh in this image" >&2
	exit 1
fi
exec scripts_ae/run.sh'
		warn "The full evaluation takes many hours; run it inside tmux or screen"
	fi

	info "Image:       $image"
	info "Data:        $data_path -> /data"
	info "Results:     $PWD/scripts_ae/{data,logs,plots}"
	info "Input size:  $input_size"

	local status=0
	docker run "${args[@]}" "$image" bash -c "$prelude
$cmd" || status=$?
	if [ "$perf" = 1 ]; then
		give_back
	fi
	return "$status"
}

open_shell() {
	if ! running; then
		error "$container is not running; start it with: $0 run --data-path PATH"
		exit 1
	fi
	docker exec -it -u "$(id -u):$(id -g)" -e HOME=/tmp "$container" bash
}

stop_container() {
	if running; then
		info "Stopping $container"
		docker stop "$container"
	else
		warn "$container is not running"
	fi
}

# Datasets under --data-path are left alone: delete them by hand.
clean() {
	if running; then
		docker stop "$container"
	fi
	info "Removing scripts_ae/data, scripts_ae/logs, scripts_ae/plots"
	if ! rm -rf scripts_ae/data scripts_ae/logs scripts_ae/plots 2>/dev/null; then
		warn "Some results belong to root; removing them in a container"
		docker run --rm -v "$PWD/scripts_ae:/ae" "$image" \
			rm -rf /ae/data /ae/logs /ae/plots
	fi
	if docker image inspect "$image" >/dev/null 2>&1; then
		docker rmi "$image"
	fi
	info "Clean"
}

if ! command -v docker >/dev/null; then
	error "Docker is not installed"
	exit 1
fi

command="${1:-help}"
if [ $# -gt 0 ]; then
	shift
fi

data_path=""
input_size=1000000000
download=1
perf=0
with_masstree_bwtree=0
cpus=""
memory=""

while [ $# -gt 0 ]; do
	case "$1" in
	--data-path)
		data_path="${2:?--data-path needs a directory}"
		shift 2
		;;
	--input-size)
		input_size="${2:?--input-size needs a number}"
		shift 2
		;;
	--no-download)
		download=0
		shift
		;;
	--perf)
		perf=1
		shift
		;;
	--with-masstree-bwtree)
		with_masstree_bwtree=1
		shift
		;;
	--cpus)
		cpus="${2:?--cpus needs a number}"
		shift 2
		;;
	--memory)
		memory="${2:?--memory needs a size}"
		shift 2
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		error "Unknown option: $1"
		usage
		exit 1
		;;
	esac
done

if ! [[ "$input_size" =~ ^[0-9]+$ ]]; then
	error "--input-size must be a number, got: $input_size"
	exit 1
fi

case "$command" in
build)
	build_image
	;;
pull)
	docker pull "$image"
	;;
run | full)
	run_container "$command"
	;;
shell)
	open_shell
	;;
stop)
	stop_container
	;;
clean)
	clean
	;;
help | -h | --help)
	usage
	;;
*)
	error "Unknown command: $command"
	usage
	exit 1
	;;
esac
