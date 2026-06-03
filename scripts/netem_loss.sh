#!/usr/bin/env bash
set -euo pipefail

DEFAULT_LEFT_NS="webrtc_tx"
DEFAULT_RIGHT_NS="webrtc_rx"
DEFAULT_LEFT_VETH="veth_tx"
DEFAULT_RIGHT_VETH="veth_rx"
DEFAULT_LEFT_IP="10.88.0.1/24"
DEFAULT_RIGHT_IP="10.88.0.2/24"

usage() {
    cat <<'EOF'
Usage:
  Device mode:
    sudo ./scripts/netem_loss.sh set <dev> <loss_percent> [delay_ms]
    sudo ./scripts/netem_loss.sh clear <dev>
    sudo ./scripts/netem_loss.sh show <dev>

  Network namespace + veth mode:
    sudo ./scripts/netem_loss.sh ns-up
    sudo ./scripts/netem_loss.sh ns-loss <loss_percent> [delay_ms] [tx|rx|both]
    sudo ./scripts/netem_loss.sh ns-clear [tx|rx|both]
    sudo ./scripts/netem_loss.sh ns-show
    sudo ./scripts/netem_loss.sh ns-down

Examples:
  sudo ./scripts/netem_loss.sh ns-up
  sudo ./scripts/netem_loss.sh ns-loss 15
  sudo ./scripts/netem_loss.sh ns-show
  sudo ./scripts/netem_loss.sh ns-clear
  sudo ./scripts/netem_loss.sh ns-down

Run commands inside namespaces:
  sudo ip netns exec webrtc_tx <sender command>
  sudo ip netns exec webrtc_rx <receiver command>

Default topology:
  webrtc_tx/veth_tx 10.88.0.1/24  <---->  webrtc_rx/veth_rx 10.88.0.2/24

Notes:
  - ns-loss defaults to tx, so loss is applied only from webrtc_tx to webrtc_rx.
  - This avoids loopback's possible two-direction loss amplification.
  - The current single-process local WebRTC demo does not cross veth. To test
    this topology, run sender and receiver as separate processes in different
    namespaces.
EOF
}

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "error: this script must run as root, use sudo" >&2
        exit 1
    fi
}

require_dev() {
    local dev="$1"
    if ! ip link show "${dev}" >/dev/null 2>&1; then
        echo "error: network device '${dev}' not found" >&2
        exit 1
    fi
}

ns_exists() {
    local ns="$1"
    ip netns list | awk '{print $1}' | grep -qx "${ns}"
}

require_ns() {
    local ns="$1"
    if ! ns_exists "${ns}"; then
        echo "error: network namespace '${ns}' not found; run ns-up first" >&2
        exit 1
    fi
}

qdisc_show_ns() {
    local ns="$1"
    local dev="$2"
    ip netns exec "${ns}" tc qdisc show dev "${dev}"
}

qdisc_clear_ns() {
    local ns="$1"
    local dev="$2"
    ip netns exec "${ns}" tc qdisc del dev "${dev}" root 2>/dev/null || true
}

qdisc_set_ns() {
    local ns="$1"
    local dev="$2"
    local loss="$3"
    local delay_ms="$4"

    if [[ "${delay_ms}" == "0" ]]; then
        ip netns exec "${ns}" tc qdisc replace dev "${dev}" root netem loss "${loss}%"
    else
        ip netns exec "${ns}" tc qdisc replace dev "${dev}" root netem loss "${loss}%" delay "${delay_ms}ms"
    fi
}

setup_namespaces() {
    require_root

    if ns_exists "${DEFAULT_LEFT_NS}" || ns_exists "${DEFAULT_RIGHT_NS}"; then
        echo "error: namespace already exists; run ns-down first if you want a clean setup" >&2
        exit 1
    fi

    ip netns add "${DEFAULT_LEFT_NS}"
    ip netns add "${DEFAULT_RIGHT_NS}"

    ip link add "${DEFAULT_LEFT_VETH}" type veth peer name "${DEFAULT_RIGHT_VETH}"
    ip link set "${DEFAULT_LEFT_VETH}" netns "${DEFAULT_LEFT_NS}"
    ip link set "${DEFAULT_RIGHT_VETH}" netns "${DEFAULT_RIGHT_NS}"

    ip netns exec "${DEFAULT_LEFT_NS}" ip addr add "${DEFAULT_LEFT_IP}" dev "${DEFAULT_LEFT_VETH}"
    ip netns exec "${DEFAULT_RIGHT_NS}" ip addr add "${DEFAULT_RIGHT_IP}" dev "${DEFAULT_RIGHT_VETH}"

    ip netns exec "${DEFAULT_LEFT_NS}" ip link set lo up
    ip netns exec "${DEFAULT_RIGHT_NS}" ip link set lo up
    ip netns exec "${DEFAULT_LEFT_NS}" ip link set "${DEFAULT_LEFT_VETH}" up
    ip netns exec "${DEFAULT_RIGHT_NS}" ip link set "${DEFAULT_RIGHT_VETH}" up

    echo "created namespace topology:"
    echo "  ${DEFAULT_LEFT_NS}/${DEFAULT_LEFT_VETH} ${DEFAULT_LEFT_IP}"
    echo "  ${DEFAULT_RIGHT_NS}/${DEFAULT_RIGHT_VETH} ${DEFAULT_RIGHT_IP}"
    echo
    echo "test connectivity:"
    echo "  sudo ip netns exec ${DEFAULT_LEFT_NS} ping -c 3 10.88.0.2"
}

