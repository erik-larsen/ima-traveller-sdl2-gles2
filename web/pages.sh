#!/bin/sh
# Publish the current web build to the gh-pages branch (GitHub Pages:
# https://erik-larsen.github.io/ima-traveller-sdl2-gles2/).  Run via
# `make pages`, which rebuilds web/traveller.js + traveller.wasm first.
#
# The branch holds only the built page: traveller.html as index.html, the
# .js/.wasm, the vendored libraries' licenses, and .nojekyll.  Commits only
# when something changed, then pushes.
#
# Override via env: PAGES_BRANCH=... PAGES_REMOTE=...
set -e
cd "$(dirname "$0")/.."

BRANCH=${PAGES_BRANCH:-gh-pages}
REMOTE=${PAGES_REMOTE:-origin}
REV=$(git describe --always --dirty)

for f in web/traveller.html web/traveller.js web/traveller.wasm; do
    test -f "$f" || { echo "ERROR: $f not found -- run make browser"; exit 1; }
done

TMP=$(mktemp -d)
trap 'git worktree remove --force "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT

git fetch -q "$REMOTE" "$BRANCH"
git worktree add -q -B "$BRANCH" "$TMP" "$REMOTE/$BRANCH"

cp web/traveller.html "$TMP/index.html"
cp web/traveller.js web/traveller.wasm "$TMP/"
touch "$TMP/.nojekyll"
{
    printf 'IMA Traveller (traveller 2.0c, 1996) by Erwin Driessens & Maria Verstappen.\n'
    printf "Browser port by Erik Larsen, published with Erwin Driessens's permission.\n\n"
    printf 'This build includes the following libraries:\n\n'
    for f in freeglut/COPYING gl4es/LICENSE angle/LICENSE; do
        printf '==== %s ====\n\n' "${f%%/*}"
        cat "deps/$f"
        printf '\n\n'
    done
} > "$TMP/LICENSES.txt"

cd "$TMP"
git add -A
if git diff --cached --quiet; then
    echo "pages: $BRANCH already matches this build ($REV)"
    exit 0
fi
git commit -q -m "Web build of IMA Traveller (from main $REV)"
git push -q "$REMOTE" "$BRANCH"
echo "pages: pushed $BRANCH ($REV) -- live in a minute or so at"
echo "  https://erik-larsen.github.io/ima-traveller-sdl2-gles2/"
