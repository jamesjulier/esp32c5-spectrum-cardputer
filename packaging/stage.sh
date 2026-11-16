#!/usr/bin/env bash
set -euo pipefail
install -D -m 0755 esp32c5-spectrum "$STAGE$INSTALL_PREFIX/bin/$PKG_NAME"
