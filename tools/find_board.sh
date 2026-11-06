#!/bin/bash
# Find the Orange Pi Zero 3W on the local /24 and update the
# opz3w ssh alias in ~/.ssh/config (WSL cannot resolve the mDNS
# name orangepizero3w.local, so the numeric IP lives in the config).
set -u

SUBNET=${1:-${OPZ3W_SUBNET:-192.168.5}}
# NOTE: the WSL2 default route points at the NAT gateway (172.x.x.x),
# not the LAN - deriving the subnet from it finds nothing. Pass the LAN
# prefix as $1 (or OPZ3W_SUBNET) if your LAN differs from 192.168.5.
if [ -z "$SUBNET" ]; then
	echo "cannot determine subnet, pass one: $0 192.168.5" >&2
	exit 1
fi

echo "scanning $SUBNET.1-254 port 22 ..."
FOUND=""
for i in $(seq 1 254); do
	(timeout 0.4 bash -c "echo > /dev/tcp/$SUBNET.$i/22" 2>/dev/null && echo "$SUBNET.$i") &
done
wait > /tmp/.opz3wscan.$$

for ip in $(cat /tmp/.opz3wscan.$$); do
	host=$(ssh -o BatchMode=yes -o ConnectTimeout=3 \
		-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		root@$ip hostname 2>/dev/null)
	if [ "$host" = "orangepizero3w" ]; then
		echo "FOUND board at $ip"
		FOUND=$ip
		break
	fi
done
rm -f /tmp/.opz3wscan.$$

if [ -z "$FOUND" ]; then
	echo "board not found on $SUBNET.0/24" >&2
	exit 1
fi

# rewrite the Host block in ~/.ssh/config
awk -v ip="$FOUND" '
	/^Host opz3w$/ {
		skip = 1
		print "Host opz3w"
		print "    HostName " ip
		print "    User root"
		print "    IdentityFile ~/.ssh/id_rsa_pocket"
		print "    StrictHostKeyChecking no"
		print "    UserKnownHostsFile /dev/null"
		print "    LogLevel ERROR"
		next
	}
	/^[ \t]/ { if (skip) next }
	{ skip = 0; print }
' ~/.ssh/config > ~/.ssh/config.new && mv ~/.ssh/config.new ~/.ssh/config
chmod 600 ~/.ssh/config

echo "ssh alias opz3w -> $FOUND"
ssh -o ConnectTimeout=5 opz3w hostname
