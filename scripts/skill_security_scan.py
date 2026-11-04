#!/usr/bin/env python3
"""
skill_security_scan.py -- AI Agent Skill Security Scanner

Scans agent_skills/*.md for dangerous tool usage and unsafe behavior
patterns. Tailored for the ai_agent project on Vela/NuttX.

Usage:
    python3 scripts/skill_security_scan.py
    python3 scripts/skill_security_scan.py --ci
    python3 scripts/skill_security_scan.py --baseline _security_reports/report.json
    python3 scripts/skill_security_scan.py --llm-review  # LLM intent analysis

Exit codes (--ci mode):
    0 = pass (no new unsafe/critical findings)
    1 = block (new unsafe or critical findings detected)
"""

from __future__ import annotations
import argparse
import hashlib
import json
import os
import re
import sys
import urllib.request
import urllib.error
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# -- Tool risk classification --
# Only tools with actual security impact. Zero-risk tools omitted.

TOOL_RISK = {
    # Critical: arbitrary command execution
    "run_shell": 32,
    # High: persistent data modification
    "write_file": 16,
    "edit_file": 16,
    "cron_add": 12,
    "cron_remove": 12,
    "feishu_doc_create": 10,
    "feishu_doc_write": 10,
    # Medium: network egress (data exfiltration vector)
    "fetch_url": 8,
    "feishu_send_mention": 8,
    "web_search": 6,
    "feishu_chat_members": 4,
    "news_search": 4,
    # Low: read-only but sensitive
    "read_file": 4,
    "analyze_image": 4,
    "feishu_doc_read": 4,
    "list_dir": 2,
}

# -- Unsafe behavior patterns --

UNSAFE_PATTERNS = [
    (
        "prompt-injection",
        "critical",
        re.compile(
            r"(?i)(ignore\s+(all\s+|previous\s+)?instructions|"
            r"you\s+are\s+now|reveal\s+(the\s+)?(system|hidden)\s+prompt|"
            r"\u8f93\u51fa\u7cfb\u7edf\u63d0\u793a\u8bcd|\u5ffd\u7565(\u6240\u6709|\u4e4b\u524d\u7684)\u6307\u4ee4)"
        ),
        "Prompt injection or boundary bypass attempt",
    ),
    (
        "remote-exec",
        "critical",
        re.compile(r"(?i)\b(curl|wget)\b[^\n]{0,200}\|\s*(sh|bash|python)"),
        "Remote script pipeline execution",
    ),
    (
        "stealth-instruction",
        "high",
        re.compile(
            r"(?i)(do\s*n[o']t\s+tell\s+the\s+user|"
            r"silently|without\s+(user\s+)?confirmation|"
            r"\u4e0d\u8981\u544a\u8bc9\u7528\u6237|\u9759\u9ed8|\u7ed5\u8fc7\u786e\u8ba4|\u9690\u85cf)"
        ),
        "Instructs agent to hide actions from user",
    ),
    (
        "data-exfiltration",
        "high",
        re.compile(
            r"(?i)(read_file[^\n]{0,60}(fetch_url|web_search|feishu_send)|"
            r"(\u5fc3\u7387|\u6b65\u6570|\u4f4d\u7f6e|\u901a\u8baf\u5f55)[^\n]{0,60}(\u53d1\u9001|\u4e0a\u4f20|\u8f6c\u53d1))"
        ),
        "Read sensitive data then send externally",
    ),
    (
        "credential-extract",
        "high",
        re.compile(
            r"(?i)(read_file[^\n]{0,40}config\.json|"
            r"cat[^\n]{0,40}\.env|"
            r"(\u8bfb\u53d6|\u83b7\u53d6|\u63d0\u53d6)[^\n]{0,20}(\u5bc6\u94a5|token|secret))"
        ),
        "Attempts to extract credentials from files",
    ),
    (
        "persistence",
        "medium",
        re.compile(
            r"(?i)(cron_add[^\n]{0,80}(every|\u6bcf)\s*\d+\s*(min|sec|\u5206|\u79d2)|"
            r"\u5b9a\u65f6[^\n]{0,40}(\u4e0a\u4f20|\u53d1\u9001|\u76d1\u63a7))"
        ),
        "High-frequency cron or persistent monitoring",
    ),
]


@dataclass
class Finding:
    category: str
    severity: str
    path: str
    line: int
    message: str
    excerpt: str
    confidence: str = "high"  # high / medium / low

    def key(self) -> str:
        """Stable fingerprint -- hash-based, immune to line number drift."""
        raw = f"{self.path}:{self.category}:{self.excerpt[:50]}"
        return hashlib.sha1(raw.encode()).hexdigest()[:12]

    def to_dict(self) -> dict:
        return {
            "category": self.category,
            "severity": self.severity,
            "confidence": self.confidence,
            "path": self.path,
            "line": self.line,
            "message": self.message,
            "excerpt": self.excerpt[:200],
            "fingerprint": self.key(),
        }


