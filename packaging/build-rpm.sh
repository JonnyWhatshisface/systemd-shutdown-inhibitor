#!/usr/bin/env bash
# Build RPM and/or SRPM for system-update-inhibitor.
#
# Usage: build-rpm.sh [--srpm-only] [VERSION]
#
#   --srpm-only   Build SRPM only (no binary RPM)
#   VERSION       Version string (default: from git tag, fallback 1.0.0)
#
# The RPM/SRPM output is written to ~/rpmbuild/RPMS/ and ~/rpmbuild/SRPMS/.
# Requires: rpmbuild, git

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SPEC_SRC="${SCRIPT_DIR}/rpm/system-update-inhibitor.spec"
PACKAGE="system-update-inhibitor"
RPMBUILD_ROOT="${HOME}/rpmbuild"

SRPM_ONLY=false
VERSION=""

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --srpm-only) SRPM_ONLY=true ; shift ;;
        --help|-h)   usage ;;
        --*)         echo "error: unknown option: $1" >&2 ; exit 1 ;;
        *)           VERSION="$1" ; shift ;;
    esac
done

# Accept tags such as v1.2.3 while preserving package-friendly version strings.
VERSION="${VERSION#v}"

if [[ -z "${VERSION}" ]]; then
    VERSION=$(git -C "${REPO_ROOT}" describe --tags --abbrev=0 2>/dev/null \
              | sed 's/^v//' || true)
    VERSION="${VERSION:-1.0.0}"
fi

TARBALL="${PACKAGE}-${VERSION}.tar.gz"

echo "==> Building ${PACKAGE} ${VERSION}"

# Ensure rpmbuild directory tree exists
mkdir -p "${RPMBUILD_ROOT}"/{SPECS,SOURCES,BUILD,RPMS,SRPMS}

# Create source tarball from current working tree
echo "--> Creating source tarball ${TARBALL}"
TMPDIR=$(mktemp -d)
trap 'rm -rf "${TMPDIR}"' EXIT

STAGE_DIR="${TMPDIR}/${PACKAGE}-${VERSION}"
mkdir -p "${STAGE_DIR}"

# Export repository contents, excluding VCS metadata and transient outputs.
(
    cd "${REPO_ROOT}"
    tar \
        --exclude='.git' \
        --exclude='./dist' \
        --exclude='./system-update-inhibitor' \
        -cf - .
) | tar -xf - -C "${STAGE_DIR}"

tar -C "${TMPDIR}" -czf "${RPMBUILD_ROOT}/SOURCES/${TARBALL}" \
    "${PACKAGE}-${VERSION}"

# Write spec with version stamped in
SPEC="${RPMBUILD_ROOT}/SPECS/${PACKAGE}.spec"
sed "s/^Version:.*/Version:        ${VERSION}/" "${SPEC_SRC}" > "${SPEC}"

if [[ "${SRPM_ONLY}" == "true" ]]; then
    BUILD_FLAG="-bs"
    echo "--> Building SRPM"
else
    BUILD_FLAG="-ba"
    echo "--> Building RPM + SRPM"
fi

rpmbuild "${BUILD_FLAG}" \
    --define "_topdir ${RPMBUILD_ROOT}" \
    "${SPEC}"

echo ""
echo "==> Done"
[[ "${SRPM_ONLY}" != "true" ]] && echo "    RPMs:  ${RPMBUILD_ROOT}/RPMS/"
echo "    SRPMs: ${RPMBUILD_ROOT}/SRPMS/"
