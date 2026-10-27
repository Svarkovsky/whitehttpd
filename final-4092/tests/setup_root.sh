#!/bin/sh
# Creates the test tree in /tmp/rt on the device under test (busybox sh).
# Run it ON THE DEVICE: sh setup_root.sh
#
# This only builds files and symlinks. It starts no server and touches no
# port: when you start whitehttpd yourself, pick a free port of your own.
# Do not disturb an instance already listening on port 80.
set -e
rm -rf /tmp/rt /tmp/secret_outside.txt
mkdir -p /tmp/rt/sub /tmp/rt/nolist "/tmp/rt/space dir" /tmp/rt/escdir2

echo "HELLO_ROOT" > /tmp/rt/index.html
printf "0123456789abcdef" > /tmp/rt/a.txt
echo "SUBIDX" > /tmp/rt/sub/index.html
echo "SUBFILE" > /tmp/rt/sub/b.txt
echo "SPACED" > "/tmp/rt/space dir/c.txt"
echo "x" > /tmp/rt/nolist/f1.txt
echo "y" > /tmp/rt/nolist/f2.txt
echo ".hidden" > /tmp/rt/.dotfile

# a target outside the root, and the escape vectors
echo "SECRET_OUTSIDE" > /tmp/secret_outside.txt
ln -s /tmp/secret_outside.txt /tmp/rt/link_file.txt
ln -s /tmp /tmp/rt/escdir
ln -s /tmp/secret_outside.txt /tmp/rt/escdir2.txt
echo "SECRET_OUTSIDE" > /tmp/rt/escdir2/whsecret.txt

# 4 MB, to check transfer integrity
head -c 4194304 /dev/urandom > /tmp/rt/big.bin
echo "root ready: $(ls /tmp/rt | wc -l) entries"
