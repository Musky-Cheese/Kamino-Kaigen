#!/usr/bin/env bash
# Publish the web build to GitHub Pages: release wasm build + the cooked files the game reaches, pushed on gh-pages.
# Usage (Git Bash, from the project root):  bash tools/publish_web.sh [--no-build] [--no-push]
set -e
cd "$(dirname "$0")/.."
BUILD=1; PUSH=1
for a in "$@"; do case "$a" in --no-build) BUILD=0;; --no-push) PUSH=0;; esac; done
[ "$BUILD" = 1 ] && cmd //c "hz\hzbuild wasm --release"
P=out/pages
if [ ! -d "$P/.git" ] && [ ! -f "$P/.git" ]; then
  git fetch origin gh-pages 2>/dev/null && git worktree add "$P" gh-pages || { git worktree add --detach "$P"; git -C "$P" checkout --orphan gh-pages; git -C "$P" rm -rfq .; }
fi
find "$P" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
for f in main.js main_worker.js worker.js audio_worklet.js mic_worklet.js game.wasm; do cp "out/wasm/release/$f" "$P/$f"; done
cp tools/web/index.html "$P/index.html"
cp tools/web/coi-serviceworker.min.js "$P/coi-serviceworker.min.js"
cp hz/tools/publish/default_icon.png "$P/favicon.png"
touch "$P/.nojekyll"
# reachable cooked files: no other GPU backends, no png fallbacks, no unused template props, only the font pages the UI uses
{
  ls cooked/catalog.hza cooked/catalog.index cooked/cook_stamps.index
  find cooked/engine -type f ! -path "cooked/engine/fonts/*"
  echo cooked/engine/fonts/inter/inter.hza
  for w in Regular Medium SemiBold Bold Italic; do ls cooked/engine/fonts/inter/Inter-$w.hza cooked/engine/fonts/inter/Inter-$w.page000.hza cooked/engine/fonts/inter/Inter-$w.page001.hza; done
  ls cooked/engine/fonts/inter/Inter-Regular.page007.hza
  find cooked/gameplay_blocks -type f
  find cooked/shaders -type f -name "*_webgpu.hza"
  find cooked/shaders -type f ! -name "*_webgpu.hza" ! -name "*_vulkan.hza" ! -name "*_d3d12.hza"
  find cooked/proving_grounds -type f ! -path "cooked/proving_grounds/models/core/*" ! -path "cooked/proving_grounds/models/jumppad/*" ! -name "*.png"
  find cooked/kamino -type f ! -name "*.png"
} | sort -u | while read f; do mkdir -p "$P/$(dirname "$f")"; cp "$f" "$P/$f"; done
du -sh "$P" --exclude=.git
git -C "$P" add -A
git -C "$P" commit -q -m "Web build $(git rev-parse --short HEAD): $(git log -1 --format=%s)" || echo "nothing changed"
[ "$PUSH" = 1 ] && git -C "$P" push -u origin gh-pages
echo "https://musky-cheese.github.io/Kamino-Kaigen/"
