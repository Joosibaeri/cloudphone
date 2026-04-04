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
└───────┬──────────────────────┬───────────────┬───────┘
        │                      │               │
        ▼                      ▼               ▼
┌───────────────┐  ┌───────────────────┐  ┌───────────────────┐
│  Code-Review  │  │  Issue-Triage     │  │  KDE Plasma / Qt  │
│  Agent        │  │  Agent            │  │  Checker Agent    │
│               │  │                   │  │                   │
│ code_         │  │ issue_            │  │ plasma_           │
│ reviewer.py   │  │ triager.py        │  │ checker.py        │
│               │  │                   │  │                   │
│ • PR open/    │  │ • issues.opened   │  │ • PR open/sync    │
│   sync        │  │   or dispatch     │  │   (Qt/QML files)  │
│ • TODOs,      │  │ • Classifies by   │  │ • Qt antipatterns │
│   FIXMEs,     │  │   keywords        │  │ • KDE Plasma QML  │
│   unsafe C,   │  │ • Creates labels  │  │   API usage       │
│   credentials │  │ • Posts comment   │  │ • Signal/slot     │
│ • Posts review│  │                   │  │   style checks    │
└───────────────┘  └───────────────────┘  └───────────────────┘
```

## Agents

| Agent | Script | Workflow |
|---|---|---|
| Orchestrator | `agents/orchestrator.py` | `.github/workflows/agent-orchestrator.yml` |
| Code Review | `agents/code_reviewer.py` | `.github/workflows/agent-code-review.yml` |
| Issue Triage | `agents/issue_triager.py` | `.github/workflows/agent-issue-triage.yml` |
| KDE Plasma Checker | `agents/plasma_checker.py` | `.github/workflows/agent-plasma-checker.yml` |

## How it works

### Orchestrator
- Runs every day at 06:00 UTC via a cron schedule, or on demand via
  **Actions → Agent – Orchestrator → Run workflow**.
- Fetches all open pull requests and issues from the GitHub API.
- Dispatches `agent-code-review.yml` and `agent-plasma-checker.yml` for
  each open PR, and `agent-issue-triage.yml` for each open issue.
- The `task` input accepts: `code-review` | `issue-triage` | `plasma` | `all`

### Code-Review Agent
- Triggered automatically whenever a PR is opened, updated, or reopened.
- Downloads the PR diff and scans each **added** line for:
  - `TODO` / `FIXME` / `HACK` markers
  - Hard-coded credentials (`password`, `api_key`, `secret`, …)
  - Unsafe C calls (`gets`, `strcpy`, bare `printf(var)`)
- Posts a structured review comment on the PR with a summary of findings.

### Issue-Triage Agent
- Triggered automatically whenever a new issue is opened.
- Classifies the issue by matching keywords in the title and body against
  built-in rules (bug, enhancement, question, documentation, security,
  build, performance).
- Creates any missing labels in the repository.
- Applies the matched labels and posts a triage comment.

### KDE Plasma Checker Agent
- Triggered automatically on PRs that touch `.cpp`, `.h`, `.qml`, or `.js`
  files, or on demand via the orchestrator.
- Scans each added line in Qt/QML source files for KDE Plasma / Qt
  antipatterns, including:
  - `new QObject` without a parent (memory-leak risk)
  - Manual `delete` of Qt objects (prefer parent-child ownership)
  - Removed Qt APIs (`QString::fromAscii`)
  - Old-style `SIGNAL()`/`SLOT()` macro connections
  - `QApplication::processEvents()` re-entrancy hazard
  - Deprecated Plasma QML components (`PlasmaComponents 2.0`)
  - Inline `setStyleSheet()` vs. proper KDE theming
  - Missing `QVariant::canConvert` checks
- Posts a structured review comment with links to the relevant Qt/KDE docs.

## Manual dispatch

Every agent workflow can be triggered manually from the **Actions** tab:

1. Open **Actions** in your repository.
2. Select the desired workflow (e.g. *Agent – KDE Plasma Checker*).
3. Click **Run workflow** and fill in the required input (PR number).

## Required permissions

The `GITHUB_TOKEN` provided automatically by GitHub Actions is sufficient.
The workflows declare the minimum required permissions (`pull-requests: write`,
`issues: write`, `actions: write`).

## Extending the network

1. Add a new Python script under `agents/`.
2. Create a matching workflow file under `.github/workflows/`.
3. Add a dispatch call in `agents/orchestrator.py` if the new agent should
   be part of the daily run.
