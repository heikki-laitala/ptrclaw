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
JUDGE_MODEL = "claude-opus-4-6-20250514"
TIMEOUT_SECONDS = 180

# ── Conversation scripts ─────────────────────────────────────────────

# ── Scenario 1: Single topic (analytics dashboard) ───────────────────

SINGLE_TOPIC_SEED = [
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

SINGLE_TOPIC_TESTS = [
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

# ── Scenario 2: Multi-topic with interleaved recall ──────────────────
# Topic A = analytics dashboard (reuses first 8 messages from above)
# Topic B = companion mobile app (new topic)
# Test questions jump between topics like a real conversation

MULTI_TOPIC_SEED_A = SINGLE_TOPIC_SEED[:8]

MULTI_TOPIC_SEED_B = [
    "We're starting a companion mobile app for the analytics dashboard. "
    "It'll be built with React Native so we can ship iOS and Android from "
    "a single codebase. The lead developer is Tomoko.",

    "The mobile app will focus on three core features: push notification "
    "alerts when metrics cross thresholds, a simplified dashboard view "
    "with the top 5 KPIs, and the ability to annotate data points with "
    "voice memos that get transcribed using Whisper.",

    "Tomoko chose Zustand for state management and React Query for API "
    "caching. The app communicates with the same Go backend that Marcus "
    "built for the web dashboard, so no new API work needed.",

    "For authentication, the mobile app will use biometric login (Face ID "
    "and fingerprint) backed by the same OAuth 2.0 service that Marcus "
    "migrated. Refresh tokens are stored in the secure keychain.",

    "The push notification system uses Firebase Cloud Messaging. Tomoko "
    "set up a notification service in Go that listens to the Kafka event "
    "stream and triggers alerts based on user-configured rules. Each user "
    "can define up to 10 alert rules.",

    "We estimated the mobile app timeline. Beta release is April 30th, "
    "two weeks after the web dashboard GA. The app will initially be "
    "internal-only, then open to customers in June. The App Store review "
    "process needs 2 weeks buffer.",

    "Tomoko raised a concern about offline support. The app needs to cache "
    "the last 24 hours of dashboard data locally using SQLite on-device. "
    "When connectivity resumes, it syncs and reconciles with the server. "
    "She estimates this adds 3 weeks to the timeline.",

    "The mobile app budget is separate: $2,800 per month. That covers "
    "Firebase ($400), App Store developer accounts ($100), TestFlight "
    "and CI with Bitrise ($600), and Tomoko's contractor rate of $1,700 "
    "per month for the initial 4-month engagement.",
]

MULTI_TOPIC_SEED = MULTI_TOPIC_SEED_A + MULTI_TOPIC_SEED_B

MULTI_TOPIC_TESTS = [
    # Topic A
    {
        "question": "What technology stack was chosen for the analytics dashboard?",
        "ground_truth": "Vue.js frontend, Go backend, PostgreSQL with TimescaleDB",
        "topic": "A",
    },
    # Topic B
    {
        "question": "Who is the lead developer for the mobile app?",
        "ground_truth": "Tomoko is the lead developer for the mobile app",
        "topic": "B",
    },
    # Topic A
    {
        "question": "How does the data pipeline process events?",
        "ground_truth": (
            "Events come from Kafka topics, processed by Apache Flink for "
            "stream processing, raw events stored in S3, then loaded into "
            "TimescaleDB. Peak throughput is 50,000 events per second"
        ),
        "topic": "A",
    },
    # Topic B
    {
        "question": "What are the three core features of the mobile app?",
        "ground_truth": (
            "Push notification alerts for metric thresholds, simplified "
            "dashboard with top 5 KPIs, and voice memo annotations on data "
            "points transcribed using Whisper"
        ),
        "topic": "B",
    },
    # Topic A
    {
        "question": "What's the main blocker for the dashboard integration?",
        "ground_truth": (
            "OAuth 1.0 to OAuth 2.0 migration was the blocker, Marcus "
            "handled it and finished ahead of schedule"
        ),
        "topic": "A",
    },
    # Topic B
    {
        "question": "How does the mobile app handle offline support?",
        "ground_truth": (
            "Caches last 24 hours of dashboard data locally using SQLite "
            "on-device, syncs and reconciles with server when connectivity "
            "resumes. Adds 3 weeks to timeline"
        ),
        "topic": "B",
    },
    # Topic A
    {
        "question": "What are the main panels on the dashboard UI?",
        "ground_truth": (
            "Four panels: real-time event stream, retention cohort chart, "
            "revenue funnel, and geographic heatmap. Uses Tailwind CSS "
            "with a custom component library called Prism"
        ),
        "topic": "A",
    },
    # Topic B
    {
        "question": "What's the mobile app budget breakdown?",
        "ground_truth": (
            "$2,800 per month: Firebase $400, App Store accounts $100, "
            "Bitrise CI $600, Tomoko's contractor rate $1,700/month "
            "for 4-month engagement"
        ),
        "topic": "B",
    },
    # Topic A
    {
        "question": "How is rate limiting handled in the backend?",
        "ground_truth": (
            "Redis with token bucket algorithm, 1000 requests per second "
            "per tenant, Redis cluster on ElastiCache. Marcus designed this "
            "for burst traffic during marketing campaigns"
        ),
        "topic": "A",
    },
    # Topic B
    {
        "question": "How do push notifications work in the mobile app?",
        "ground_truth": (
            "Firebase Cloud Messaging, Go notification service listens to "
            "Kafka event stream, triggers alerts based on user-configured "
            "rules. Each user can define up to 10 alert rules"
        ),
        "topic": "B",
    },
    # Cross-topic
    {
        "question": "Compare the timelines for the web dashboard and mobile app.",
        "ground_truth": (
            "Web dashboard MVP deadline is March 15th. Mobile app beta is "
            "April 30th (two weeks after web GA), internal-only first, "
            "then open to customers in June. App Store review needs 2 weeks"
        ),
        "topic": "A+B",
    },
    # Cross-topic
    {
        "question": (
            "Which team members work on both the web dashboard and mobile "
            "app, and how do the projects share infrastructure?"
        ),
        "ground_truth": (
            "Marcus built the Go backend used by both web and mobile. The "
            "mobile app reuses the same API and OAuth 2.0 auth service. "
            "Tomoko's notification service also listens to Priya's Kafka "
            "event stream. Sarah leads web frontend, Tomoko leads mobile"
        ),
        "topic": "A+B",
    },
    # Topic A
    {
        "question": "Who is Diego and what is he working on?",
        "ground_truth": (
            "Diego is a new contractor starting next Monday, assigned to "
            "SAML integration for enterprise SSO customers"
        ),
        "topic": "A",
    },
    # Cross-topic
    {
        "question": "What's the combined monthly infrastructure cost?",
        "ground_truth": (
            "Web dashboard AWS: $4,200/month (EKS $1,800, ElastiCache $900, "
            "RDS $600, S3 $500, Datadog $400). Mobile app: $2,800/month "
            "(Firebase $400, App Store $100, Bitrise $600, Tomoko $1,700). "
            "Combined roughly $7,000/month"
        ),
        "topic": "A+B",
    },
    # Cross-topic
    {
        "question": "Summarize what each team member is responsible for across both projects.",
        "ground_truth": (
            "Sarah leads web frontend and designed dashboard wireframes, "
            "Marcus handles backend services including auth and rate limiting "
            "(shared by both web and mobile), Priya owns the data pipeline "
            "with Kafka/Flink, Diego handles SAML SSO, Tomoko leads mobile app"
        ),
        "topic": "A+B",
    },
]

# All scenarios for the runner
SCENARIOS = [
    {
        "name": "single-topic",
        "description": "Single topic recall (analytics dashboard)",
        "seed": SINGLE_TOPIC_SEED,
        "tests": SINGLE_TOPIC_TESTS,
    },
    {
        "name": "multi-topic",
        "description": "Multi-topic interleaved recall (dashboard + mobile app)",
        "seed": MULTI_TOPIC_SEED,
        "tests": MULTI_TOPIC_TESTS,
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
            "synthesis_interval": 5,
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


def run_scenario(binary, backend, scenario):
    """Run a single scenario: seed → test → score. Returns list of result dicts."""
    seed = scenario["seed"]
    tests = scenario["tests"]
    home = tempfile.mkdtemp(prefix=f"ptrclaw_bench_{scenario['name']}_")
    try:
        make_config(home, backend)

        # Phase 1: Seed
        print(f"\n  Phase 1: Seeding ({len(seed)} messages)...",
              file=sys.stderr)
        proc = start_pipe(binary, home)
        for i, msg in enumerate(seed):
            resp = send_message(proc, msg)
            print(f"    Seed {i + 1}: sent ({len(resp)} chars response)",
                  file=sys.stderr)
        close_pipe(proc)
        print("    Seed phase complete, memories accumulated.",
              file=sys.stderr)

        # Phase 2: Test — new process, same HOME (same memory files)
        print(f"\n  Phase 2: Testing ({len(tests)} questions)...",
              file=sys.stderr)
        proc = start_pipe(binary, home)
        responses = []
        for i, tc in enumerate(tests):
            resp = send_message(proc, tc["question"])
            responses.append(resp)
            print(f"    Q{i + 1}: {resp[:120]}...", file=sys.stderr)
        close_pipe(proc)

        # Phase 3: Score — LLM judge
        print("\n  Phase 3: Scoring with LLM judge...", file=sys.stderr)
        results = []
        for i, (tc, resp) in enumerate(zip(tests, responses)):
            score = llm_judge(resp, tc["ground_truth"])
            results.append({
                "question": tc["question"],
                "ground_truth": tc["ground_truth"],
                "topic": tc.get("topic", ""),
                "response": resp,
                "score": score,
            })
            print(f"    Q{i + 1}: {score:.2f}", file=sys.stderr)

        return results
    finally:
        shutil.rmtree(home, ignore_errors=True)


def print_scenario_results(name, results, baseline_questions=None):
    """Print results table for one scenario."""
    scores = [r["score"] for r in results]
    mean = sum(scores) / len(scores)

    # Build baseline lookup
    base_scores = {}
    if baseline_questions:
        for bq in baseline_questions:
            base_scores[bq["question"]] = bq["score"]

    has_baseline = bool(base_scores)
    w = 78 if has_baseline else 66

    print(f"\n  {name}")
    print(f"  {'─' * w}")
    if has_baseline:
        print(f"   {'#':<4} {'Score':>5}  {'Base':>5}  {'Delta':>6}  "
              f"{'Topic':<5} Question")
    else:
        print(f"   {'#':<4} {'Score':>5}  {'Topic':<5} Question")
    print(f"  {'─' * w}")

    for i, r in enumerate(results):
        tag = "✓" if r["score"] >= 0.5 else "✗"
        topic = r.get("topic", "")
        q_max = 38 if has_baseline else 46
        q = r["question"][:q_max]
        if has_baseline:
            bs = base_scores.get(r["question"])
            if bs is not None:
                delta = r["score"] - bs
                sign = "+" if delta > 0 else ""
                print(f"   {i + 1:<4} {r['score']:>5.2f}  {bs:>5.2f}  "
                      f"{sign}{delta:>5.2f}  {topic:<5} {tag} {q}")
            else:
                print(f"   {i + 1:<4} {r['score']:>5.2f}     —      —  "
                      f"{topic:<5} {tag} {q}")
        else:
            print(f"   {i + 1:<4} {r['score']:>5.2f}  {topic:<5} {tag} {q}")

    print(f"  {'─' * w}")
    print(f"   Mean: {mean:.2f}  ({len(results)} questions)")
    return mean


def print_summary(scenario_means, backend, baseline=None):
    """Print the overall summary across all scenarios."""
    overall = sum(scenario_means.values()) / len(scenario_means)
    passed = overall >= 0.5

    base_overall = None
    if baseline:
        base_overall = baseline.get("overall_mean")

    print(f"\n{'=' * 60}")
    for name, mean in scenario_means.items():
        print(f"  {name:.<40} {mean:.2f}")
    print(f"  {'─' * 56}")
    if base_overall is not None:
        delta = overall - base_overall
        sign = "+" if delta > 0 else ""
        print(f"  {'Overall':.<40} {overall:.2f}  "
              f"(baseline: {base_overall:.2f}, delta: {sign}{delta:.2f})")
    else:
        print(f"  {'Overall':.<40} {overall:.2f}")
    print(f"\n  Result:  {'PASS' if passed else 'FAIL'} (threshold: 0.50)")
    print(f"  Model:   {MODEL}")
    print(f"  Judge:   {JUDGE_MODEL}")
    print(f"  Backend: {backend}")
    print(f"{'=' * 60}")
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

    # Run all scenarios
    all_results = {}
    scenario_means = {}
    for scenario in SCENARIOS:
        name = scenario["name"]
        print(f"\n{'─' * 40}", file=sys.stderr)
        print(f"  Scenario: {scenario['description']}", file=sys.stderr)
        print(f"{'─' * 40}", file=sys.stderr)

        results = run_scenario(binary, backend, scenario)
        all_results[name] = results

        # Print per-scenario table
        base_qs = None
        if baseline and name in baseline.get("scenarios", {}):
            base_qs = baseline["scenarios"][name].get("questions")
        mean = print_scenario_results(
            scenario["description"], results, base_qs)
        scenario_means[name] = mean

    # Overall summary
    print_summary(scenario_means, backend, baseline)

    overall = sum(scenario_means.values()) / len(scenario_means)
    passed = overall >= 0.5

    # Save structured results for comparison
    if output_file:
        output = {
            "backend": backend,
            "model": MODEL,
            "judge_model": JUDGE_MODEL,
            "overall_mean": round(overall, 3),
            "passed": passed,
            "scenarios": {
                name: {
                    "mean_score": round(scenario_means[name], 3),
                    "seed_messages": len(
                        next(s for s in SCENARIOS if s["name"] == name)["seed"]
                    ),
                    "questions": [
                        {
                            "question": r["question"],
                            "score": round(r["score"], 3),
                            "topic": r.get("topic", ""),
                            "ground_truth": r["ground_truth"],
                            "response": r["response"],
                        }
                        for r in results
                    ],
                }
                for name, results in all_results.items()
            },
        }
        with open(output_file, "w") as f:
            json.dump(output, f, indent=2)
        print(f"Results saved to {output_file}", file=sys.stderr)

    # Machine-readable output for CI
    for name, mean in scenario_means.items():
        print(f"::notice::Memory benchmark {name} ({backend}): mean={mean:.2f}")
    print(f"::notice::Memory benchmark overall ({backend}): mean={overall:.2f}")

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
