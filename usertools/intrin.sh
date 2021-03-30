#!/bin/bash

# Script to find missing machine flags
# ref: https://stackoverflow.com/questions/43128698/inlining-failed-in-call-to-always-inline-mm-mullo-epi32-target-specific-opti

get_instruction ()
{
    [ -z "$1" ] && exit
    func_name="$1 "

    header_file=`grep --include=\*intrin.h -Rl "$func_name" /usr/lib/gcc | head -n1`
    [ -z "$header_file" ] && exit
    >&2 echo "find in: $header_file"

    target_directive=`grep "#pragma GCC target(\|$func_name" $header_file | grep -B 1 "$func_name" | head -n1`
    echo $target_directive | grep -o '"[^,]*[,"]' | sed 's/"//g' | sed 's/,//g'
}

instruction=`get_instruction $1`
if [ -z "$instruction" ]; then
    echo "Error: function not found: $1"
else
    echo "add this option to gcc: -m$instruction"
fi