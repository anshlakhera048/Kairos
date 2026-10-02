#!/usr/bin/env bash
# Installs .git/hooks/pre-commit: format check + debug build + tests.
# Run once per clone: ./scripts/install-hooks.sh
set -euo pipefail

HOOK=".git/hooks/pre-commit"
if [ ! -d ".git" ]; then
    echo "not a git checkout (no .git); skipping hook install" >&2
    exit 1
fi

cat > "$HOOK" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
echo "[pre-commit] clang-format check..."
./scripts/format.sh --check
echo "[pre-commit] build + test (debug)..."
cmake --preset debug >/dev/null
cmake --build --preset debug
ctest --preset debug --output-on-failure
echo "[pre-commit] OK"
EOF
chmod +x "$HOOK"
echo "installed $HOOK"
