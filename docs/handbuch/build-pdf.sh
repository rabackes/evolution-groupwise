#!/bin/sh
# Builds a manual PDF from its HTML source with WeasyPrint
# (pip install weasyprint, or the distribution package python3-weasyprint).
# Usage: build-pdf.sh [Benutzerhandbuch|UserGuide]  (default: Benutzerhandbuch)
#
# Copyright (C) 2026 bond Software Entwicklung GmbH
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu
cd "$(dirname "$0")"
WEASYPRINT="${WEASYPRINT:-weasyprint}"
BASE="${1:-Benutzerhandbuch}"
"$WEASYPRINT" "$BASE.html" "$BASE.pdf"
echo "$BASE.pdf written"
