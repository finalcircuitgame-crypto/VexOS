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

Start the listener from the repository root on Windows:

```powershell
py tools/netlog_receiver.py
```

If Windows Firewall prompts, allow Python on the private network. You can also
create an inbound UDP rule for local port 5140. Do not expose the listener to
public networks.

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
physical LAN test, override `NETLOG_LOCAL_IP`, `NETLOG_NETMASK`,
`NETLOG_GATEWAY`, and `NETLOG_DESTINATION_IP` with the correct host-order
`ARP_IPV4(a,b,c,d)` values in the kernel build flags. Keep the receiver on a
trusted private network.

NetLog sends bounded UTF-8 text datagrams (at most 1000 bytes) at a maximum
rate of 10 packets/second. Its 8 KiB queue never blocks `serial_print`; when
full, new log bytes are dropped and a notice is sent when possible. UDP is
best-effort and does not guarantee delivery.
