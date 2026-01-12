#!/usr/bin/bash

clear

typeset -i RELEASE=0

if [[ $1 == "release" ]] then
    RELEASE=1
elif [[ -n $1 ]] then
    echo "ERROR: Unsupported build mode $RELEASE"
    echo "Build modes:"
    echo "    release: optimizations"
    exit 1
fi

if (( $RELEASE )) then
    echo "release"
    gcc -o editor editor.c -lncurses -lm -Wall -Wextra -Werror -Wno-switch -Wno-discarded-qualifiers -O2
else
    gcc -o editor editor.c -lncurses -lm -Wall -Wextra -Werror -Wno-switch -Wno-discarded-qualifiers -ggdb
fi
