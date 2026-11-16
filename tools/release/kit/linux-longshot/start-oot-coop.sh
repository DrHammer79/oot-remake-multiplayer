#!/bin/sh
# The old name of start-squadala.sh (the start file up to v0.11.2), kept so a menu entry, a Steam shortcut
# or the updater of an older kit still finds it. It only starts start-squadala.sh: use that one.
here=$(cd "$(dirname "$0")" && pwd)
exec sh "$here/start-squadala.sh" "$@"
