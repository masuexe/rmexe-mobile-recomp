#!/usr/bin/env bash
# Build unsigned arm64 iOS IPA for AltStore sideloading (runs on macOS with Xcode).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-ios"
OUT_IPA="${ROOT_DIR}/dist/rmexe-collection-ios-altstore.ipa"
BUNDLE_USER_ROMS="${BUNDLE_USER_ROMS:-0}"

if [ ! -f "${ROOT_DIR}/third_party/SDL2/CMakeLists.txt" ]; then
    echo "Downloading SDL2 2.30.11 into third_party/SDL2..."
    mkdir -p "${ROOT_DIR}/third_party"
    curl -fL --retry 3 -o /tmp/SDL2-2.30.11.tar.gz \
        https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz
    tar -xzf /tmp/SDL2-2.30.11.tar.gz -C "${ROOT_DIR}/third_party"
    rm -rf "${ROOT_DIR}/third_party/SDL2"
    mv "${ROOT_DIR}/third_party/SDL2-2.30.11" "${ROOT_DIR}/third_party/SDL2"
    rm -f /tmp/SDL2-2.30.11.tar.gz
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DRMEXE_BUILD_COLLECTION=ON

cmake --build "${BUILD_DIR}" --config Release --target RMEXECollection -- \
    -sdk iphoneos \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGN_IDENTITY=""

APP_PATH="$(find "${BUILD_DIR}" -name "RMEXECollection.app" -type d | head -n 1)"
if [ -z "${APP_PATH}" ]; then
    echo "ERROR: RMEXECollection.app not found under ${BUILD_DIR}" >&2
    exit 1
fi

# Bundle non-copyrighted fonts & translations into the .app bundle
mkdir -p "${APP_PATH}/assets/fonts" "${APP_PATH}/assets/translations"
cp -f "${ROOT_DIR}"/assets/fonts/*.ttf "${APP_PATH}/assets/fonts/" 2>/dev/null || true
cp -f "${ROOT_DIR}"/assets/fonts/*.ttc "${APP_PATH}/assets/fonts/" 2>/dev/null || true
cp -f "${ROOT_DIR}"/assets/fonts/*.otf "${APP_PATH}/assets/fonts/" 2>/dev/null || true
cp -f "${ROOT_DIR}/assets/translations/pon_zh.json" "${APP_PATH}/assets/translations/" 2>/dev/null || true
cp -f "${ROOT_DIR}/assets/translations/lon_zh.json" "${APP_PATH}/assets/translations/" 2>/dev/null || true

if [ "${BUNDLE_USER_ROMS}" = "1" ]; then
    for d in PoN_jp PoN LoN_jp LoN; do
        if [ -d "${ROOT_DIR}/assets/${d}" ]; then
            mkdir -p "${APP_PATH}/assets/${d}/bin" "${APP_PATH}/assets/${d}/sp"
            cp -f "${ROOT_DIR}/assets/${d}/bin/"*.jar "${APP_PATH}/assets/${d}/bin/" 2>/dev/null || true
            for spf in "${ROOT_DIR}/assets/${d}/sp/"*.sp; do
                case "${spf}" in *.orig.sp) continue ;; esac
                [ -f "${spf}" ] && cp -f "${spf}" "${APP_PATH}/assets/${d}/sp/"
            done
        fi
    done
fi

STAGE_DIR="${BUILD_DIR}/ipa_stage"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/Payload" "${ROOT_DIR}/dist"
cp -R "${APP_PATH}" "${STAGE_DIR}/Payload/RMEXECollection.app"
rm -f "${OUT_IPA}"
(cd "${STAGE_DIR}" && zip -qr "${OUT_IPA}" Payload)
echo "Built AltStore IPA: ${OUT_IPA}"
ls -lh "${OUT_IPA}"
