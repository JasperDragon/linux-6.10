#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

if [ -z "$SRCARCH" ]; then
	echo 'sync-check.sh: error: missing $SRCARCH environment variable' >&2
	exit 1
fi

FILES="include/linux/objtool_types.h"

if [ "$SRCARCH" = "x86" ]; then
FILES="$FILES
include/linux/interval_tree_generic.h
include/linux/livepatch_external.h
include/linux/static_call_types.h
"

SYNC_CHECK_FILES='
'
fi

check_2 () {
  file1=$1
  file2=$2

  shift
  shift

  cmd="diff $* $file1 $file2 > /dev/null"

  test -f $file2 && {
    eval $cmd || {
      echo "Warning: Kernel ABI header at '$file1' differs from latest version at '$file2'" >&2
      echo diff -u $file1 $file2
    }
  }
}

check () {
  file=$1

  shift

  check_2 tools/$file $file $*
}

if [ ! -d ../../kernel ] || [ ! -d ../../tools ] || [ ! -d ../objtool ]; then
	exit 0
fi

cd ../..

while read -r file_entry; do
    if [ -z "$file_entry" ]; then
	continue
    fi

    check $file_entry
done <<EOF
$FILES
EOF

if [ "$SRCARCH" = "x86" ]; then
	for i in $SYNC_CHECK_FILES; do
		check $i '-I "^.*\/\*.*__ignore_sync_check__.*\*\/.*$"'
	done
fi
