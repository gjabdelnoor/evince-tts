#!/usr/bin/env bash
# Run the Evince TTS fork from the uninstalled build tree.
# meson devenv sets EV_BACKENDS_DIR, GSETTINGS_SCHEMA_DIR and LD_LIBRARY_PATH
# so the freshly built binary (with the new tts-* schema keys) is used.
set -e
cd "$(dirname "$0")/evince-46.3.1"
exec meson devenv -C build ./shell/evince "$@"
