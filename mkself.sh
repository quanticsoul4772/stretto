#!/bin/bash
cat > synth.packed <<'STUB'
#!/bin/bash
T=$(mktemp)
tail -n+9 "$0" | xz -dc > "$T"
chmod +x "$T"
"$T" "$@"
R=$?
rm "$T"
exit $R
STUB
cat synth.xz >> synth.packed
chmod +x synth.packed
