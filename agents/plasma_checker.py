#!/usr/bin/env python3
"""
KDE Plasma / Qt Code-Checker Agent
------------------------------------
Fetches the diff of a pull request, scans added lines for common
KDE Plasma / Qt antipatterns, and posts a structured review comment.

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
# GitHub API helpers (shared pattern with code_reviewer.py)
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
        print(f"[plasma-checker] HTTP {exc.code} – {body_text}", file=sys.stderr)
        raise


def get_pr_diff(repo: str, pr_number: int) -> str:
    return _api_request(
        "GET",
        f"/repos/{repo}/pulls/{pr_number}",
        accept="application/vnd.github.diff",
    )


def post_pr_review(repo: str, pr_number: int, body: str,
                   event: str = "COMMENT") -> None:
    _api_request(
        "POST",
        f"/repos/{repo}/pulls/{pr_number}/reviews",
        body={"body": body, "event": event},
    )


# ---------------------------------------------------------------------------
# KDE Plasma / Qt analysis rules
# ---------------------------------------------------------------------------

# Each entry: (compiled pattern, human-readable message, docs URL)
_RULES: list[tuple[re.Pattern, str, str]] = [
    (
        re.compile(r"\bnew\s+Q[A-Z]\w+\s*\(\s*\)"),
        "🟡 `new QObject()` with no parent argument – consider passing a parent "
        "to let Qt manage object lifetime automatically.",
        "https://doc.qt.io/qt-6/objecttrees.html",
    ),
    (
        re.compile(r"\bdelete\s+(?:this->)?[\w.>-]+\s*;"),
        "🟡 Manual `delete` in a Qt source file – prefer parent-child ownership or "
        "`QScopedPointer` / `std::unique_ptr` to avoid memory management bugs.",
        "https://doc.qt.io/qt-6/qscopedpointer.html",
    ),
    (
        re.compile(r"\bQString::fromAscii\b"),
        "🔴 `QString::fromAscii()` is removed in Qt 5+ – use "
        "`QString::fromLatin1()` or `QStringLiteral`.",
        "https://doc.qt.io/qt-6/qstring.html",
    ),
    (
        re.compile(r"\bqDebug\b.*<<.*\bpassword\b", re.I),
        "🔒 Possible credential logged via `qDebug` – never log secrets.",
        "",
    ),
    (
        re.compile(r"#include\s*<QtGui/"),
        "🟠 Direct `QtGui/` include – prefer the module-level include "
        "(e.g. `#include <QWidget>`) for forward-compatibility.",
        "https://doc.qt.io/qt-6/modules-cpp.html",
    ),
    (
        re.compile(r"\bQMetaObject::invokeMethod\b.*\bQt::DirectConnection\b"),
        "🟠 `Qt::DirectConnection` inside `invokeMethod` can cause re-entrancy "
        "issues across threads – verify thread safety.",
        "https://doc.qt.io/qt-6/qmetaobject.html#invokeMethod",
    ),
    (
        re.compile(r"\bconnect\s*\(.*SIGNAL\s*\("),
        "🟡 Old-style `SIGNAL()`/`SLOT()` macro connect – use the "
        "type-safe pointer-to-member syntax introduced in Qt 5.",
        "https://doc.qt.io/qt-6/signalsandslots.html",
    ),
    (
        re.compile(r"\bQVariant::value<"),
        "🟡 `QVariant::value<T>()` without validity check – call "
        "`QVariant::canConvert<T>()` first to avoid runtime errors.",
        "https://doc.qt.io/qt-6/qvariant.html#value",
    ),
    (
        re.compile(r"\bsetStyleSheet\b"),
        "🟡 Inline `setStyleSheet()` – prefer a dedicated `.qss` file or "
        "a KDE Plasma `ColorScheme` for theming consistency.",
        "https://api.kde.org/frameworks/plasma-framework/html/",
    ),
    (
        re.compile(r"\bQApplication::processEvents\b"),
        "🔴 `QApplication::processEvents()` can cause re-entrancy bugs and "
        "makes code hard to reason about – redesign to use signals/slots or "
        "`QTimer::singleShot` instead.",
        "https://doc.qt.io/qt-6/qcoreapplication.html#processEvents",
    ),
    (
        re.compile(r"\bPlasmaComponents\b.*\bLoader\b"),
        "🟡 `PlasmaComponents.Loader` is deprecated – use "
        "`PlasmaComponents3.Loader` (Qt 5) or `PlasmaComponents` from "
        "`org.kde.plasma.components` (Qt 6).",
        "https://api.kde.org/frameworks/plasma-framework/html/",
    ),
    (
        re.compile(r"\bimport\s+org\.kde\.plasma\.core\s+2\.0\b"),
        "🟠 `org.kde.plasma.core 2.0` is the Qt 5 API – migrate to "
        "`org.kde.plasma.core` without a version for Plasma 6 / Qt 6.",
        "https://develop.kde.org/docs/plasma/widget/",
    ),
]

# File extensions considered relevant for Qt/Plasma analysis
_QT_EXTENSIONS = {
    ".cpp", ".cxx", ".cc", ".c++",
    ".h", ".hpp", ".hxx",
    ".qml", ".js",
}


def _is_qt_file(filename: str) -> bool:
    _, ext = os.path.splitext(filename.lower())
    return ext in _QT_EXTENSIONS


# ---------------------------------------------------------------------------
# Diff analysis
# ---------------------------------------------------------------------------

def analyse_diff(diff: str) -> list[tuple[str, str, str]]:
    """Return list of (file, code_snippet, message+url) tuples."""
    findings: list[tuple[str, str, str]] = []
    current_file = ""
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            current_file = line[6:]
        if not line.startswith("+") or line.startswith("+++"):
            continue
        if not _is_qt_file(current_file):
            continue
        code = line[1:]
        for pattern, message, url in _RULES:
            if pattern.search(code):
                detail = f"{message}  \n  📖 {url}" if url else message
                findings.append((current_file, code.strip(), detail))
    return findings


def summarise_diff(diff: str) -> dict[str, int]:
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
        sys.exit("[plasma-checker] GITHUB_REPO and PR_NUMBER must be set.")

    pr_number = int(pr_number_raw)
    print(f"[plasma-checker] Checking PR #{pr_number} in {repo}")

    diff: str = get_pr_diff(repo, pr_number)
    findings = analyse_diff(diff)
    file_summary = summarise_diff(diff)

    qt_files = [f for f in file_summary if _is_qt_file(f)]

    lines: list[str] = [
        "## 🔵 KDE Plasma / Qt Code Check",
        "",
    ]

    if qt_files:
        lines += ["### Qt/QML files changed", ""]
        for f in qt_files:
            lines.append(f"- `{f}` (+{file_summary[f]} lines)")
        lines.append("")
    else:
        lines += [
            "ℹ️ No Qt/QML source files detected in this PR – skipping "
            "Plasma-specific checks.",
            "",
        ]

    lines += ["### Plasma / Qt findings", ""]
    if findings:
        for fname, snippet, detail in findings:
            lines.append(f"**`{fname}`**")
            lines.append(f"> `{snippet}`")
            lines.append(f"  {detail}")
            lines.append("")
    else:
        lines.append("✅ No KDE Plasma / Qt antipatterns detected.")

    lines += [
        "",
        "---",
        f"_Checked by the **CloudPhone KDE Plasma Agent** – PR #{pr_number}_",
    ]

    review_body = "\n".join(lines)
    post_pr_review(repo, pr_number, review_body)
    print(f"[plasma-checker] Review posted for PR #{pr_number}.")
    if findings:
        print(f"[plasma-checker] {len(findings)} Plasma/Qt issue(s) found.")


if __name__ == "__main__":
    run()
