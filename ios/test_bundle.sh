set -euo pipefail
CONFIG=Debug
BUILD_DIR="${CONFIGURATION_BUILD_DIR:-$PWD/build/${CONFIG}-iphonesimulator}"
BUNDLE="${BUILD_DIR}/PSCAL.app"
PROJECT_DIR="${PWD}/ios"
mkdir -p "${BUILD_DIR}"
export PROJECT_DIR
CONFIGURATION_BUILD_DIR="${BUILD_DIR}" FULL_PRODUCT_NAME="PSCAL.app" bash -c 'set -euo pipefail; bundle_root="${CONFIGURATION_BUILD_DIR}/${FULL_PRODUCT_NAME}"; toolchain_root="${PROJECT_DIR}/.."; mkdir -p "${bundle_root}"; copy_dir(){ local src="$1"; local dest_name="$2"; if [ ! -d "${src}" ]; then echo "missing ${src}" >&2; return; fi; /usr/bin/rsync -a --delete "${src}/" "${bundle_root}/${dest_name}/"; }; copy_dir "${toolchain_root}/lib" "lib"; copy_dir "${toolchain_root}/fonts" "fonts"; copy_dir "${toolchain_root}/etc" "etc"; copy_dir "${toolchain_root}/Docs" "Docs"; copy_dir "${toolchain_root}/Tests/libs" "TestsLibs";'
ls "$BUNDLE"
