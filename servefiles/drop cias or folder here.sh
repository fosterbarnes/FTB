#!/bin/bash

if [ $# -eq 0 ]; then
    set -- .
fi
python3 "$(dirname "$0")/ftbsend.py" "$@"
