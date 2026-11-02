#!/bin/sh
# Fails if a binary needs a newer glibc/libstdc++ than RHEL 8 (Node.js's own floor: glibc 2.28, GCC 8 libstdc++).
set -e
objdump -T "$1" | grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9.]+' | sort -u | while read -r symbol; do
	case $symbol in
		GLIBC_*) max=GLIBC_2.28 ;;
		GLIBCXX_*) max=GLIBCXX_3.4.25 ;;
		CXXABI_*) max=CXXABI_1.3.11 ;;
	esac
	echo "$symbol (max $max)"
	[ "$(printf '%s\n%s\n' "$symbol" "$max" | sort -V | tail -n 1)" = "$max" ] || { echo "$1 needs $symbol, newer than $max"; exit 1; }
done
