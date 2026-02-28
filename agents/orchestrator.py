#!/usr/bin/env python3
"""
Orchestrator Agent
------------------
Central coordinator of the coding agent network.
Dispatches tasks to specialised agents by triggering GitHub Actions
workflow_dispatch events via the GitHub REST API.

Environment variables (provided by GitHub Actions):
  GITHUB_TOKEN  – token with `repo` + `workflow` scopes
  GITHUB_REPO   – owner/repo  (e.g. "Joosibaeri/cloudphone")
  AGENT_TASK    – task to dispatch: "code-review" | "issue-triage" | "all"
  EXTRA_INPUTS  – optional JSON string forwarded to the target workflow
"""

import json
import os
import sys
import urllib.request
import urllib.error


def _api_request(method: str, path: str, body: dict | None = None) -> dict:
    token = os.environ["GITHUB_TOKEN"]
    url = f"https://api.github.com{path}"
    data = json.dumps(body).encode() if body else None
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
        print(f"[orchestrator] HTTP {exc.code} – {body_text}", file=sys.stderr)
        raise


def dispatch_workflow(repo: str, workflow_file: str, ref: str = "main",
                      inputs: dict | None = None) -> None:
    """Trigger a workflow_dispatch event on the given workflow."""
    payload = {"ref": ref, "inputs": inputs or {}}
    _api_request("POST", f"/repos/{repo}/actions/workflows/{workflow_file}/dispatches",
                 body=payload)
    print(f"[orchestrator] Dispatched: {workflow_file}")


def list_open_prs(repo: str) -> list[dict]:
    return _api_request("GET", f"/repos/{repo}/pulls?state=open&per_page=20")


def list_open_issues(repo: str) -> list[dict]:
    return _api_request("GET", f"/repos/{repo}/issues?state=open&per_page=20")


def run() -> None:
    repo = os.environ.get("GITHUB_REPO", "")
    task = os.environ.get("AGENT_TASK", "all").lower()
    extra_inputs_raw = os.environ.get("EXTRA_INPUTS", "{}")
    extra_inputs: dict = json.loads(extra_inputs_raw)

    if not repo:
        sys.exit("[orchestrator] GITHUB_REPO is not set")

    print(f"[orchestrator] Starting – repo={repo}  task={task}")

    if task in ("code-review", "all"):
        open_prs = list_open_prs(repo)
        print(f"[orchestrator] Open PRs: {len(open_prs)}")
        for pr in open_prs:
            dispatch_workflow(
                repo,
                "agent-code-review.yml",
                ref="main",
                inputs={"pr_number": str(pr["number"]), **extra_inputs},
            )

    if task in ("issue-triage", "all"):
        open_issues = [i for i in list_open_issues(repo) if "pull_request" not in i]
        print(f"[orchestrator] Open issues (non-PR): {len(open_issues)}")
        for issue in open_issues:
            dispatch_workflow(
                repo,
                "agent-issue-triage.yml",
                inputs={"issue_number": str(issue["number"]), **extra_inputs},
            )

    print("[orchestrator] All tasks dispatched.")


if __name__ == "__main__":
    run()
