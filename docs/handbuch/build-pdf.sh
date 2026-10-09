#!/bin/sh
# Builds Benutzerhandbuch.pdf from Benutzerhandbuch.html with WeasyPrint
# (pip install weasyprint, or the distribution package python3-weasyprint).
#
# Copyright (C) 2026 bond Software Entwicklung GmbH
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu
cd "$(dirname "$0")"
WEASYPRINT="${WEASYPRINT:-weasyprint}"
"$WEASYPRINT" Benutzerhandbuch.html Benutzerhandbuch.pdf
echo "Benutzerhandbuch.pdf written"
