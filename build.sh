#!/usr/bin/bash

clear

gcc -o editor editor.c -lncurses -lm -Wall -Wextra -Wswitch-enum -Werror -Wno-discarded-qualifiers
