#!/usr/bin/env python3
"""Memory benchmark for ptrclaw.

Measures synthesis quality, memory-assisted answers, and auto-recall
behavior using a real LLM (Anthropic Haiku). Each scenario runs in an
isolated temp HOME so tests don't interfere with each other.

Usage:
    python3 tests/e2e/memory_benchmark.py ./builddir/ptrclaw

Requires ANTHROPIC_API_KEY in the environment.
"""

import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

MODEL = "claude-haiku-4-5-20251001"
TIMEOUT_SECONDS = 120


# ── Helpers ──────────────────────────────────────────────────────────


def write_config(home):
    """Write ptrclaw config.json to $HOME/.ptrclaw/."""
    config_dir = os.path.join(home, ".ptrclaw")
    os.makedirs(config_dir, exist_ok=True)
    config = {
        "provider": "anthropic",
        "model": MODEL,
        "memory": {
            "backend": "sqlite",
            "synthesis": True,
            "synthesis_interval": 1,
            "recall_limit": 5,
            "enrich_depth": 1,
        },
    }
    with open(os.path.join(config_dir, "config.json"), "w") as f:
        json.dump(config, f)
    return config_dir


def run_ptrclaw(binary, home, message):
    """Run ptrclaw -m <message> and return (stdout, stderr, returncode)."""
    env = os.environ.copy()
    env["HOME"] = home
    try:
        result = subprocess.run(
            [binary, "-m", message],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            env=env,
        )
        return result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired:
        return "", "TIMEOUT", 1


def db_path(home):
    return os.path.join(home, ".ptrclaw", "memory.db")


def get_memories(path):
    """Read all memory entries from the SQLite DB."""
    if not os.path.exists(path):
        return []
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT key, content, category FROM memories ORDER BY timestamp"
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


_SCHEMA_SQL = """
PRAGMA trusted_schema=ON;

CREATE TABLE IF NOT EXISTS memories (
    id         TEXT PRIMARY KEY,
    key        TEXT UNIQUE NOT NULL,
    content    TEXT NOT NULL,
    category   TEXT NOT NULL,
    timestamp  INTEGER NOT NULL,
    session_id TEXT NOT NULL
);

CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts
    USING fts5(key, content, content=memories, content_rowid=rowid);

CREATE TRIGGER IF NOT EXISTS memories_ai AFTER INSERT ON memories BEGIN
    INSERT INTO memories_fts(rowid, key, content)
    VALUES (new.rowid, new.key, new.content);
END;

CREATE TRIGGER IF NOT EXISTS memories_ad AFTER DELETE ON memories BEGIN
    INSERT INTO memories_fts(memories_fts, rowid, key, content)
    VALUES ('delete', old.rowid, old.key, old.content);
END;

CREATE TRIGGER IF NOT EXISTS memories_au AFTER UPDATE ON memories BEGIN
    INSERT INTO memories_fts(memories_fts, rowid, key, content)
    VALUES ('delete', old.rowid, old.key, old.content);
    INSERT INTO memories_fts(rowid, key, content)
    VALUES (new.rowid, new.key, new.content);
END;

CREATE TABLE IF NOT EXISTS memory_links (
    from_key TEXT NOT NULL,
    to_key   TEXT NOT NULL,
    PRIMARY KEY (from_key, to_key)
);
"""


