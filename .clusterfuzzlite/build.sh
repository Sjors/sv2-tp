#!/usr/bin/env bash

export LC_ALL=C
set -eu

date

cd "$SRC/sv2-tp"

# Keep ccache outputs on the runner workspace so cache save can pick them up.
if [ -n "${GITHUB_WORKSPACE:-}" ] && [ -n "${CCACHE_DIR:-}" ]; then
  host_ccache_dir="${GITHUB_WORKSPACE}/.cfl-ccache"
  mkdir -p "$host_ccache_dir"
  if [ "$CCACHE_DIR" != "$host_ccache_dir" ]; then
    mkdir -p "$(dirname "$CCACHE_DIR")"
    if [ -e "$CCACHE_DIR" ] || [ -L "$CCACHE_DIR" ]; then
      rm -rf "$CCACHE_DIR"
    fi
    ln -s "$host_ccache_dir" "$CCACHE_DIR"
  fi
fi

SANITIZER_CHOICE="${SANITIZER:-address}"

# Surface ClusterFuzzLite-provided toolchain flags for visibility and auditing.
echo "[cfl] toolchain env:" >&2
echo "  CC=${CC:-}"
echo "  CXX=${CXX:-}"
echo "  CFLAGS=${CFLAGS:-}"
echo "  CXXFLAGS=${CXXFLAGS:-}"
echo "  LIB_FUZZING_ENGINE=${LIB_FUZZING_ENGINE:-}"
echo "  SANITIZER=${SANITIZER:-}"

export BUILD_TRIPLET="x86_64-pc-linux-gnu"
export CFLAGS="${CFLAGS:-} -flto=full"
export CXXFLAGS="${CXXFLAGS:-} -flto=full"
export LDFLAGS="-fuse-ld=lld -flto=full ${LDFLAGS:-}"
export CPPFLAGS="${CPPFLAGS:-} -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG"

FUZZ_LIBS_VALUE="$LIB_FUZZING_ENGINE"
DEFAULT_LIBCPP_DIR=""
SYSTEM_LIBSTDCPP_DIR=""
SYSTEM_LIBSTDCPP_FILE=""

CXX_BIN="${CXX:-clang++}"
if command -v "$CXX_BIN" >/dev/null 2>&1; then
  libcxx_archive="$("$CXX_BIN" -print-file-name=libc++.a 2>/dev/null || true)"
  if [ -n "$libcxx_archive" ] && [ "$libcxx_archive" != "libc++.a" ]; then
    DEFAULT_LIBCPP_DIR="$(dirname "$libcxx_archive")"
  fi
  stdcpp_path="$("$CXX_BIN" -print-file-name=libstdc++.so 2>/dev/null || true)"
  if [ -z "$stdcpp_path" ] || [ "$stdcpp_path" = "libstdc++.so" ]; then
    stdcpp_path="$("$CXX_BIN" -print-file-name=libstdc++.a 2>/dev/null || true)"
  fi
  if [ -n "$stdcpp_path" ] && [ "$stdcpp_path" != "libstdc++.so" ] && [ "$stdcpp_path" != "libstdc++.a" ]; then
    SYSTEM_LIBSTDCPP_DIR="$(dirname "$stdcpp_path")"
    SYSTEM_LIBSTDCPP_FILE="$stdcpp_path"
  fi
fi

if [ -z "$SYSTEM_LIBSTDCPP_DIR" ] && command -v g++ >/dev/null 2>&1; then
  stdcpp_path="$(g++ -print-file-name=libstdc++.so 2>/dev/null || true)"
  if [ -z "$stdcpp_path" ] || [ "$stdcpp_path" = "libstdc++.so" ]; then
    stdcpp_path="$(g++ -print-file-name=libstdc++.a 2>/dev/null || true)"
  fi
  if [ -n "$stdcpp_path" ] && [ "$stdcpp_path" != "libstdc++.so" ] && [ "$stdcpp_path" != "libstdc++.a" ]; then
    SYSTEM_LIBSTDCPP_DIR="$(dirname "$stdcpp_path")"
    SYSTEM_LIBSTDCPP_FILE="$stdcpp_path"
  fi
