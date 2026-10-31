# Add the IPs of host.docker.internal and host.external to NO_PROXY/no_proxy.
# They come from --add-host (host-gateway and $HOST_IP) and differ between
# Docker hosts, so they are resolved when the shell starts.
# Installed to /etc/profile.d and sourced from /etc/bash.bashrc; safe to source repeatedly.
for __host in host.docker.internal host.external; do
    __host_ip="$(getent ahostsv4 "$__host" 2>/dev/null | awk 'NR == 1 { print $1 }')"
    [ -n "$__host_ip" ] || continue
    case ",${NO_PROXY}," in
        *",${__host_ip},"*) ;;
        *) NO_PROXY="${NO_PROXY:+${NO_PROXY},}${__host_ip}" ;;
    esac
    case ",${no_proxy}," in
        *",${__host_ip},"*) ;;
        *) no_proxy="${no_proxy:+${no_proxy},}${__host_ip}" ;;
    esac
done
export NO_PROXY no_proxy
unset __host __host_ip
