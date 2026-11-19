# VexOS NetLog (development)

NetLog streams VexOS serial log text to a Windows/Linux/macOS UDP listener. It is
development-only: the kernel logger, COM1 output, and `/boot.log` remain the
primary/local logging paths.

## QEMU user networking

The development build defaults to:

- Guest IPv4: `10.0.2.15/24`
- Gateway and host endpoint: `10.0.2.2`
- UDP destination port: `5140`
- UDP source port: `5141`

These defaults are for QEMU's `-netdev user` network. The guest uses ARP, IPv4,
and UDP; DHCP and DNS are not required. The destination host is QEMU's host-side
gateway address, so the receiver can run on the Windows host.

Start the listener from the repository root on Windows **before booting VexOS**:

```powershell
py tools/netlog_receiver.py
```

The receiver prints an explicit waiting status every 10 seconds while no packets
arrive. A blank log stream does not mean the receiver is broken: it means no UDP
datagrams have reached that PC yet. Add `--output netlog.log` to save received
packets to a file, or `--self-test` to send a local test datagram through the
receiver socket. The self-test verifies only the local receiver, not VexOS,
Ethernet, or the LAN.

If Windows Firewall prompts, allow Python on the private network. Do not expose
the listener to public networks.

## Build modes

Development builds enable NetLog by default:

```sh
make
```

Release builds omit the serial logger hook and NetLog code path:

```sh
make VEXOS_RELEASE=1
```

## Physical LAN

The default static address is QEMU-specific. DHCP is not implemented. For a
physical LAN test, pass the laptop's static address, subnet mask, gateway, and
receiver PC's LAN address to `build.sh`:

```sh
NETLOG_LOCAL_IP=172.16.6.145 \
NETLOG_NETMASK=255.255.255.0 \
NETLOG_GATEWAY=172.16.6.1 \
NETLOG_DESTINATION=172.16.6.110 \
./build.sh
```

Replace those example addresses with the current addresses on your LAN. The
receiver listens on UDP port 5140 by default; VexOS sends from UDP source port
5141. Start the receiver before booting the freshly built image. Keep both
devices on a trusted private network.

NetLog sends bounded UTF-8 text datagrams (at most 1000 bytes) at a maximum
rate of 10 packets/second. Its 8 KiB queue never blocks `serial_print`; when
full, new log bytes are dropped and a notice is sent when possible. UDP is
best-effort and does not guarantee delivery.
