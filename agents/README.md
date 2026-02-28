# CloudPhone – GitHub Coding Agent Network

A lightweight network of automated coding agents that runs entirely inside
GitHub Actions. Each agent is a small Python script triggered by the
corresponding workflow file.

```
┌──────────────────────────────────────────────────────┐
│                   Orchestrator                       │
│  agents/orchestrator.py                              │
│  .github/workflows/agent-orchestrator.yml            │
│                                                      │
│  • Runs daily (06:00 UTC) or on demand               │
│  • Discovers open PRs and issues                     │
│  • Dispatches the other agents via workflow_dispatch │
└───────────┬──────────────────────────┬───────────────┘
            │                          │
            ▼                          ▼
┌───────────────────────┐  ┌──────────────────────────┐
│  Code-Review Agent    │  │  Issue-Triage Agent       │
│  agents/              │  │  agents/                  │
│    code_reviewer.py   │  │    issue_triager.py       │
│  .github/workflows/   │  │  .github/workflows/       │
│   agent-code-         │  │   agent-issue-triage.yml  │
│   review.yml          │  │                           │
│                       │  │  • Triggers: issues.opened│
│  • Triggers: PR open/ │  │    or workflow_dispatch   │
│    sync or            │  │  • Classifies issue by    │
│    workflow_dispatch  │  │    keyword matching       │
│  • Analyses diff for  │  │  • Creates missing labels │
│    TODOs, FIXMEs,     │  │  • Applies labels         │
│    unsafe C calls,    │  │  • Posts triage comment   │
│    hard-coded secrets │  │                           │
│  • Posts review       │  │                           │
└───────────────────────┘  └──────────────────────────┘
```

## Agents

| Agent | Script | Workflow |
|---|---|---|
| Orchestrator | `agents/orchestrator.py` | `.github/workflows/agent-orchestrator.yml` |
| Code Review | `agents/code_reviewer.py` | `.github/workflows/agent-code-review.yml` |
| Issue Triage | `agents/issue_triager.py` | `.github/workflows/agent-issue-triage.yml` |

## How it works

### Orchestrator
- Runs every day at 06:00 UTC via a cron schedule, or on demand via
  **Actions → Agent – Orchestrator → Run workflow**.
- Fetches all open pull requests and issues from the GitHub API.
- Dispatches `agent-code-review.yml` for each open PR and
  `agent-issue-triage.yml` for each open issue.

### Code-Review Agent
- Triggered automatically whenever a PR is opened, updated, or reopened.
- Downloads the PR diff and scans each **added** line for:
  - `TODO` / `FIXME` / `HACK` markers
  - Hard-coded passwords
  - Unsafe C calls (`printf %s`, `gets`, `strcpy`)
- Posts a structured review comment on the PR with a summary of findings.

### Issue-Triage Agent
- Triggered automatically whenever a new issue is opened.
- Classifies the issue by matching keywords in the title and body against
  built-in rules (bug, enhancement, question, documentation, security,
  build, performance).
- Creates any missing labels in the repository.
- Applies the matched labels and posts a triage comment.

## Manual dispatch

Every agent workflow can be triggered manually from the **Actions** tab:

1. Open **Actions** in your repository.
2. Select the desired workflow (e.g. *Agent – Code Review*).
3. Click **Run workflow** and fill in the required input (PR or issue number).

## Required permissions

The `GITHUB_TOKEN` provided automatically by GitHub Actions is sufficient.
The workflows declare the minimum required permissions (`pull-requests: write`,
`issues: write`, `actions: write`).

## Extending the network

1. Add a new Python script under `agents/`.
2. Create a matching workflow file under `.github/workflows/`.
3. Add a dispatch call in `agents/orchestrator.py` if the new agent should
   be part of the daily run.