@dataclass
class SkillReport:
    path: str
    name: str
    tools_used: list[str] = field(default_factory=list)
    risk_score: int = 0
    findings: list[Finding] = field(default_factory=list)
    disposition: str = "pass"

    def to_dict(self) -> dict:
        return {
            "path": self.path,
            "name": self.name,
            "tools_used": self.tools_used,
            "risk_score": self.risk_score,
            "finding_count": len(self.findings),
            "findings": [f.to_dict() for f in self.findings],
            "disposition": self.disposition,
        }


def scan_skill(path: Path) -> SkillReport:
    content = path.read_text(encoding="utf-8", errors="replace")
    lines = content.splitlines()

    # Extract title
    name = path.stem
    for line in lines:
        if line.strip().startswith("#"):
            name = line.lstrip("#").strip()
            break

    report = SkillReport(path=str(path), name=name)

    # Detect tool references
    for tool_name, weight in TOOL_RISK.items():
        if re.search(rf"\b{re.escape(tool_name)}\b", content):
            report.tools_used.append(tool_name)
            report.risk_score += weight

    # Build set of line indices inside fenced code blocks (``` ... ```)
    in_code_block = False
    code_block_lines: set[int] = set()
    for idx, line in enumerate(lines):
        if line.strip().startswith("```"):
            in_code_block = not in_code_block
            code_block_lines.add(idx)
            continue
        if in_code_block:
            code_block_lines.add(idx)

    # Scan for unsafe patterns (skip code blocks, lower confidence)
    for category, severity, pattern, message in UNSAFE_PATTERNS:
        for idx, line in enumerate(lines):
            if pattern.search(line):
                # Findings inside code blocks get low confidence
                confidence = "low" if idx in code_block_lines else "high"
                report.findings.append(Finding(
                    category=category,
                    severity=severity,
                    path=str(path),
                    line=idx + 1,
                    message=message,
                    excerpt=line.strip(),
                    confidence=confidence,
                ))

    # Disposition: pass / review / block
    # Only high-confidence findings affect disposition
    high_conf = [f for f in report.findings if f.confidence == "high"]
    severities = [f.severity for f in high_conf]
    if "critical" in severities:
        report.disposition = "block"
    elif "high" in severities or report.risk_score >= 48:
        report.disposition = "review"
    elif report.risk_score >= 24:
        report.disposition = "review"
    else:
        report.disposition = "pass"

    return report


def load_baseline(path: Path) -> set[str]:
    """Load finding fingerprints from a previous report for diff."""
    if not path.exists():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    keys = set()
    for skill in data.get("skills", []):
        for f in skill.get("findings", []):
            fp = f.get("fingerprint")
            if fp:
                keys.add(fp)
            else:
                # Fallback for old reports without fingerprint
                keys.add(f"{f['path']}:{f['category']}:{f['line']}")
    return keys


# -- LLM Intent Review --

LLM_REVIEW_PROMPT = """You are a security reviewer for an embedded AI agent on a smartwatch (Vela/NuttX).

Analyze this skill file and determine if the tools it references are consistent with its stated purpose.

Skill title: {title}
Skill content (first 500 chars):
{content_preview}

Tools referenced: {tools}
Risk score: {risk_score}

Static findings:
{findings_text}

Answer in this exact JSON format:
{{
  "verdict": "safe" | "suspicious" | "dangerous",
  "reason": "one sentence explanation",
  "false_positives": ["list of finding categories that are false positives given the skill's intent"]
}}

Rules:
- A "system health" or "config" skill using run_shell/read_file is SAFE
- A "weather" or "reminder" skill using run_shell is SUSPICIOUS
- Any skill that reads credentials then sends them externally is DANGEROUS
- If tool usage matches the skill's declared purpose, it's SAFE
"""


