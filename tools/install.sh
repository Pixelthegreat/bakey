#!/bin/sh
implementations="$1"
destination="$2"

mkdir -pv "$destination/bin" "$destination/share"

for impl in $implementations; do
	cp -v "./bin/bakey-$impl" "$destination/bin/bakey-$impl"
	chmod 0755 "$destination/bin/bakey-$impl"

	[ -f "./bakey${impl}rc.default" ] &&\
		cp -v "./bakey${impl}rc.default" "$destination/share/bakey${impl}rc"
done