fi

declare -a LIB_SEARCH_PATHS=()
if [ -n "$SYSTEM_LIBSTDCPP_DIR" ] && [ -d "$SYSTEM_LIBSTDCPP_DIR" ]; then
  LIB_SEARCH_PATHS+=("$SYSTEM_LIBSTDCPP_DIR")
fi
if [ -n "$DEFAULT_LIBCPP_DIR" ] && [ -d "$DEFAULT_LIBCPP_DIR" ]; then
  if [ "$DEFAULT_LIBCPP_DIR" != "$SYSTEM_LIBSTDCPP_DIR" ]; then
    LIB_SEARCH_PATHS+=("$DEFAULT_LIBCPP_DIR")
  fi
fi

if [ ${#LIB_SEARCH_PATHS[@]} -gt 0 ]; then
  local_path_list="${LIBRARY_PATH:-}"
  local_ld_path="${LD_LIBRARY_PATH:-}"
  for search_dir in "${LIB_SEARCH_PATHS[@]}"; do
    if [ -n "$local_path_list" ]; then
      local_path_list="${search_dir}:${local_path_list}"
    else
      local_path_list="$search_dir"
    fi
    if [ -n "$local_ld_path" ]; then
      local_ld_path="${search_dir}:${local_ld_path}"
    else
      local_ld_path="$search_dir"
    fi
    export LDFLAGS="${LDFLAGS} -L${search_dir}"
  done
  export LIBRARY_PATH="$local_path_list"
  export LD_LIBRARY_PATH="$local_ld_path"
  if [ -n "$DEFAULT_LIBCPP_DIR" ]; then
    export LDFLAGS="${LDFLAGS} -Wl,-rpath,${DEFAULT_LIBCPP_DIR}"
  fi
fi
if [ -n "$SYSTEM_LIBSTDCPP_FILE" ] && [ -e "$SYSTEM_LIBSTDCPP_FILE" ]; then
  if [ -n "$FUZZ_LIBS_VALUE" ]; then
    FUZZ_LIBS_VALUE="${FUZZ_LIBS_VALUE};${SYSTEM_LIBSTDCPP_FILE}"
  else
    FUZZ_LIBS_VALUE="$SYSTEM_LIBSTDCPP_FILE"
  fi
else
  if [ -n "$FUZZ_LIBS_VALUE" ]; then
    FUZZ_LIBS_VALUE="${FUZZ_LIBS_VALUE};-lstdc++"
  else
    FUZZ_LIBS_VALUE="-lstdc++"
  fi
fi

DEPENDS_PREFIX_DIR="depends/${BUILD_TRIPLET}"
DEPENDS_TOOLCHAIN_PATH="${DEPENDS_PREFIX_DIR}/toolchain.cmake"
DEPENDS_STAMP_PRESENT=0

if [ -d "$DEPENDS_PREFIX_DIR" ] && [ -f "$DEPENDS_TOOLCHAIN_PATH" ]; then
  if find "$DEPENDS_PREFIX_DIR" -maxdepth 1 -type f -name '.stamp_*' -print -quit >/dev/null 2>&1; then
    DEPENDS_STAMP_PRESENT=1
  fi
fi

NEED_DEPENDS_BUILD=1
if [ "${FORCE_DEPENDS_BUILD:-0}" = "1" ]; then
  NEED_DEPENDS_BUILD=1
elif [ "$DEPENDS_STAMP_PRESENT" -eq 1 ]; then
  echo "Using cached depends outputs at ${DEPENDS_PREFIX_DIR}; skipping depends make step."
  NEED_DEPENDS_BUILD=0
fi

if [ "$NEED_DEPENDS_BUILD" -eq 1 ]; then
  (
    cd depends
    sed -i --regexp-extended '/.*rm -rf .*extract_dir.*/d' ./funcs.mk || true

    # Mirror the MSan depends invocation from ci/test/00_setup_env_native_fuzz_with_msan.sh
    # so that dependencies pick up the sanitizer-friendly toolchain.
    make \
      HOST=$BUILD_TRIPLET \
      DEBUG=1 \
      NO_IPC=1 \
      LOG=1 \
      CC=clang \
      CXX=clang++ \
      CFLAGS="$CFLAGS" \
      CXXFLAGS="$CXXFLAGS" \
      AR=llvm-ar \
      NM=llvm-nm \
      RANLIB=llvm-ranlib \
      STRIP=llvm-strip \
      -j"$(nproc)"
  )
fi

EXTRA_CMAKE_ARGS=()
if [ "$SANITIZER_CHOICE" = "memory" ]; then
  EXTRA_CMAKE_ARGS+=("-DAPPEND_CPPFLAGS=-U_FORTIFY_SOURCE")
fi

cmake -B build_fuzz \
  --toolchain "depends/${BUILD_TRIPLET}/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="${CC:-clang}" \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="" \
  -DCMAKE_C_FLAGS="${CFLAGS:-}" \
  -DCMAKE_CXX_FLAGS="${CXXFLAGS:-}" \
  -DBUILD_FOR_FUZZING=ON \
  -DBUILD_FUZZ_BINARY=ON \
  -DFUZZ_BINARY_LINKS_WITHOUT_MAIN_FUNCTION=ON \
  -DFUZZ_LIBS="${FUZZ_LIBS_VALUE:-${LIB_FUZZING_ENGINE:-}}" \
  -DSANITIZERS="$SANITIZER_CHOICE" \
  -DCMAKE_VERBOSE_MAKEFILE=ON \
  "${EXTRA_CMAKE_ARGS[@]}"

cmake --build build_fuzz -j"$(nproc)"

# First execution happens inside the build container so we can enumerate targets before bundling.
# The later "bad build" replay runs in a stripped sandbox with only bundled files, so passing here
# doesn't guarantee all runtime artefacts are packaged correctly—that check happens post-bundle.
WRITE_ALL_FUZZ_TARGETS_AND_ABORT="$WORK/fuzz_targets.txt" ./build_fuzz/bin/fuzz || true
readarray -t FUZZ_TARGETS < "$WORK/fuzz_targets.txt" || FUZZ_TARGETS=()

if [ ${#FUZZ_TARGETS[@]} -eq 0 ]; then
  echo "no fuzz targets discovered" >&2
  exit 1
fi

# Must match FuzzTargetPlaceholder in src/test/fuzz/fuzz.cpp so the python
# patching below can locate the placeholder string.
MAGIC_STR="d6f1a2b39c4e5d7a8b9c0d1e2f30415263748596a1b2c3d4e5f60718293a4b5c6d7e8f90112233445566778899aabbccddeeff00fedcba9876543210a0b1c2d3"

for fuzz_target in "${FUZZ_TARGETS[@]}"; do
  [ -z "$fuzz_target" ] && continue
  python3 - << PY
c_str_target=b"${fuzz_target}\x00"
c_str_magic=b"$MAGIC_STR"
with open('./build_fuzz/bin/fuzz','rb') as f:
    dat=f.read()
dat=dat.replace(c_str_magic, c_str_target + c_str_magic[len(c_str_target):])
with open("$OUT/${fuzz_target}", 'wb') as g:
    g.write(dat)
PY
  chmod +x "$OUT/${fuzz_target}"

  corpus_dir="assets/fuzz_corpora/${fuzz_target}"
  if [ -d "$corpus_dir" ] && find "$corpus_dir" -type f -print -quit >/dev/null 2>&1; then
    (
      cd "$corpus_dir"
      zip --recurse-paths --quiet --junk-paths "$OUT/${fuzz_target}_seed_corpus.zip" .
    )
  fi

done

# Leave a marker so sandboxed bad-build checks can recognise ClusterFuzzLite bundles.
: >"$OUT/.sv2-clusterfuzzlite"

if [ -d assets/fuzz_dicts ]; then
  find assets/fuzz_dicts -maxdepth 1 -type f -name '*.dict' -exec cp {} "$OUT/" \;
fi

if [ -d "$OUT" ]; then
  echo "ClusterFuzzLite bundle tree (find $OUT -maxdepth 2):"
  find "$OUT" -maxdepth 2 -print | sort
fi
