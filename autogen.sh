#! /bin/sh
set -ex
test -d m4 || mkdir m4
autoreconf -i -f
intltoolize -c --automake --force
rm -rf autom4te.cache