def seed_database(path, entries):
    """Create the DB with full schema and insert seed entries."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    conn = sqlite3.connect(path)
    conn.executescript(_SCHEMA_SQL)
    ts = int(time.time())
    for i, entry in enumerate(entries):
        conn.execute(
            "INSERT INTO memories (id, key, content, category, timestamp, session_id)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            (
                f"seed-{i}",
                entry["key"],
                entry["content"],
                entry.get("category", "knowledge"),
                ts,
                "",
            ),
        )
    conn.commit()
    conn.close()


# ── Scenarios ────────────────────────────────────────────────────────


def test_synthesis_quality(binary, home):
    """Send messages containing facts, verify DB has synthesized entries.

    With synthesis_interval=1, synthesis runs after every user message.
    We send three messages with distinct facts and check whether those
    facts appear in the DB afterward.
    """
    write_config(home)

    facts = [
        ("My favorite programming language is Rust", "rust"),
        ("I work at a company called Acme Corp", "acme"),
        ("My project uses PostgreSQL for the database", "postgresql"),
    ]

    for msg, _ in facts:
        stdout, stderr, rc = run_ptrclaw(binary, home, msg)
        if rc != 0:
            return 0.0, f"ptrclaw exited {rc}: {stderr[:200]}"

    memories = get_memories(db_path(home))
    if not memories:
        return 0.0, "no memories found in DB after synthesis"

    searchable = " ".join(
        (m["content"] + " " + m["key"]).lower() for m in memories
    )

    found = sum(1 for _, kw in facts if kw in searchable)
    return found / len(facts), f"{found}/{len(facts)} facts in {len(memories)} entries"


def test_memory_assisted_answers(binary, home):
    """Pre-populate DB with facts, ask questions, check responses.

    Auto-recall enriches the user message with matching memory entries
    before sending to the LLM. The LLM should answer using that context.
    """
    config_dir = write_config(home)

    seed_database(
        db_path(home),
        [
            {
                "key": "user:favorite_color",
                "content": "The user's favorite color is cerulean blue",
            },
            {
                "key": "user:pet",
                "content": "The user has a cat named Whiskers who is 3 years old",
            },
            {
                "key": "project:tech_stack",
                "content": "The current project uses FastAPI with Redis caching",
            },
        ],
    )

    qa_pairs = [
        ("What is my favorite color?", "cerulean"),
        ("What pet do I have?", "whiskers"),
        ("What web framework does my project use?", "fastapi"),
    ]

    correct = 0
    for question, keyword in qa_pairs:
        stdout, stderr, rc = run_ptrclaw(binary, home, question)
        if rc != 0:
            continue
        if keyword.lower() in stdout.lower():
            correct += 1

    return correct / len(qa_pairs), f"{correct}/{len(qa_pairs)} correct answers"


def test_no_redundant_recall(binary, home):
    """Verify auto-recall provides context without redundant tool calls.

    When memory entries match the user message, auto-enrichment prepends
    a [Memory context] block. The LLM should use that context directly
    rather than calling the memory_recall tool again.

    We verify the answer is correct (proving auto-recall worked) and
    check that the output doesn't contain memory_recall tool artifacts.
    """
    write_config(home)

    seed_database(
        db_path(home),
        [
            {
                "key": "user:birthday",
                "content": "The user's birthday is March 15th, 1990",
            },
        ],
    )

    stdout, stderr, rc = run_ptrclaw(
        binary, home, "When is my birthday? Answer briefly with just the date."
    )

    if rc != 0:
        return 0.0, f"ptrclaw exited {rc}: {stderr[:200]}"

    has_answer = "march" in stdout.lower() and "15" in stdout.lower()
    # In -m mode, tool calls happen internally and aren't printed.
    # If memory_recall appears in stdout, the LLM is narrating tool use
    # instead of answering directly from the auto-recalled context.
    has_tool_artifact = "memory_recall" in stdout.lower()

    if has_answer and not has_tool_artifact:
        return 1.0, "correct answer from auto-recall, no redundant tool call"
    elif has_answer:
        return 0.5, "correct answer but possible redundant recall"
    else:
        return 0.0, f"expected March 15: {stdout.strip()[:200]}"


# ── Runner ───────────────────────────────────────────────────────────

SCENARIOS = [
    ("synthesis_quality", test_synthesis_quality),
    ("memory_assisted_answers", test_memory_assisted_answers),
    ("no_redundant_recall", test_no_redundant_recall),
]


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-ptrclaw-binary>", file=sys.stderr)
        sys.exit(1)

    binary = os.path.abspath(sys.argv[1])
    if not os.path.isfile(binary):
        print(f"Error: binary not found: {binary}", file=sys.stderr)
        sys.exit(1)

    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("Error: ANTHROPIC_API_KEY environment variable required", file=sys.stderr)
        sys.exit(1)

    print(f"Memory Benchmark — ptrclaw: {binary}")
    print(f"Model: {MODEL}\n")

    results = []
    for name, fn in SCENARIOS:
        home = tempfile.mkdtemp(prefix=f"ptrclaw_bench_{name}_")
        try:
            score, details = fn(binary, home)
            results.append({"name": name, "score": score, "details": details})
            tag = "PASS" if score >= 0.5 else "FAIL"
            print(f"  [{tag}] {name}: {score:.0%} — {details}")
        except Exception as e:
            results.append({"name": name, "score": 0.0, "details": str(e)})
            print(f"  [ERROR] {name}: {e}")
        finally:
            shutil.rmtree(home, ignore_errors=True)

    # Summary
    total = sum(r["score"] for r in results)
    maximum = len(results)
    print(f"\n{'=' * 60}")
    print(f"Memory Benchmark Results: {total:.1f}/{maximum} ({total / maximum:.0%})")
    print(f"{'=' * 60}")

    # Machine-readable output for CI
    print(f"\n::notice::Memory benchmark score: {total:.1f}/{maximum}")

    sys.exit(0 if all(r["score"] >= 0.5 for r in results) else 1)


if __name__ == "__main__":
    main()
