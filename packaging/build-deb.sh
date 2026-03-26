#!/usr/bin/env bash
# Build a Debian package (.deb) for system-update-inhibitor.
#
# Usage: build-deb.sh [VERSION]
#
#   VERSION   Version string (default: from git tag, fallback 1.0.0)
#
# The resulting .deb is written to dist/ in the repository root.
# Requires: dpkg-dev, debhelper (>= 13), libsystemd-dev, pkg-config, gcc, make

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEBIAN_SRC="${SCRIPT_DIR}/debian"
PACKAGE="system-update-inhibitor"
DIST_DIR="${REPO_ROOT}/dist"

VERSION="${1:-}"

if [[ -z "${VERSION}" ]]; then
    VERSION=$(git -C "${REPO_ROOT}" describe --tags --abbrev=0 2>/dev/null \
              | sed 's/^v//' || true)
    VERSION="${VERSION:-1.0.0}"
fi

if ! command -v dpkg-buildpackage &>/dev/null; then
    echo "error: dpkg-buildpackage not found. Install dpkg-dev." >&2
    exit 1
fi

echo "==> Building ${PACKAGE} ${VERSION}"

WORKDIR=$(mktemp -d)
trap 'rm -rf "${WORKDIR}"' EXIT

SRCDIR="${WORKDIR}/${PACKAGE}-${VERSION}"
mkdir -p "${SRCDIR}"

# Export current working tree source (including uncommitted changes)
echo "--> Exporting source from working tree"
(
    cd "${REPO_ROOT}"
    tar \
        --exclude='.git' \
        --exclude='./dist' \
        --exclude='./system-update-inhibitor' \
        -cf - .
) | tar -xf - -C "${SRCDIR}"

# The packaging directory is not part of the built package
rm -rf "${SRCDIR}/packaging"

# Place the debian directory and stamp the version into changelog
cp -r "${DEBIAN_SRC}" "${SRCDIR}/debian"
chmod +x "${SRCDIR}/debian/rules"
sed -i \
    "s/^${PACKAGE} ([^)]*) /${PACKAGE} (${VERSION}-1) /" \
    "${SRCDIR}/debian/changelog"

# Build binary package without signing
echo "--> Running dpkg-buildpackage"
(cd "${SRCDIR}" && dpkg-buildpackage -us -uc -b)

# Collect .deb output (dpkg-buildpackage writes one level up from SRCDIR)
mkdir -p "${DIST_DIR}"
find "${WORKDIR}" -maxdepth 1 -name "*.deb" -exec cp -v {} "${DIST_DIR}/" \;

echo ""
echo "==> Done"
echo "    Package: ${DIST_DIR}/"
