# devcontainer.json, Dockerfile

`
Always use 
FROM mcr.azure.cn/devcontainers/base:trixie
`

*Build section should be like*

```json
{
    "build": {
        "dockerfile": "Dockerfile",
        "args": {
            "HTTP_PROXY": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
            "HTTPS_PROXY": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
            "NO_PROXY": "localhost,127.0.0.1",
            "http_proxy": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
            "https_proxy": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
            "no_proxy": "localhost,127.0.0.1"
        },
        "options": [
            "--add-host=host.docker.internal:host-gateway",
            "--add-host=host.external:${localEnv:HOST_IP:host-gateway}"
        ]
    }
}
```

*Mount section should be like*

userid - root folder name

`pnpm-store` should be defined if pnpm installed,
`userid-go` should be if Go used on project

```json
{
    "mounts": [
        "source=pnpm-store,target=/pnpm-store,type=volume,consistency=cached",
        "source=claude,target=/home/vscode/.claude,type=volume,consistency=cached",
        "source=vscode-server-userid,target=/home/vscode/.vscode-server,type=volume,consistency=cached", 
        "source=userid-go,target=/home/vscode/go,type=volume,consistency=cached"
    ]
}
```

based on mounts etc, on Dockerfile should be code like this

```bash
RUN mkdir -p /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/go /pnpm-store && \
    chown -R vscode:vscode /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/go /pnpm-store
```

*Container env should be like*

`npm_config_store_dir` and  `pnpm_config_store_dir` should be defined if pnpm installed

```json
{
    "containerEnv": {
        "npm_config_store_dir": "/pnpm-store",
        "pnpm_config_store_dir": "/pnpm-store",
        "CONTAINER_NAME": "userid-dev-${localEnv:USERID_INSTANCE:default}",
        "HTTP_PROXY": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
        "HTTPS_PROXY": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
        "NO_PROXY": "localhost,127.0.0.1,host.docker.internal,host.external",
        "http_proxy": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
        "https_proxy": "${localEnv:VPN_DOCKER_HTTP_PROXY}",
        "no_proxy": "localhost,127.0.0.1,host.docker.internal,host.external",
        "PROJECT_NAME": "${localWorkspaceFolderBasename}",
        "WORKSPACE_DIR": "${containerWorkspaceFolder}"
    }
}
```

*NO_PROXY must also contain the IPs of host.docker.internal and host.external*

Host services (MySQL, S3, etc.) are reached by these names or by their IPs; if an IP is
missing from `NO_PROXY`, requests go to the VPN proxy, which can't reach the host, and hang.
The IPs come from `--add-host` and differ between Docker hosts, so never hardcode them
(`containerEnv` can't run commands) — resolve them at runtime with `.devcontainer/no-proxy-host.sh`:

```sh
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
```

and install it in the Dockerfile, so login and interactive shells both load it:

```dockerfile
COPY no-proxy-host.sh /etc/profile.d/20-no-proxy-host.sh
RUN echo '. /etc/profile.d/20-no-proxy-host.sh' >> /etc/bash.bashrc
```

This covers terminals and anything started from them. Processes started directly by VS Code
(e.g. MCP servers) only get `containerEnv`, so configure those with the `host.docker.internal`
name, not the IP.

*RunArgs should be like*

```json
{
    "runArgs": [
        "--name=userid-dev-${localEnv:USERID_INSTANCE:default}",
        "--add-host=host.docker.internal:host-gateway",
        "--add-host=host.external:${localEnv:HOST_IP:host-gateway}",
        "--env-file=.devcontainer/.env"
    ],
}
```

`host.docker.internal` always points to `host-gateway`. `host.external` points to `HOST_IP` (falls
back to `host-gateway` when unset, so the container still starts) — use it for services on another
machine or the host's LAN address. Set `HOST_IP` in the **host machine's** environment
(e.g. `export HOST_IP=192.168.1.10` before starting VS Code): devcontainer.json reads only host env
vars via `${localEnv:...}`, not `.devcontainer/.env`. Use the same two `--add-host` lines in
`build.options`, and list both names in `NO_PROXY`/`no_proxy` of `containerEnv`.

*Extensions vscode.extensions*

`
install vscode extesnions "anthropic.claude-code"
`

## Worry about cached, where you can cache. After rebuild container, should use cache, where it possible, via mounted dir etc.


## About js applications etc.

Always try use `viteplus`, and pnpm, for create project etc, install packages etc. And install vite plus with static node version. And always set static version on package.json "devEngines", and pnpm too.

example package .json

```json
{
  "devEngines": {
    "runtime": {
      "name": "node",
      "version": "XX.XX.X",
      "onFail": "download"
    },
    "packageManager": {
      "name": "pnpm",
      "version": "XX.XX.XX",
      "onFail": "download"
    }
  }
}
```
