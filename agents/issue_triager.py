#!/usr/bin/env python3
"""
Issue-Triage Agent
------------------
Reads new GitHub issues, classifies them by keyword matching, and
applies appropriate labels via the GitHub REST API.

Environment variables (provided by GitHub Actions):
  GITHUB_TOKEN   – token with `issues` write scope
  GITHUB_REPO    – owner/repo  (e.g. "Joosibaeri/cloudphone")
  ISSUE_NUMBER   – the issue number to triage
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

def _api_request(method: str, path: str, body=None) -> dict | list:
    token = os.environ["GITHUB_TOKEN"]
    url = f"https://api.github.com{path}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "Content-Type": "application/json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with urllib.request.urlopen(req) as resp:
            raw = resp.read()
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        body_text = exc.read().decode(errors="replace")
        print(f"[issue-triage] HTTP {exc.code} – {body_text}", file=sys.stderr)
        raise


def get_issue(repo: str, issue_number: int) -> dict:
    return _api_request("GET", f"/repos/{repo}/issues/{issue_number}")


def get_labels(repo: str) -> list[str]:
    labels = _api_request("GET", f"/repos/{repo}/labels?per_page=100")
    return [lbl["name"] for lbl in labels]


def create_label(repo: str, name: str, color: str, description: str) -> None:
    try:
        _api_request("POST", f"/repos/{repo}/labels",
                     body={"name": name, "color": color, "description": description})
        print(f"[issue-triage] Created label: {name}")
    except urllib.error.HTTPError:
        pass  # label may already exist


def add_labels(repo: str, issue_number: int, labels: list[str]) -> None:
    _api_request("POST", f"/repos/{repo}/issues/{issue_number}/labels",
                 body={"labels": labels})


def post_comment(repo: str, issue_number: int, body: str) -> None:
    _api_request("POST", f"/repos/{repo}/issues/{issue_number}/comments",
                 body={"body": body})


# ---------------------------------------------------------------------------
# Classification rules
# ---------------------------------------------------------------------------

_RULES: list[tuple[str, str, str, list[str]]] = [
    # (label_name, hex_color, description, keywords)
    ("bug",         "d73a4a", "Something isn't working",
     ["bug", "crash", "error", "exception", "fail", "broken", "segfault", "sigsegv"]),
    ("enhancement", "a2eeef", "New feature or request",
     ["feature", "enhance", "improvement", "request", "add", "support for"]),
    ("question",    "d876e3", "Further information is requested",
     ["question", "how to", "help", "clarification", "does it", "can i", "?"]),
    ("documentation","0075ca", "Improvements or additions to documentation",
     ["docs", "documentation", "readme", "wiki", "comment", "typo"]),
    ("security",    "ee0701", "Security-related concern",
     ["security", "vulnerability", "cve", "auth", "injection", "overflow", "leak"]),
    ("build",       "e4e669", "Build or CI related",
     ["build", "compile", "ci", "workflow", "makefile", "cmake", "linker"]),
    ("performance", "fbca04", "Performance-related concern",
     ["performance", "slow", "latency", "memory", "cpu", "speed", "optimise", "optimize"]),
]


def classify(title: str, body: str) -> list[str]:
    """Return list of label names that match the issue content."""
    text = (title + " " + (body or "")).lower()
    matched: list[str] = []
    for label_name, _color, _desc, keywords in _RULES:
        if any(kw in text for kw in keywords):
            matched.append(label_name)
    return matched or ["needs-triage"]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run() -> None:
    repo = os.environ.get("GITHUB_REPO", "")
    issue_number_raw = os.environ.get("ISSUE_NUMBER", "")

    if not repo or not issue_number_raw:
        sys.exit("[issue-triage] GITHUB_REPO and ISSUE_NUMBER must be set.")

    issue_number = int(issue_number_raw)
    print(f"[issue-triage] Triaging issue #{issue_number} in {repo}")

    issue = get_issue(repo, issue_number)
    title: str = issue.get("title", "")
    body: str = issue.get("body", "") or ""

    labels_to_apply = classify(title, body)
    print(f"[issue-triage] Classified as: {labels_to_apply}")

    # Ensure all required labels exist
    existing_labels = get_labels(repo)
    _color_map = {r[0]: (r[1], r[2]) for r in _RULES}
    _color_map["needs-triage"] = ("ededed", "Awaiting agent triage")
    for lbl in labels_to_apply:
        if lbl not in existing_labels:
            color, description = _color_map.get(lbl, ("cccccc", ""))
            create_label(repo, lbl, color, description)

    add_labels(repo, issue_number, labels_to_apply)

    comment = (
        "## 🤖 Automated Issue Triage\n\n"
        f"I've automatically labelled this issue as: "
        + ", ".join(f"`{l}`" for l in labels_to_apply)
        + ".\n\n"
        "_If the labels are incorrect, please update them manually._\n\n"
        "---\n"
        f"_Triaged by the **CloudPhone Issue-Triage Agent** – issue #{issue_number}_"
    )
    post_comment(repo, issue_number, comment)
    print(f"[issue-triage] Labels applied and comment posted for issue #{issue_number}.")


if __name__ == "__main__":
    run()
