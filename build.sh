#!/usr/bin/bash

clear

gcc -o editor editor.c -lm -Wall -Wextra -O2 -Wswitch-enum -Werror
