#!/bin/sh
set -eu

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if ! command -v gitleaks >/dev/null 2>&1; then
    echo "error: gitleaks is required (https://github.com/gitleaks/gitleaks)" >&2
    exit 127
fi

mode=${1:-history}
case "$mode" in
    install)
        git config core.hooksPath .githooks
        printf '%s\n' "configured core.hooksPath=.githooks"
        ;;
    history)
        exec gitleaks git --no-banner --redact .
        ;;
    staged)
        exec gitleaks git --no-banner --redact --staged .
        ;;
    range)
        if [ "$#" -ne 2 ] || [ -z "$2" ]; then
            echo "usage: $0 range '<git-log-options>'" >&2
            exit 2
        fi
        exec gitleaks git --no-banner --redact --log-opts="$2" .
        ;;
    *)
        echo "usage: $0 [install|history|staged|range '<git-log-options>']" >&2
        exit 2
        ;;
esac
