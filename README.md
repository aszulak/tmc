# tmc (Tibia Multi Connection)

Small `LD_PRELOAD` library (`libtmc.so`) and launcher (`tmc`) for Linux that splits Tibia's game connections across two network interfaces (for example Ethernet + Wi-Fi, or main ISP + mobile hotspot).

## How it works

When logging into a game server, the Tibia client opens two connections on port `7171`:
- `prod-cf-*.tibia.com` (Cloudflare proxy)
- `prod-lb-*.tibia.com` (AWS load balancer)

Normally the OS sends both connections through the same default route and interface. `tmc` preloads `libtmc.so` into the client and hooks two libc calls:
1. `getaddrinfo()`: watches DNS queries for `prod-cf-*` and `prod-lb-*` and saves the resolved IPv4 addresses.
2. `connect()`: intercepts outgoing connections to port `7171` and binds each socket to the matching network interface with `SO_BINDTODEVICE` (`prod-cf-*` goes to `--primary`, `prod-lb-*` goes to `--secondary`). If an IP wasn't seen in DNS, it alternates between both interfaces.

https://github.com/user-attachments/assets/c70a8c35-06da-4f6d-a7bd-e6056819b40d


## Tibia ToS

Response from the Tibia support regarding whenever this app is allowed to use.

>Hello,
>
>Thank you for your message. I understand that you're worried that your account will be punished if you implement your own tool.
>
>I'm afraid I cannot fully greenlight your project. Personally, I don't expect it to cause problems, but then again, please understand that you lose all rights to support if you experience problems >with the Tibia client due to using an external program - even if the program on paper acts according to the Tibia Rules.
>
>If you have other concerns, please feel free to contact me.
>
>
>Kind regards,  
>XXX  
>CipSoft Tech Support  


## Build

```bash
cmake -B build
cmake --build build
```

This builds `build/tmc` and `build/libtmc.so`.

## Usage

Run the launcher with your primary and secondary interface names:

```bash
./build/tmc --primary eth0 --secondary wlan0
```

By default `tmc` looks for the client at `$HOME/.local/share/CipSoft GmbH/Tibia/packages/Tibia/bin/client` (or `$TMC_CLIENT_BIN`). You can also pass the client binary path and any client arguments directly:

```bash
./build/tmc -p enp2s0 -s wlp3s0 -c /path/to/Tibia/packages/Tibia/bin/client -- --battleye
```

Or use `LD_PRELOAD` manually without the `tmc` binary:

```bash
TMC_PRIMARY=eth0 TMC_SECONDARY=wlan0 LD_PRELOAD=./build/libtmc.so /path/to/client
```

## Logs and verification

Logs are printed to `stderr` with the `[tmc]` tag:

```text
[tmc] exec: '/home/user/.local/share/CipSoft GmbH/Tibia/packages/Tibia/bin/client' | primary='eth0' secondary='wlan0' preload='/path/to/build/libtmc.so'
[tmc] init: pid=48210 primary='eth0' secondary='wlan0'
[tmc] dns: prod-cf-eu.tibia.com -> 172.65.201.42 (primary -> 'eth0')
[tmc] dns: prod-lb-eu.tibia.com -> 3.68.114.90 (secondary -> 'wlan0')
[tmc] connect: fd=28 dst=172.65.201.42:7171 route=primary/cf -> bound to 'eth0'
[tmc] connect: fd=29 dst=3.68.114.90:7171 route=secondary/lb -> bound to 'wlan0'
```

You can also check the active sockets with `ss` while in game:

```bash
ss -tnp 'dport = :7171'
```

Both connections should show up bound to their respective interfaces (`%eth0` and `%wlan0`).
