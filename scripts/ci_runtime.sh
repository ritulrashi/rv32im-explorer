#!/usr/bin/env bash
# Records the measured wall time of the GitHub Actions regression job for a
# commit (docs/phase6-design.md §4), from the GitHub API via the gh CLI.
#
#   scripts/ci_runtime.sh COMMIT_SHA > results/phase6/regression_runtime_github.txt
set -euo pipefail
sha=$(git rev-parse "$1")  # the API filter needs the full 40-character SHA
repo=$(gh repo view --json nameWithOwner -q .nameWithOwner)
run=$(gh run list --repo "$repo" --commit "$sha" --workflow regression.yml --json databaseId,event \
      -q 'map(select(.event == "push")) | .[0].databaseId')
echo "# GitHub Actions regression runtime"
echo "command: scripts/ci_runtime.sh $sha"
echo "repository: $repo"
echo "commit: $sha"
echo "run: https://github.com/$repo/actions/runs/$run"
gh run view "$run" --repo "$repo" --json conclusion,createdAt,updatedAt,jobs -q '
  "conclusion: \(.conclusion)",
  (.jobs[] | "job \(.name): \(.conclusion), started \(.startedAt), completed \(.completedAt)"),
  (.jobs[] | .steps[] | "  step \(.number) \(.name): \(.conclusion), \(.startedAt) -> \(.completedAt)")'
gh run view "$run" --repo "$repo" --json jobs -q '.jobs[] | select(.conclusion != "skipped") |
  "job \(.name) wall time (s):\(((.completedAt | fromdateiso8601) - (.startedAt | fromdateiso8601)))"'
echo "runner: ubuntu-24.04 (GitHub-hosted; public repository standard runner)"
