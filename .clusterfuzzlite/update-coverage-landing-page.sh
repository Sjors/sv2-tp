#!/usr/bin/env bash
export LC_ALL=C
set -euo pipefail

if [ -z "${PERSONAL_ACCESS_TOKEN:-}" ]; then
  echo "PERSONAL_ACCESS_TOKEN not set; skipping landing page update." >&2
  exit 0
fi

if [ -z "${STORAGE_REPO:-}" ]; then
  echo "STORAGE_REPO not set; cannot refresh coverage landing page." >&2
  exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

git clone --depth 1 --branch gh-pages "$STORAGE_REPO" "$tmpdir"

install -m 0644 ".clusterfuzzlite/index.html" "$tmpdir/index.html"

touch "$tmpdir/.nojekyll"
git -C "$tmpdir" add index.html .nojekyll

if git -C "$tmpdir" diff --cached --quiet; then
  exit 0
fi

author_name="$(git -C "$tmpdir" log -1 --pretty=%an 2>/dev/null || true)"
author_email="$(git -C "$tmpdir" log -1 --pretty=%ae 2>/dev/null || true)"
if [ -z "$author_name" ] && [ -n "${GIT_AUTHOR_NAME:-}" ]; then
  author_name="$GIT_AUTHOR_NAME"
fi
if [ -z "$author_email" ] && [ -n "${GIT_AUTHOR_EMAIL:-}" ]; then
  author_email="$GIT_AUTHOR_EMAIL"
fi
if [ -z "$author_name" ] && [ -n "${GITHUB_ACTOR:-}" ]; then
  author_name="$GITHUB_ACTOR"
fi
if [ -z "$author_email" ] && [ -n "${GITHUB_ACTOR:-}" ]; then
  author_email="${GITHUB_ACTOR}@users.noreply.github.com"
fi
if [ -n "$author_name" ]; then
  git -C "$tmpdir" config user.name "$author_name"
fi
if [ -n "$author_email" ]; then
  git -C "$tmpdir" config user.email "$author_email"
fi

git -C "$tmpdir" commit -m "coverage: refresh landing page"
git -C "$tmpdir" push origin gh-pages
