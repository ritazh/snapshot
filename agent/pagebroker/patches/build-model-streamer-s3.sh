#!/bin/sh
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Temporary AWS SDK dependency fix; remove once ModelStreamer ships it upstream.
# Arguments are the extracted ModelStreamer source, installed release SDK, and
# unmodified static dependencies from its pinned upstream build image.
set -eu
source_dir=$1
sdk_dir=$2
deps_dir=$3
patch_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
cd "$work_dir"

mkdir -p aws/src/aws-cpp-sdk-core/source/internal objects
cp "$sdk_dir/legal/source/AWSHttpResourceClient.cpp" aws/src/aws-cpp-sdk-core/source/internal/
patch --batch --fuzz=0 -p1 -d aws < "$patch_dir/aws-http-resource-ca.patch"
# Replace precisely the original archive member, rather than adding a second
# definition or rebuilding unrelated AWS/CRT/curl/OpenSSL dependencies.
archive="$deps_dir/aws/lib/libaws-cpp-sdk-core.a"
test "$(ar t "$archive" | grep -cx AWSHttpResourceClient.cpp.o)" -eq 1
c++ -O2 -fPIC -std=c++17 -pthread -I"$deps_dir/aws/include" \
    -c aws/src/aws-cpp-sdk-core/source/internal/AWSHttpResourceClient.cpp \
    -o AWSHttpResourceClient.cpp.o
ar r "$archive" AWSHttpResourceClient.cpp.o

while IFS= read -r source; do
    case "$source" in ''|'#'*) continue ;; esac
    object="objects/$(printf '%s' "$source" | tr / _).o"
    case "$source" in
        *.c) cc -O2 -fPIC -I"$source_dir/cpp" -c "$source_dir/cpp/$source" -o "$object" ;;
        *) c++ -O2 -fPIC -std=c++17 -pthread -I"$source_dir/cpp" \
            -I"$sdk_dir/include" -I"$deps_dir/aws/include" \
            -c "$source_dir/cpp/$source" -o "$object" ;;
    esac
done < "$patch_dir/model-streamer-s3-sources.txt"
c++ -static-libstdc++ -static-libgcc -shared \
    -Wl,--version-script="$source_dir/cpp/s3/s3.ldscript",--gc-sections \
    -Wl,--wrap=memcpy,--wrap=__fdelt_chk -o "$sdk_dir/lib/libstreamers3.so" objects/*.o \
    -L"$deps_dir/aws/lib" -L"$deps_dir/curl/lib" -L"$deps_dir/ssl/lib" -L"$deps_dir/zlib/lib" \
    -Wl,--start-group -l:libaws-cpp-sdk-s3-crt.a -l:libaws-cpp-sdk-core.a -l:libaws-crt-cpp.a \
    -l:libaws-c-mqtt.a -l:libaws-c-event-stream.a -l:libaws-c-s3.a -l:libaws-c-auth.a \
    -l:libaws-c-http.a -l:libaws-c-io.a -l:libs2n.a -l:libaws-c-compression.a -l:libaws-c-cal.a \
    -l:libaws-c-sdkutils.a -l:libaws-checksums.a -l:libaws-c-common.a \
    -l:libcurl.a -l:libssl.a -l:libcrypto.a -l:libz.a -Wl,--end-group -lpthread -ldl -lm -lrt
# Ship the patch and recipe with the matching source archives for attribution
# and reproducibility; SDK.sha256 continues to identify the original release.
cp "$patch_dir/aws-http-resource-ca.patch" "$patch_dir/build-model-streamer-s3.sh" \
    "$patch_dir/model-streamer-s3-sources.txt" "$sdk_dir/legal/source/"
sha256sum "$sdk_dir/lib/libstreamers3.so" > "$sdk_dir/legal/PATCHED-S3.sha256"
