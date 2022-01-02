# Geomesh - Basic Mesh Routing Protocols

A code base to quickly implement mesh routing protocols. A collection of basic protocols have been implemented already.
Included is also a OpenWrt package that is ready to be used in real networks, albeit not being production ready.

The goal of this project is to help to develop better mesh routing protocols, so that community networks such as Freifunk can scale better.

Note: These implementations use simplified algorithms that do not adhere to any technical publication.

Implemented Protocols:

- [flood-0](src/flood-0/)
  - reactive
  - sequence number
  - broadcasta only
  - no metric
- [flood-1](src/flood-1/)
  - reactive
  - sequence number
  - switch between broadcast and unicast
- [aodv-0](src/aodv-0/)
  - reactive
  - sequence number
- [dsr-0](src/dsr-0/)
  - reactive
  - sequence number
  - uses paths of MAC/IP
- [dsr-bloom-0](src/dsr-bloom-0/)
  - reactive
  - Bloom filter in packet
  - nodes have no state
  - only bradcast
- [dsr-bloom-1](src/dsr-bloom-1/)
  - reactive
  - Bloom filter in packet
  - nodes store Bloom filters of neighbbors
  - switches between bradcast and unicast
- [counting-bloom-0](src/counting-bloom-0/)
  - proactive
  - counting Bloom filter
  - degrade filter on every hop
  - send routing information as one hop bradcast to neighbors
  - send payload as unicast along gradient
- [vivaldi-0](src/vivaldi-0/)
  - proactive
  - incomplete!

Notes:
 - node identifiers are mapped to the tunnels IP address
 - the hop count metric is used in all examples
 - DSR stands for Dynamic Source Routing
 - AODV stands for Ad-hoc Distance Vector
 - the number after a routing protocol name differentiates variants

## Usage

```
./geomesh -p flood-0 -i wlan0
```

Since multiple protocols are available, a protocol must be specified at startup.
Otherwise a list of available protocols is printed.

Use the `tun0` interface to exchange packets with other instances.

```
$ ./build/geomesh -h
Usage: geomesh -i eth0 -i wlan0

  --protocol,-p               Select routing protocol
  --daemon,-d                 Run as daemon
  --interface,-i <interface>  Limit to given interfaces
  --log <path>                Write log output to file
  --peer <address>            Add a peer manually by address
  --control,-c <path>         Control socket to connect to a daemon
  --tun-name <ifname>         Set route device (Default: tun0)
  --tun-setup <1/0>           Configure tun device (Default: 1)
  --ether-type <hex>          Ethernet type (Default: 88b5)
  --log-level <level>         Logging level. From 0 to 6 (Default: 3)
  --verbosity <level>         Set verbosity to quiet, verbose or debug (Default: verbose)
  --disable-stdin             Disable interactive console on startup
  --enable-ipv4,-4 <0/1>      Enable IPv4 (Default: 0)
  --enable-ipv6,-6 <1/0>      Enable IPv6 (Default: 1)
  --help,-h                   Prints this help text
  --version,-v                Print version

Valid protocols: dsr-0, flood-0, ...
```

## Daemon Control

If geomesh is started as daemon (`-d`), a control socket can be used `./geomesh ... -s /tmp/geomesh.sock` to inspect the state.

```
geomesh-ctl -s /tmp/geomesh.sock
```

or use `socat`:

```
socat - UNIX-CONNECT:/tmp/geomesh.sock
```

## Further Reading

* [Development Notes](docs/notes.md)
* [Further Ideas](docs/ideas.md)

## Other Mobile Ad-hoc Network Protocols

Some popular or interesting Mobile Ad-hoc mesh routing protocols.

* [OLSR](https://datatracker.ietf.org/doc/html/rfc3626) (proactive, Link State)
* [Batman-adv](https://www.open-mesh.org/projects/batman-adv/wiki/Wiki) (proactive, Distance Vector)
* [Babel](https://www.irif.fr/~jch/software/babel/) (proactive, Distance Vector)
* [Yggdrasil](https://yggdrasil-network.github.io/) (Spanning Tree/Distance Vector)

Interesting projects for low bandwidth networks:

* [Disaster Radio](https://disaster.radio/)
* [GoTenna](https://gotenna.com/)
* [Reticulum](https://unsigned.io/projects/reticulum/)
