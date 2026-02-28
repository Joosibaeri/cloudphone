#!/usr/bin/env python3
"""
Code-Review Agent
-----------------
Fetches the diff of a pull request, analyses changed files, and posts
a structured review comment via the GitHub REST API.

Environment variables (provided by GitHub Actions):
  GITHUB_TOKEN  – token with `pull_requests` write scope
  GITHUB_REPO   – owner/repo  (e.g. "Joosibaeri/cloudphone")
  PR_NUMBER     – the pull-request number to review
"""

import json
import os
import re
import sys
import urllib.request
import urllib.error


# ---------------------------------------------------------------------------
# GitHub API helpers
# ---------------------------------------------------------------------------

def _api_request(method: str, path: str, body: dict | None = None,
                 accept: str = "application/vnd.github+json") -> dict | list | str:
    token = os.environ["GITHUB_TOKEN"]
    url = f"https://api.github.com{path}"
    data = json.dumps(body).encode() if body else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": accept,
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with urllib.request.urlopen(req) as resp:
            raw = resp.read()
            if accept == "application/vnd.github.diff":
                return raw.decode(errors="replace")
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode(errors="replace")
        print(f"[code-review] HTTP {exc.code} – {body_text}", file=sys.stderr)
        raise


def get_pr(repo: str, pr_number: int) -> dict:
    return _api_request("GET", f"/repos/{repo}/pulls/{pr_number}")


def get_pr_diff(repo: str, pr_number: int) -> str:
    return _api_request(
        "GET",
        f"/repos/{repo}/pulls/{pr_number}",
        accept="application/vnd.github.diff",
    )


def post_pr_review(repo: str, pr_number: int, body: str, event: str = "COMMENT") -> None:
    _api_request(
        "POST",
        f"/repos/{repo}/pulls/{pr_number}/reviews",
        body={"body": body, "event": event},
    )


# ---------------------------------------------------------------------------
# Analysis helpers
# ---------------------------------------------------------------------------

_ISSUES: list[tuple[re.Pattern, str]] = [
    (re.compile(r"\bTODO\b"),             "⚠️  Unresolved TODO comment found."),
    (re.compile(r"\bFIXME\b"),            "🔴 FIXME marker – please address before merging."),
    (re.compile(r"\bHACK\b"),             "🟠 HACK marker – consider a cleaner approach."),
    (re.compile(r"(?:password|passwd|secret|api_key)\s*=\s*['\"][^'\"]{4,}['\"]", re.I),
                                           "🔒 Possible hard-coded credential – use secrets instead."),
    (re.compile(r"printf\s*\(\s*[a-zA-Z_][a-zA-Z0-9_]*\s*\)"),
                                           "🛡️  Potential format-string vulnerability: user input passed as format string."),
    (re.compile(r"gets\s*\("),            "🛡️  Unsafe `gets()` call – use `fgets()` instead."),
    (re.compile(r"strcpy\s*\("),          "🛡️  Unsafe `strcpy()` – consider `strncpy()` or `strlcpy()`."),
]


def analyse_diff(diff: str) -> list[str]:
    """Return a list of human-readable finding strings."""
    findings: list[str] = []
    for line in diff.splitlines():
        if not line.startswith("+"):
            continue  # only inspect added lines
        code = line[1:]
        for pattern, message in _ISSUES:
            if pattern.search(code):
                findings.append(f"`{code.strip()}` → {message}")
    return findings


def summarise_diff(diff: str) -> dict[str, int]:
    """Return a summary of changed files and line counts."""
    files: dict[str, int] = {}
    current_file = ""
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            current_file = line[6:]
            files.setdefault(current_file, 0)
        elif line.startswith("+") and not line.startswith("+++"):
            files[current_file] = files.get(current_file, 0) + 1
    return files


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run() -> None:
    repo = os.environ.get("GITHUB_REPO", "")
    pr_number_raw = os.environ.get("PR_NUMBER", "")

    if not repo or not pr_number_raw:
        sys.exit("[code-review] GITHUB_REPO and PR_NUMBER must be set.")

    pr_number = int(pr_number_raw)
    print(f"[code-review] Reviewing PR #{pr_number} in {repo}")

    pr = get_pr(repo, pr_number)
    diff: str = get_pr_diff(repo, pr_number)

    findings = analyse_diff(diff)
    file_summary = summarise_diff(diff)

    # Build the review body
    lines: list[str] = [
        "## 🤖 Automated Code Review",
        "",
        "### Changed files",
        "",
    ]
    for fname, added_lines in file_summary.items():
        lines.append(f"- `{fname}` (+{added_lines} lines)")

    lines += ["", "### Findings", ""]
    if findings:
        for f in findings:
            lines.append(f"- {f}")
    else:
        lines.append("✅ No automated issues detected.")

    lines += [
        "",
        "---",
        f"_Reviewed by the **CloudPhone Code-Review Agent** – PR #{pr_number}_",
    ]

    review_body = "\n".join(lines)
    post_pr_review(repo, pr_number, review_body)
    print(f"[code-review] Review posted for PR #{pr_number}.")
    if findings:
        print(f"[code-review] {len(findings)} issue(s) found.")


if __name__ == "__main__":
    run()
