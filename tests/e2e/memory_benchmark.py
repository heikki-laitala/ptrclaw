#!/usr/bin/env python3
"""Memory recall benchmark for ptrclaw.

Drives a multi-turn conversation through ptrclaw's pipe channel, lets
memories accumulate naturally via synthesis, restarts the agent, runs
test questions, and scores responses with an LLM judge.

Usage:
    python3 tests/e2e/memory_benchmark.py ./builddir/ptrclaw
    python3 tests/e2e/memory_benchmark.py ./builddir/ptrclaw --backend json

Requires ANTHROPIC_API_KEY in the environment.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

MODEL = "claude-haiku-4-5-20251001"
TIMEOUT_SECONDS = 180

# ── Conversation scripts ─────────────────────────────────────────────

SEED_MESSAGES = [
    "We decided to build the new analytics dashboard using Vue.js for the "
    "frontend and Go for the backend API. The database will be PostgreSQL "
    "with TimescaleDB extension for time-series data.",

    "The team consists of Sarah who leads frontend, Marcus handles the "
    "backend services, and Priya is responsible for the data pipeline. "
    "The deadline for the MVP is March 15th.",

    "We identified a major risk: the existing authentication service uses "
    "OAuth 1.0 which needs to be migrated to OAuth 2.0 before the dashboard "
    "can integrate. Marcus estimates this will take about two weeks.",

    "For deployment we're using Kubernetes on AWS EKS. The staging "
    "environment is already set up but production needs a new cluster. "
    "Sarah mentioned we should also add Grafana for monitoring the "
    "dashboard's own performance metrics.",
]

TEST_CASES = [
    {
        "question": "What technology stack was chosen for the analytics dashboard?",
        "ground_truth": "Vue.js frontend, Go backend, PostgreSQL with TimescaleDB",
    },
    {
        "question": "Who is responsible for the data pipeline work?",
        "ground_truth": "Priya is responsible for the data pipeline",
    },
    {
        "question": "What's the main blocker for the dashboard integration?",
        "ground_truth": (
            "OAuth 1.0 to OAuth 2.0 migration, Marcus is handling it, "
            "estimated two weeks"
        ),
    },
    {
        "question": "Describe the deployment infrastructure for the project.",
        "ground_truth": (
            "Kubernetes on AWS EKS, staging environment already set up, "
            "production needs a new cluster, Grafana for monitoring"
        ),
    },
    {
        "question": "What's the project timeline and what risks should we track?",
        "ground_truth": (
            "MVP deadline is March 15th, main risk is OAuth 1.0 to 2.0 "
            "migration taking about two weeks"
        ),
    },
]


# ── Pipe channel helpers ─────────────────────────────────────────────


def make_config(home, backend="sqlite"):
    """Write ptrclaw config.json to $HOME/.ptrclaw/."""
    config_dir = os.path.join(home, ".ptrclaw")
    os.makedirs(config_dir, exist_ok=True)
    config = {
        "provider": "anthropic",
        "model": MODEL,
        "memory": {
            "backend": backend,
            "synthesis": True,
            "synthesis_interval": 1,
            "recall_limit": 5,
            "enrich_depth": 1,
            "auto_save": True,
        },
    }
    with open(os.path.join(config_dir, "config.json"), "w") as f:
        json.dump(config, f)


def start_pipe(binary, home):
    """Start ptrclaw in pipe mode, return the Popen handle."""
    env = os.environ.copy()
    env["HOME"] = home
    return subprocess.Popen(
        [binary, "--channel", "pipe"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )


def send_message(proc, content):
    """Send a JSONL message and read the response."""
    line = json.dumps({"content": content}) + "\n"
    proc.stdin.write(line)
    proc.stdin.flush()
    resp_line = proc.stdout.readline()
    if not resp_line:
        stderr = proc.stderr.read()
        raise RuntimeError(f"pipe: no response (stderr: {stderr[:300]})")
    return json.loads(resp_line).get("content", "")


def close_pipe(proc):
    """Close stdin to signal EOF, wait for clean exit."""
    try:
        proc.stdin.close()
    except BrokenPipeError:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ── LLM judge ────────────────────────────────────────────────────────


def llm_judge(response, ground_truth):
    """Score a response against ground truth using Anthropic Haiku.

    Returns 0.0, 0.5, or 1.0.
    """
    api_key = os.environ["ANTHROPIC_API_KEY"]
    body = json.dumps({
        "model": MODEL,
        "max_tokens": 16,
        "system": (
            "You are a scoring assistant. Rate how well a response "
            "demonstrates awareness of specific facts from a prior "
            "conversation.\n\n"
            "Score exactly one of:\n"
            "- 1.0: Response clearly references the key facts correctly\n"
            "- 0.5: Response references some facts or is partially correct\n"
            "- 0.0: Response shows no awareness of the relevant facts\n\n"
            "Output only the numeric score."
        ),
        "messages": [
            {
                "role": "user",
                "content": (
                    f"Ground truth facts: {ground_truth}\n\n"
                    f"Response to evaluate:\n{response}"
                ),
            }
        ],
    }).encode()

    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
        },
    )

    with urllib.request.urlopen(req, timeout=30) as resp:
        result = json.loads(resp.read())

    text = result["content"][0]["text"].strip()
    # Parse the numeric score
    for candidate in ("1.0", "0.5", "0.0"):
        if candidate in text:
            return float(candidate)
    # Fallback: try to parse as float and round to nearest valid score
    try:
        raw = float(text)
        if raw >= 0.75:
            return 1.0
        if raw >= 0.25:
            return 0.5
        return 0.0
    except ValueError:
        return 0.0


# ── Benchmark runner ─────────────────────────────────────────────────


def run_benchmark(binary, backend):
    """Run the full seed → test → score pipeline. Returns (scores, details)."""
    home = tempfile.mkdtemp(prefix=f"ptrclaw_bench_{backend}_")
    try:
        make_config(home, backend)

        # Phase 1: Seed — send 4 messages to build up memories
        print(f"\n  Phase 1: Seeding ({len(SEED_MESSAGES)} messages)...")
        proc = start_pipe(binary, home)
        for i, msg in enumerate(SEED_MESSAGES):
            resp = send_message(proc, msg)
            print(f"    Seed {i + 1}: sent ({len(resp)} chars response)")
        close_pipe(proc)
        print("    Seed phase complete, memories accumulated.")

        # Phase 2: Test — new process, same HOME (same memory files)
        print(f"\n  Phase 2: Testing ({len(TEST_CASES)} questions)...")
        proc = start_pipe(binary, home)
        responses = []
        for i, tc in enumerate(TEST_CASES):
            resp = send_message(proc, tc["question"])
            responses.append(resp)
            print(f"    Q{i + 1}: {resp[:120]}...")
        close_pipe(proc)

        # Phase 3: Score — LLM judge
        print("\n  Phase 3: Scoring with LLM judge...")
        scores = []
        details = []
        for i, (tc, resp) in enumerate(zip(TEST_CASES, responses)):
            score = llm_judge(resp, tc["ground_truth"])
            scores.append(score)
            tag = "PASS" if score >= 0.5 else "FAIL"
            detail = f"Q{i + 1}: {score:.1f} [{tag}]"
            details.append(detail)
            print(f"    {detail}")

        return scores, details
    finally:
        shutil.rmtree(home, ignore_errors=True)


def main():
    binary = None
    backend = "sqlite"

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--backend" and i + 1 < len(args):
            backend = args[i + 1]
            i += 2
        elif args[i].startswith("--"):
            print(f"Unknown option: {args[i]}", file=sys.stderr)
            sys.exit(1)
        else:
            binary = os.path.abspath(args[i])
            i += 1

    if not binary:
        print(
            f"Usage: {sys.argv[0]} <path-to-ptrclaw-binary> [--backend sqlite|json]",
            file=sys.stderr,
        )
        sys.exit(1)

    if not os.path.isfile(binary):
        print(f"Error: binary not found: {binary}", file=sys.stderr)
        sys.exit(1)

    if not os.environ.get("ANTHROPIC_API_KEY"):
        print(
            "Error: ANTHROPIC_API_KEY environment variable required",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Memory Recall Benchmark")
    print(f"  Binary:  {binary}")
    print(f"  Backend: {backend}")
    print(f"  Model:   {MODEL}")

    scores, details = run_benchmark(binary, backend)

    mean_score = sum(scores) / len(scores)
    passed = mean_score >= 0.5

    print(f"\n{'=' * 60}")
    print(f"  Backend: {backend}")
    print(f"  Scores:  {', '.join(f'{s:.1f}' for s in scores)}")
    print(f"  Mean:    {mean_score:.2f}")
    print(f"  Result:  {'PASS' if passed else 'FAIL'} (threshold: 0.50)")
    print(f"{'=' * 60}")

    # Machine-readable output for CI
    print(f"\n::notice::Memory benchmark ({backend}): mean={mean_score:.2f}")

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
