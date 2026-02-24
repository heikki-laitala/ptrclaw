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

MODEL = "claude-sonnet-4-6-20250514"
JUDGE_MODEL = "claude-haiku-4-5-20251001"
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

    "In today's standup, Priya said the data pipeline will ingest events "
    "from Kafka topics. She's using Apache Flink for stream processing and "
    "the raw events land in S3 before being loaded into TimescaleDB. The "
    "expected throughput is around 50,000 events per second at peak.",

    "Sarah demoed the dashboard wireframes. The main view has four panels: "
    "a real-time event stream, a retention cohort chart, a revenue funnel, "
    "and a geographic heatmap. The design system is based on Tailwind CSS "
    "with a custom component library called Prism.",

    "Marcus raised a concern about rate limiting. The Go backend needs to "
    "handle burst traffic during marketing campaigns. He proposed using "
    "Redis with a token bucket algorithm, capped at 1000 requests per "
    "second per tenant. The Redis cluster will be hosted on ElastiCache.",

    "We had a retrospective on the auth migration. Marcus finished the "
    "OAuth 2.0 work ahead of schedule. The remaining piece is SAML "
    "integration for enterprise SSO customers — that's been assigned to "
    "a new contractor named Diego who starts next Monday.",

    "Priya shared her data quality strategy. She's implementing a dead "
    "letter queue in Kafka for malformed events. Events that fail schema "
    "validation three times get routed to a separate S3 bucket for manual "
    "review. She estimates about 2% of events will need this path.",

    "Sarah proposed a feature flag system using LaunchDarkly for the "
    "dashboard rollout. The plan is to do a gradual rollout: 10% of "
    "internal users first, then 50%, then GA. Each phase lasts one week.",

    "Marcus set up the CI/CD pipeline using GitHub Actions. The backend "
    "runs unit tests, integration tests against a Dockerized PostgreSQL, "
    "and then deploys to staging via ArgoCD. The test suite takes about "
    "8 minutes to complete.",

    "We discussed the budget. The AWS infrastructure cost estimate is "
    "$4,200 per month: $1,800 for EKS, $900 for ElastiCache, $600 for "
    "RDS PostgreSQL, $500 for S3 and data transfer, and $400 for "
    "monitoring and logging with Datadog.",

    "Sarah mentioned accessibility requirements. The dashboard must meet "
    "WCAG 2.1 AA compliance. She's adding aria labels to all chart "
    "components and ensuring keyboard navigation works for every panel. "
    "An external audit by AccessiTech is scheduled for April 1st.",

    "Diego sent his first status update. He's completed the SAML metadata "
    "parser and is now working on the SP-initiated SSO flow. He found "
    "that three enterprise customers — Globex, Initech, and Umbrella "
    "Corp — are waiting for SAML support before signing their contracts.",

    "In the latest planning session, we agreed to add a fifth dashboard "
    "panel: a custom SQL query builder that lets power users write ad-hoc "
    "queries against TimescaleDB. Sarah will build the UI and Marcus will "
    "create a sandboxed read-only database connection pool for it.",
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
            "OAuth 1.0 to OAuth 2.0 migration was the blocker, Marcus "
            "handled it and finished ahead of schedule"
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
            "MVP deadline is March 15th, OAuth migration risk is resolved, "
            "SAML integration for enterprise SSO still pending"
        ),
    },
    {
        "question": "How does the data pipeline process events?",
        "ground_truth": (
            "Events come from Kafka topics, processed by Apache Flink for "
            "stream processing, raw events stored in S3, then loaded into "
            "TimescaleDB. Peak throughput is 50,000 events per second"
        ),
    },
    {
        "question": "What are the main panels on the dashboard UI?",
        "ground_truth": (
            "Four panels: real-time event stream, retention cohort chart, "
            "revenue funnel, and geographic heatmap. Uses Tailwind CSS "
            "with a custom component library called Prism"
        ),
    },
    {
        "question": "How is rate limiting handled in the backend?",
        "ground_truth": (
            "Redis with token bucket algorithm, 1000 requests per second "
            "per tenant, Redis cluster on ElastiCache. Marcus designed this "
            "for burst traffic during marketing campaigns"
        ),
    },
    {
        "question": "Who is Diego and what is he working on?",
        "ground_truth": (
            "Diego is a new contractor starting next Monday, assigned to "
            "SAML integration for enterprise SSO customers"
        ),
    },
    {
        "question": "Summarize what each team member is responsible for.",
        "ground_truth": (
            "Sarah leads frontend and designed the dashboard wireframes, "
            "Marcus handles backend services including auth migration and "
            "rate limiting, Priya owns the data pipeline with Kafka/Flink, "
            "Diego is the new contractor handling SAML SSO integration"
        ),
    },
    {
        "question": "What happens to malformed events in the data pipeline?",
        "ground_truth": (
            "Malformed events go to a dead letter queue in Kafka. After "
            "three failed schema validations they're routed to a separate "
            "S3 bucket for manual review. About 2% of events expected"
        ),
    },
    {
        "question": "How much does the AWS infrastructure cost monthly?",
        "ground_truth": (
            "$4,200 per month total: $1,800 EKS, $900 ElastiCache, "
            "$600 RDS PostgreSQL, $500 S3 and data transfer, $400 "
            "Datadog monitoring and logging"
        ),
    },
    {
        "question": "What enterprise customers are waiting for SAML support?",
        "ground_truth": (
            "Three enterprise customers: Globex, Initech, and Umbrella "
            "Corp are waiting for SAML support before signing contracts. "
            "Diego is building the SP-initiated SSO flow"
        ),
    },
    {
        "question": "What's the rollout strategy for the dashboard?",
        "ground_truth": (
            "Gradual rollout using LaunchDarkly feature flags: 10% "
            "internal users first, then 50%, then GA. Each phase "
            "lasts one week"
        ),
    },
    {
        "question": "What's the fifth dashboard panel that was added?",
        "ground_truth": (
            "Custom SQL query builder for power users to write ad-hoc "
            "queries against TimescaleDB. Sarah builds UI, Marcus "
            "creates sandboxed read-only database connection pool"
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
        "agent": {
            "max_history_messages": 20,
        },
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

    Returns a float between 0.0 and 1.0.
    """
    api_key = os.environ["ANTHROPIC_API_KEY"]
    body = json.dumps({
        "model": JUDGE_MODEL,
        "max_tokens": 16,
        "system": (
            "You are a scoring assistant. Rate how well a response "
            "demonstrates awareness of specific facts from a prior "
            "conversation.\n\n"
            "Score on a continuous scale from 0.0 to 1.0:\n"
            "- 1.0: All key facts referenced correctly\n"
            "- 0.7-0.9: Most facts present, minor gaps\n"
            "- 0.4-0.6: Some facts present, partially correct\n"
            "- 0.1-0.3: Vague awareness, mostly missing\n"
            "- 0.0: No awareness of the relevant facts\n\n"
            "Output only the numeric score, nothing else."
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
    try:
        score = float(text)
        return max(0.0, min(1.0, score))
    except ValueError:
        return 0.0


# ── Benchmark runner ─────────────────────────────────────────────────


def run_benchmark(binary, backend):
    """Run the full seed → test → score pipeline. Returns list of result dicts."""
    home = tempfile.mkdtemp(prefix=f"ptrclaw_bench_{backend}_")
    try:
        make_config(home, backend)

        # Phase 1: Seed
        print(f"\n  Phase 1: Seeding ({len(SEED_MESSAGES)} messages)...",
              file=sys.stderr)
        proc = start_pipe(binary, home)
        for i, msg in enumerate(SEED_MESSAGES):
            resp = send_message(proc, msg)
            print(f"    Seed {i + 1}: sent ({len(resp)} chars response)",
                  file=sys.stderr)
        close_pipe(proc)
        print("    Seed phase complete, memories accumulated.",
              file=sys.stderr)

        # Phase 2: Test — new process, same HOME (same memory files)
        print(f"\n  Phase 2: Testing ({len(TEST_CASES)} questions)...",
              file=sys.stderr)
        proc = start_pipe(binary, home)
        responses = []
        for i, tc in enumerate(TEST_CASES):
            resp = send_message(proc, tc["question"])
            responses.append(resp)
            print(f"    Q{i + 1}: {resp[:120]}...", file=sys.stderr)
        close_pipe(proc)

        # Phase 3: Score — LLM judge
        print("\n  Phase 3: Scoring with LLM judge...", file=sys.stderr)
        results = []
        for i, (tc, resp) in enumerate(zip(TEST_CASES, responses)):
            score = llm_judge(resp, tc["ground_truth"])
            results.append({
                "question": tc["question"],
                "ground_truth": tc["ground_truth"],
                "response": resp,
                "score": score,
            })
            print(f"    Q{i + 1}: {score:.2f}", file=sys.stderr)

        return results
    finally:
        shutil.rmtree(home, ignore_errors=True)


def print_results(results, backend, baseline=None):
    """Print results table to stdout, with optional baseline comparison."""
    scores = [r["score"] for r in results]
    mean = sum(scores) / len(scores)
    passed = mean >= 0.5

    # Build baseline score lookup by question text
    base_scores = {}
    base_mean = None
    if baseline:
        for bq in baseline.get("questions", []):
            base_scores[bq["question"]] = bq["score"]
        base_mean = baseline.get("mean_score")

    has_baseline = bool(base_scores)
    w = 72 if has_baseline else 62

    print(f"\nMemory Recall Benchmark — {backend}")
    print(f"{'─' * w}")
    if has_baseline:
        print(f" {'#':<4} {'Score':>5}  {'Base':>5}  {'Delta':>6}  Question")
    else:
        print(f" {'#':<4} {'Score':>5}  Question")
    print(f"{'─' * w}")

    for i, r in enumerate(results):
        tag = "✓" if r["score"] >= 0.5 else "✗"
        q = r["question"][:42] if has_baseline else r["question"][:48]
        if has_baseline:
            bs = base_scores.get(r["question"])
            if bs is not None:
                delta = r["score"] - bs
                sign = "+" if delta > 0 else ""
                print(f" {i + 1:<4} {r['score']:>5.2f}  {bs:>5.2f}  "
                      f"{sign}{delta:>5.2f}  {tag} {q}")
            else:
                print(f" {i + 1:<4} {r['score']:>5.2f}     —      —  "
                      f"{tag} {q}")
        else:
            print(f" {i + 1:<4} {r['score']:>5.2f}  {tag} {q}")

    print(f"{'─' * w}")
    if has_baseline and base_mean is not None:
        delta = mean - base_mean
        sign = "+" if delta > 0 else ""
        print(f" {'Mean':>9}: {mean:.2f}  (baseline: {base_mean:.2f}, "
              f"delta: {sign}{delta:.2f})")
    else:
        print(f" {'Mean':>9}: {mean:.2f}")
    print(f" {'Result':>9}: {'PASS' if passed else 'FAIL'} (threshold: 0.50)")
    print(f" {'Model':>9}: {MODEL}")
    print(f" {'Judge':>9}: {JUDGE_MODEL}")
    print(f" {'Backend':>9}: {backend}")
    print(f" {'Seed msgs':>9}: {len(SEED_MESSAGES)}")
    print(f" {'Questions':>9}: {len(TEST_CASES)}")
    print()


def main():
    binary = None
    backend = "sqlite"
    output_file = None
    compare_file = None

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--backend" and i + 1 < len(args):
            backend = args[i + 1]
            i += 2
        elif args[i] == "--output" and i + 1 < len(args):
            output_file = args[i + 1]
            i += 2
        elif args[i] == "--compare" and i + 1 < len(args):
            compare_file = args[i + 1]
            i += 2
        elif args[i].startswith("--"):
            print(f"Unknown option: {args[i]}", file=sys.stderr)
            sys.exit(1)
        else:
            binary = os.path.abspath(args[i])
            i += 1

    if not binary:
        print(
            f"Usage: {sys.argv[0]} <path-to-ptrclaw-binary> "
            f"[--backend sqlite|json] [--output FILE] [--compare FILE]",
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

    # Load baseline for comparison
    baseline = None
    if compare_file:
        with open(compare_file) as f:
            baseline = json.load(f)
        print(f"Comparing against baseline: {compare_file}", file=sys.stderr)

    print(f"Running memory recall benchmark...", file=sys.stderr)
    print(f"  Backend: {backend}, Model: {MODEL}", file=sys.stderr)

    results = run_benchmark(binary, backend)
    scores = [r["score"] for r in results]
    mean = sum(scores) / len(scores)
    passed = mean >= 0.5

    # Summary table to stdout
    print_results(results, backend, baseline)

    # Save structured results for comparison
    if output_file:
        output = {
            "backend": backend,
            "model": MODEL,
            "judge_model": JUDGE_MODEL,
            "seed_messages": len(SEED_MESSAGES),
            "mean_score": round(mean, 3),
            "passed": passed,
            "questions": [
                {
                    "question": r["question"],
                    "score": round(r["score"], 3),
                    "ground_truth": r["ground_truth"],
                    "response": r["response"],
                }
                for r in results
            ],
        }
        with open(output_file, "w") as f:
            json.dump(output, f, indent=2)
        print(f"Results saved to {output_file}", file=sys.stderr)

    # Machine-readable output for CI
    print(f"::notice::Memory benchmark ({backend}): mean={mean:.2f}")

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
