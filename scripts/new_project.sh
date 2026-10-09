#!/bin/sh
# new_project.sh <Name> [options]: see scripts/new_project.py
exec python3 "$(dirname "$0")/new_project.py" "$@"