delete_namespaces() {
    require_root

    if ns_exists "${DEFAULT_LEFT_NS}"; then
        ip netns del "${DEFAULT_LEFT_NS}"
    fi
    if ns_exists "${DEFAULT_RIGHT_NS}"; then
        ip netns del "${DEFAULT_RIGHT_NS}"
    fi

    echo "deleted namespaces if they existed:"
    echo "  ${DEFAULT_LEFT_NS}"
    echo "  ${DEFAULT_RIGHT_NS}"
}

set_namespace_loss() {
    require_root
    require_ns "${DEFAULT_LEFT_NS}"
    require_ns "${DEFAULT_RIGHT_NS}"

    local loss="${1:-}"
    local delay_ms="${2:-0}"
    local direction="${3:-tx}"

    if [[ -z "${loss}" ]]; then
        usage
        exit 1
    fi

    case "${direction}" in
        tx)
            qdisc_set_ns "${DEFAULT_LEFT_NS}" "${DEFAULT_LEFT_VETH}" "${loss}" "${delay_ms}"
            ;;
        rx)
            qdisc_set_ns "${DEFAULT_RIGHT_NS}" "${DEFAULT_RIGHT_VETH}" "${loss}" "${delay_ms}"
            ;;
        both)
            qdisc_set_ns "${DEFAULT_LEFT_NS}" "${DEFAULT_LEFT_VETH}" "${loss}" "${delay_ms}"
            qdisc_set_ns "${DEFAULT_RIGHT_NS}" "${DEFAULT_RIGHT_VETH}" "${loss}" "${delay_ms}"
            ;;
        *)
            echo "error: direction must be tx, rx, or both" >&2
            exit 1
            ;;
    esac

    echo "configured namespace netem: loss=${loss}% delay=${delay_ms}ms direction=${direction}"
    show_namespace_qdisc
}

clear_namespace_loss() {
    require_root
    require_ns "${DEFAULT_LEFT_NS}"
    require_ns "${DEFAULT_RIGHT_NS}"

    local direction="${1:-tx}"

    case "${direction}" in
        tx)
            qdisc_clear_ns "${DEFAULT_LEFT_NS}" "${DEFAULT_LEFT_VETH}"
            ;;
        rx)
            qdisc_clear_ns "${DEFAULT_RIGHT_NS}" "${DEFAULT_RIGHT_VETH}"
            ;;
        both)
            qdisc_clear_ns "${DEFAULT_LEFT_NS}" "${DEFAULT_LEFT_VETH}"
            qdisc_clear_ns "${DEFAULT_RIGHT_NS}" "${DEFAULT_RIGHT_VETH}"
            ;;
        *)
            echo "error: direction must be tx, rx, or both" >&2
            exit 1
            ;;
    esac

    echo "cleared namespace netem direction=${direction}"
    show_namespace_qdisc
}

show_namespace_qdisc() {
    require_ns "${DEFAULT_LEFT_NS}"
    require_ns "${DEFAULT_RIGHT_NS}"

    echo "${DEFAULT_LEFT_NS}/${DEFAULT_LEFT_VETH}:"
    qdisc_show_ns "${DEFAULT_LEFT_NS}" "${DEFAULT_LEFT_VETH}"
    echo "${DEFAULT_RIGHT_NS}/${DEFAULT_RIGHT_VETH}:"
    qdisc_show_ns "${DEFAULT_RIGHT_NS}" "${DEFAULT_RIGHT_VETH}"
}

cmd="${1:-}"

if [[ -z "${cmd}" ]]; then
    usage
    exit 1
fi

case "${cmd}" in
    set)
        dev="${2:-}"
        loss="${3:-}"
        delay_ms="${4:-0}"
        require_root

        if [[ -z "${dev}" || -z "${loss}" ]]; then
            usage
            exit 1
        fi

        require_dev "${dev}"

        if [[ "${delay_ms}" == "0" ]]; then
            tc qdisc replace dev "${dev}" root netem loss "${loss}%"
        else
            tc qdisc replace dev "${dev}" root netem loss "${loss}%" delay "${delay_ms}ms"
        fi

        echo "configured ${dev}: loss=${loss}% delay=${delay_ms}ms"
        tc qdisc show dev "${dev}"
        ;;

    clear)
        dev="${2:-}"
        require_root

        if [[ -z "${dev}" ]]; then
            usage
            exit 1
        fi

        require_dev "${dev}"
        tc qdisc del dev "${dev}" root 2>/dev/null || true
        echo "cleared netem on ${dev}"
        tc qdisc show dev "${dev}"
        ;;

    show)
        dev="${2:-}"

        if [[ -z "${dev}" ]]; then
            usage
            exit 1
        fi

        require_dev "${dev}"
        tc qdisc show dev "${dev}"
        ;;

    ns-up)
        setup_namespaces
        ;;

    ns-loss)
        set_namespace_loss "${2:-}" "${3:-0}" "${4:-tx}"
        ;;

    ns-clear)
        clear_namespace_loss "${2:-tx}"
        ;;

    ns-show)
        show_namespace_qdisc
        ;;

    ns-down)
        delete_namespaces
        ;;

    *)
        usage
        exit 1
        ;;
esac