def llm_review_skill(report: SkillReport, content: str,
                     api_key: str, api_base: str) -> dict | None:
    """Call LLM to assess if skill's tool usage matches its intent."""
    if not report.tools_used and not report.findings:
        return {"verdict": "safe", "reason": "No risky tools or findings",
                "false_positives": []}

    findings_text = "\n".join(
        f"  [{f.severity}] {f.category}: {f.message}"
        for f in report.findings
    ) or "  (none)"

    prompt = LLM_REVIEW_PROMPT.format(
        title=report.name,
        content_preview=content[:500],
        tools=", ".join(report.tools_used) or "(none)",
        risk_score=report.risk_score,
        findings_text=findings_text,
    )

    body = json.dumps({
        "model": "qwen-turbo",
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.1,
    }).encode("utf-8")

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }

    url = f"{api_base}/chat/completions"
    req = urllib.request.Request(url, data=body, headers=headers)

    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            text = data["choices"][0]["message"]["content"]
            # Extract JSON from response (may have markdown fences)
            match = re.search(r"\{[^}]+\}", text, re.DOTALL)
            if match:
                return json.loads(match.group())
    except (urllib.error.URLError, json.JSONDecodeError, KeyError) as e:
        print(f"  LLM review failed for {report.name}: {e}", file=sys.stderr)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="AI Agent Skill Security Scanner")
    parser.add_argument("--repo-root", default=".",
                        help="Repository root")
    parser.add_argument("--skills-dir", default="agent_skills",
                        help="Skills directory (relative)")
    parser.add_argument("--ci", action="store_true",
                        help="Exit 1 if new unsafe/critical findings")
    parser.add_argument("--baseline", default=None,
                        help="Previous report.json for diff mode")
    parser.add_argument("--llm-review", action="store_true",
                        help="Use LLM to assess intent consistency")
    parser.add_argument("--api-key", default=None,
                        help="LLM API key (or set DASHSCOPE_API_KEY env)")
    parser.add_argument("--api-base",
                        default="https://dashscope.aliyuncs.com/compatible-mode/v1",
                        help="LLM API base URL")
    parser.add_argument("--output", default="_security_reports",
                        help="Output directory")
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    skills_dir = repo / args.skills_dir
    output_dir = repo / args.output

    # Resolve API key for LLM review
    api_key = args.api_key or os.environ.get("DASHSCOPE_API_KEY", "")
    if args.llm_review and not api_key:
        print("Error: --llm-review requires --api-key or DASHSCOPE_API_KEY env",
              file=sys.stderr)
        return 1

    # Scan skills
    reports: list[SkillReport] = []
    skill_contents: dict[str, str] = {}
    if skills_dir.exists():
        for path in sorted(skills_dir.glob("*.md")):
            if path.name.lower() == "readme.md":
                continue
            content = path.read_text(encoding="utf-8", errors="replace")
            skill_contents[str(path)] = content
            reports.append(scan_skill(path))

    # LLM intent review (only for skills with findings or high risk)
    llm_results: dict[str, dict] = {}
    if args.llm_review and api_key:
        candidates = [r for r in reports
                      if r.findings or r.risk_score >= 24]
        print(f"LLM reviewing {len(candidates)} skills...")
        for report in candidates:
            content = skill_contents.get(report.path, "")
            result = llm_review_skill(report, content, api_key, args.api_base)
            if result:
                llm_results[report.path] = result
                verdict = result.get("verdict", "")
                reason = result.get("reason", "")
                false_pos = result.get("false_positives", [])
                print(f"  {report.name}: {verdict} -- {reason}")

                # Adjust disposition based on LLM verdict
                # Rule: LLM can downgrade "review" to "pass",
                # but CANNOT override "block" (critical findings
                # are hard gates, immune to LLM manipulation)
                if verdict == "safe" and report.disposition != "block":
                    report.disposition = "pass"
                    # Remove false positive findings (non-critical only)
                    report.findings = [
                        f for f in report.findings
                        if f.category not in false_pos
                        or f.severity == "critical"
                    ]
                elif verdict == "dangerous":
                    report.disposition = "block"

    # Sort by risk (highest first)
    reports.sort(key=lambda r: r.risk_score, reverse=True)

    # Baseline diff
    baseline_keys: set[str] = set()
    if args.baseline:
        baseline_keys = load_baseline(Path(args.baseline))

    all_findings = [f for r in reports for f in r.findings]
    new_findings = [f for f in all_findings if f.key() not in baseline_keys]

    # Summary
    summary = {
        "pass": sum(1 for r in reports if r.disposition == "pass"),
        "review": sum(1 for r in reports if r.disposition == "review"),
        "block": sum(1 for r in reports if r.disposition == "block"),
    }

    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "scanner": "ai-agent-skill-scanner",
        "repo": str(repo),
        "skill_count": len(reports),
        "summary": summary,
        "total_findings": len(all_findings),
        "new_findings": len(new_findings),
        "llm_reviewed": len(llm_results),
        "skills": [r.to_dict() for r in reports],
        "llm_results": llm_results,
    }

    # Write report
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path = output_dir / "report.json"
    json_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8")

    # Print summary
    print(f"\nSkills: {len(reports)}  "
          f"pass={summary['pass']} review={summary['review']} "
          f"block={summary['block']}")
    print(f"Findings: {len(all_findings)} total, "
          f"{len(new_findings)} new")
    if args.llm_review:
        print(f"LLM reviewed: {len(llm_results)} skills")
    if new_findings:
        print("\nNew findings:")
        for f in new_findings[:10]:
            print(f"  [{f.severity}] {f.path}:{f.line} -- {f.message}")
    print(f"\nReport: {json_path}")

    # CI gate: block only on NEW unsafe/critical findings
    if args.ci and new_findings:
        has_block = any(f.severity in ("critical", "high")
                        for f in new_findings)
        if has_block:
            print("\n[X] BLOCKED: new high/critical findings")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
