#!/bin/sh
#
# Author: Elliot Kohlmyer
# Date: March 15th, 2026
# Purpose: Bakey install script
#
set -e

progname="$0"
destdir=""
prefix="/usr/local"

print_usage() {
	echo "Usage: $progname [-h] [-d destdir]"
}

print_help() {
	print_usage
	echo "\nOptions:"
	echo "    -h|--help     Show this help message"
	echo "    -d|--destdir  Set destination directory (default is '')"
	echo "    -p|--prefix   Set directory prefix (default is '/usr/local')"
	echo "\nThe files are installed to <destdir>/<prefix>."
}

expected_argument() {
	if [ "$2" = "" ]; then
		echo "$progname: Expected argument after '$1'"
		print_usage
		exit 1
	fi
}

while ! [ "$1" = "" ]; do
	arg="$1"
	case "$arg" in
		-d|--destdir)
			shift
			destdir="$1"
			expected_argument "$arg" "$destdir"
			shift
			;;
		-p|--prefix)
			shift
			prefix="$1"
			expected_argument "$arg" "$prefix"
			shift
			;;
		-h|--help)
			print_help
			exit 0
			;;
		*)
			echo "$progname: Unrecognized option '$arg'"
			print_usage
			exit 1
			;;
	esac
done
destination="$destdir$prefix"

install_file() { # 1: source, 2: destination, 3: mode
	if [ -f "$1" ]; then
		cp -v "$1" "$2"
		chmod $3 "$2"
	fi
}

mkdir -pv "$destination/include"
mkdir -pv "$destination/share"
mkdir -pv "$destination/lib"
mkdir -pv "$destination/bin"

install_file "lib/libbakey.a" "$destination/lib/libbakey.a" 0644
install_file "lib/libbakey-posix.a" "$destination/lib/libbakey-posix.a" 0644
install_file "lib/libbakey-rc.a" "$destination/lib/libbakey-rc.a" 0644

install_file "include/bakey-config.h" "$destination/include/bakey-config.h" 0644
install_file "include/bakey.h" "$destination/include/bakey.h" 0644
install_file "include/bakey-posix.h" "$destination/include/bakey-posix.h" 0644
install_file "include/bakey-rc.h" "$destination/include/bakey-rc.h" 0644
install_file "include/bakey-sdl-config.h" "$destination/include/bakey-sdl-config.h" 0644
install_file "include/bakey-wl-config.h" "$destination/include/bakey-wl-config.h" 0644
install_file "include/bakey-xdg-shell.h" "$destination/include/bakey-xdg-shell.h" 0644

install_file "bin/bakey-dummy" "$destination/bin/bakey-dummy" 0755
install_file "bin/bakey-reference" "$destination/bin/bakey-reference" 0755
install_file "bin/bakey-sdl" "$destination/bin/bakey-sdl" 0755
install_file "bin/bakey-wl" "$destination/bin/bakey-wl" 0755

install_file "bakeysdlrc.default" "$destination/share/bakeysdlrc" 0644
install_file "bakeywlrc.default" "$destination/share/bakeywlrc" 0644
