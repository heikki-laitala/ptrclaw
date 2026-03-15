#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["anthropic>=0.40"]
# ///
"""Memory redesign benchmark for ptrclaw.

Compares memory behavior between main and feat/memory-redesign by driving
deterministic conversation scenarios through the pipe channel and measuring
compaction quality, episode recovery, concept formation, and context
boundedness.

Unlike memory_benchmark.py (CI recall scoring), this benchmark focuses on
the structural improvements in the memory redesign: structured episode
summaries, recoverable archives, and concept vs observation classification.

Usage:
    # Build with pipe channel:
    make build-pipe

    # Run all scenarios:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw

    # Run a single scenario:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw --scenario long_planning

    # Run multiple specific scenarios:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw \
        --scenario long_planning --scenario preference_refinement

    # Save results for later comparison:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw --output main.json

    # Compare against a baseline:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw \
        --output redesign.json --compare main.json

    # Use JSON memory backend instead of SQLite:
    uv run tests/e2e/memory_redesign_bench.py ./builddir-pipe/ptrclaw --backend json

Available scenarios:
    long_planning              Long planning session — compaction quality
    preference_refinement      Preference refinement — concept formation
    revisit_after_compaction   Topic revisited after compaction — episode recovery
    facts_vs_chronology        Facts vs chronology — concept/observation distinction

Requires ANTHROPIC_API_KEY in the environment.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from anthropic import Anthropic

MODEL = "claude-sonnet-4-6"
JUDGE_MODEL = "claude-opus-4-6"
TIMEOUT_SECONDS = 180

# Pacing — conservative to avoid rate limits
MESSAGE_DELAY = 2
SEED_MESSAGE_DELAY = 5
JUDGE_DELAY = 0.5

# Rate limit budgets (Anthropic Sonnet 4.x, with safety margin)
REQUEST_BUDGET_PER_MINUTE = 35
INPUT_TOKEN_BUDGET_PER_MINUTE = 18000
OUTPUT_TOKEN_BUDGET_PER_MINUTE = 5000

MODEL_CALL_INPUT_RESERVATION = 6500
MODEL_CALL_OUTPUT_RESERVATION = 2000
SEED_CALL_INPUT_RESERVATION = 11000
SEED_CALL_OUTPUT_RESERVATION = 3000
RATE_LIMIT_COOLDOWN_SECONDS = 65

JUDGE_MAX_TOKENS = 64
JUDGE_CALL_INPUT_RESERVATION = 1200
JUDGE_CALL_OUTPUT_RESERVATION = JUDGE_MAX_TOKENS


def estimate_tokens(text):
    """Rough token estimate for throttling (chars/4 heuristic)."""
    return max(1, len(text) // 4)


class TokenRateLimiter:
    """Simple sliding-window limiter (60s) for tokens/requests."""

    def __init__(self, budget_per_minute):
        self.budget = budget_per_minute
        self.events = []

    def _trim(self, now):
        cutoff = now - 60.0
        self.events = [(t, tok) for t, tok in self.events if t >= cutoff]

    def _used(self, now):
        self._trim(now)
        return sum(tok for _, tok in self.events)

    def wait_for(self, tokens):
        while True:
            now = time.time()
            used = self._used(now)
            if used + tokens <= self.budget:
                self.events.append((now, tokens))
                return
            oldest_ts = self.events[0][0]
            sleep_s = max(0.25, 60.0 - (now - oldest_ts) + 0.05)
            time.sleep(sleep_s)


# ── Scenario fixtures ────────────────────────────────────────────────────

# Scenario A: Long planning session — tests compaction quality
# Many turns with decisions, entities, and an unresolved item.
# After compaction, follow-up questions test whether key decisions survive.

SCENARIO_A_SEED = [
    "Let's plan the migration of our monolith to microservices. The current "
    "system is a Rails app called Titan, deployed on Heroku. It handles "
    "billing, user management, notifications, and content delivery.",

    "We decided to split into four services: billing-svc (Go), users-svc "
    "(Go), notifications-svc (Python), and content-svc (Rust for performance). "
    "Each service gets its own PostgreSQL database.",

    "For inter-service communication, we chose gRPC for synchronous calls and "
    "RabbitMQ for async events. The API gateway will be Kong running on "
    "Kubernetes. Service mesh is Istio for mTLS between services.",

    "The team: Alice leads billing-svc, Bob handles users-svc, Carol owns "
    "notifications-svc, and Dave is building content-svc. Eve is the SRE "
    "handling infrastructure and CI/CD.",

    "Alice raised a concern: the billing service needs to handle PCI "
    "compliance. We can't move billing data to a new database without a "
    "formal audit. She estimates the audit will take 6 weeks and cost $15,000. "
    "This is unresolved — we need budget approval from finance.",

    "Bob finished the users-svc schema design. It includes OAuth 2.0 with "
    "PKCE, RBAC with four roles (admin, editor, viewer, billing-admin), "
    "and SCIM provisioning for enterprise SSO. The migration script handles "
    "500,000 existing user records.",

    "Carol proposed using Firebase Cloud Messaging for push notifications "
    "and SendGrid for email. The notification service will process events "
    "from RabbitMQ and batch emails to avoid rate limits. She's implementing "
    "user preference management for notification channels.",

    "Dave benchmarked the Rust content service against the Rails version. "
    "Results: 12x throughput improvement (from 800 req/s to 9,600 req/s), "
    "95th percentile latency dropped from 450ms to 38ms. Memory usage went "
    "from 2.1 GB to 340 MB for the same workload.",

    "Eve set up the Kubernetes cluster on GKE with 3 node pools: general "
    "(n2-standard-4), compute (c2-standard-8 for content-svc), and spot "
    "instances for batch jobs. Total monthly infrastructure estimate is "
    "$8,400, down from $12,000 on Heroku.",

    "In the latest sync, we decided on the migration order: (1) content-svc "
    "first because it has no billing dependencies, (2) users-svc second, "
    "(3) notifications-svc third, (4) billing-svc last after the PCI audit. "
    "Each migration phase gets 3 weeks including a 1-week canary period.",

    "Dave noticed that the content service needs access to user permissions "
    "for content gating. Rather than direct database access, Bob will expose "
    "a gRPC endpoint on users-svc: CheckPermission(user_id, resource_id) "
    "returning allowed/denied with a 50ms SLA.",

    "Eve added observability: Prometheus for metrics, Grafana dashboards, "
    "Jaeger for distributed tracing, and PagerDuty for alerting. Each service "
    "must expose /healthz and /readyz endpoints. SLO target is 99.9% "
    "availability with 5-minute mean time to detect.",

    "The data migration strategy: each service runs a dual-write phase where "
    "both the monolith and the new service write to both old and new databases. "
    "After validation, we cut over reads, then stop monolith writes. Bob "
    "estimates this adds 2 weeks per service but avoids data loss risk.",

    "Carol's notification service will also handle in-app notifications via "
    "WebSocket connections. She's using Redis Pub/Sub for real-time delivery "
    "and PostgreSQL for notification history. Users can mute per-channel or "
    "set quiet hours (stored as timezone-aware CRON expressions).",

    "We reviewed the rollback plan. If any service fails during canary, "
    "Kong's traffic splitting reverts to 100% monolith within 30 seconds. "
    "Database rollback is handled by the dual-write — just stop writing to "
    "the new database and the monolith continues uninterrupted.",
]

SCENARIO_A_FOLLOWUPS = [
    {
        "question": "What's the migration order we decided on and why?",
        "expected_signals": [
            "content-svc first",
            "no billing dependencies",
            "billing-svc last",
            "PCI audit",
            "3 weeks per phase",
        ],
    },
    {
        "question": "What's the unresolved blocker we still need to address?",
        "expected_signals": [
            "PCI compliance audit",
            "billing",
            "budget approval",
            "finance",
            "$15,000",
        ],
    },
    {
        "question": "Who is responsible for each service?",
        "expected_signals": [
            "Alice.*billing",
            "Bob.*users",
            "Carol.*notification",
            "Dave.*content",
            "Eve.*SRE|infrastructure",
        ],
    },
    {
        "question": "What were the content-svc benchmark results?",
        "expected_signals": [
            "12x throughput",
            "9,?600 req/s",
            "38ms.*latency",
            "340 MB memory",
        ],
    },
]

# Scenario B: Repeated preference refinement — tests concept formation
# User states a preference, reinforces it, then adds an exception.
# Tests whether memory generalizes preferences vs duplicating notes.

SCENARIO_B_SEED = [
    "I strongly prefer functional programming patterns. When writing code, "
    "always favor immutable data structures, pure functions, and composition "
    "over inheritance.",

    "For error handling, I want Result types instead of exceptions. Use "
    "std::expected in C++23 or a custom Result<T,E> template. Never throw "
    "exceptions for expected failure paths.",

    "Let me reinforce: functional style is really important to me. Map, "
    "filter, reduce over raw loops. Const by default. No mutable state "
    "unless absolutely necessary for performance.",

    "Also, I dislike overly verbose code. Prefer concise, expressive "
    "constructs. One-liner lambdas are fine. Don't add unnecessary "
    "intermediate variables just for 'readability' — the types and function "
    "names should make the intent clear.",

    "Actually, there's an exception to my functional preference: for hot "
    "loops in performance-critical paths, I'm fine with mutable state and "
    "imperative style. The key criterion is measurable performance impact. "
    "If benchmarks show the functional version is within 5% of imperative, "
    "use functional. Otherwise, imperative is acceptable.",

    "One more nuance: I prefer functional patterns in application logic but "
    "accept OOP for the plugin/extension system. Abstract base classes with "
    "virtual methods are the right tool for provider/channel/tool interfaces "
    "because they give clean extensibility. It's not about dogma, it's about "
    "fitness for the specific problem.",

    "My naming conventions: snake_case for functions and variables, "
    "PascalCase for types, SCREAMING_SNAKE for constants. Trailing "
    "underscore for member variables. These are non-negotiable.",

    "On testing: I want tests that test behavior, not implementation. "
    "Mock only at system boundaries (network, filesystem). Never mock "
    "internal classes — if you need to mock it, the design is wrong.",
]

SCENARIO_B_FOLLOWUPS = [
    {
        "question": (
            "What's my coding style preference? Give me the general rule "
            "and any exceptions."
        ),
        "expected_signals": [
            "functional",
            "immutable",
            "exception.*performance|hot loop",
            "OOP.*plugin|interface|extension",
        ],
    },
    {
        "question": "How should error handling be done?",
        "expected_signals": [
            "Result type",
            "no exceptions",
            "expected failure",
        ],
    },
    {
        "question": (
            "If I asked you to write a hot loop that processes 10M records, "
            "would you use functional or imperative style?"
        ),
        "expected_signals": [
            "imperative|mutable",
            "performance",
            "benchmark|measur",
        ],
    },
]

# Scenario C: Topic revisited after compaction — tests episode recovery
# Discuss a topic, have many unrelated turns (causing compaction), then
# return to the original topic. Tests whether old context is recoverable.

SCENARIO_C_SEED_TOPIC = [
    "I'm debugging a segfault in the WebSocket connection pool. The crash "
    "happens in ConnectionPool::release() when a connection is returned "
    "after the pool has been resized. I think there's a use-after-free on "
    "the connection's internal buffer.",

    "I found the root cause: when the pool shrinks, it destroys excess "
    "Connection objects, but active connections aren't tracked. A thread "
    "can still hold a raw pointer to a destroyed connection. The fix is "
    "to use shared_ptr for connection ownership and weak_ptr in the pool's "
    "free list.",

    "The fix also needs a maximum idle timeout. Connections sitting in the "
    "pool for more than 60 seconds should be destroyed proactively to "
    "prevent stale TCP connections. I'll add a background cleanup thread "
    "with a condition_variable for shutdown coordination.",
]

SCENARIO_C_FILLER = [
    "What's the weather like in Helsinki today?",
    "Can you explain how B-trees work in database indexing?",
    "Write me a haiku about programming.",
    "What are the main differences between TCP and UDP?",
    "Recommend a good book on distributed systems.",
    "How does garbage collection work in Java vs Go?",
    "What's the difference between REST and GraphQL?",
    "Explain the CAP theorem in simple terms.",
    "How do hash maps handle collisions?",
    "What are the SOLID principles in software design?",
    "How does TLS 1.3 differ from TLS 1.2?",
    "What's the difference between a mutex and a semaphore?",
    "Explain eventual consistency with a real-world example.",
    "How do container orchestrators like Kubernetes schedule pods?",
    "What are the tradeoffs of microservices vs monoliths?",
]

SCENARIO_C_REVISIT = [
    {
        "question": (
            "Remember that WebSocket connection pool bug I was debugging "
            "earlier? What was the root cause and the fix I decided on?"
        ),
        "expected_signals": [
            "use-after-free",
            "pool.*resize|shrink",
            "shared_ptr",
            "weak_ptr",
            "idle timeout|60 second",
        ],
    },
    {
        "question": (
            "What was the cleanup mechanism I planned for idle connections?"
        ),
        "expected_signals": [
            "background.*thread",
            "condition_variable",
            "60 second|idle timeout",
        ],
    },
]

# Scenario D: Facts vs chronology — tests concept vs observation distinction
# Mix stable facts with time-ordered decisions. Follow-up tests whether the
# system distinguishes durable knowledge from episode-specific observations.

SCENARIO_D_SEED = [
    "Our production database is PostgreSQL 16 running on AWS RDS. The "
    "instance is db.r6g.xlarge with 500 GB gp3 storage. This is a permanent "
    "infrastructure choice — we're committed to PostgreSQL.",

    "The application language is Rust. We chose Rust for memory safety and "
    "performance. This is a long-term commitment, not something we'll change.",

    "Today we decided to add a Redis cache layer for the /api/users endpoint "
    "because response times exceeded our 100ms SLA. The cache TTL is 5 minutes.",

    "Our team uses trunk-based development with short-lived feature branches. "
    "PRs require one approval and passing CI. This is our standard workflow.",

    "In yesterday's incident, the database hit connection limits during a "
    "traffic spike. We temporarily increased max_connections from 200 to 400 "
    "as an emergency measure. We should revert this and use PgBouncer instead.",

    "Our deployment pipeline: GitHub Actions builds, then ArgoCD deploys to "
    "GKE. Staging auto-deploys on main merge, production requires manual "
    "approval. This is our permanent CI/CD setup.",

    "This morning we found a bug in the user signup flow — email validation "
    "regex allows invalid TLDs. Carol is fixing it in PR #847. This should "
    "be patched by end of day.",

    "We always run database migrations during the weekly maintenance window "
    "(Sundays 02:00-04:00 UTC). This is policy, not a one-time decision.",
]

SCENARIO_D_FOLLOWUPS = [
    {
        "question": (
            "What are the permanent, long-term technology choices for this "
            "project? Only list things that won't change."
        ),
        "expected_signals": [
            "PostgreSQL",
            "Rust",
            "trunk-based development",
            "GitHub Actions.*ArgoCD",
            "Sunday.*maintenance",
        ],
    },
    {
        "question": (
            "What are the recent, possibly temporary decisions or incidents "
            "from this week?"
        ),
        "expected_signals": [
            "Redis cache",
            "max_connections.*400",
            "email validation.*bug|PR.*847",
        ],
    },
    {
        "question": (
            "Should we still have max_connections at 400 or was that "
            "supposed to be temporary?"
        ),
        "expected_signals": [
            "temporary|emergency",
            "revert",
            "PgBouncer",
        ],
    },
]


# ── All scenarios ────────────────────────────────────────────────────────

SCENARIOS = [
    {
        "name": "long_planning",
        "description": "Long planning session — compaction quality",
        "seed": SCENARIO_A_SEED,
        "followups": SCENARIO_A_FOLLOWUPS,
        "config": {"max_history_messages": 12},
    },
    {
        "name": "preference_refinement",
        "description": "Preference refinement — concept formation",
        "seed": SCENARIO_B_SEED,
        "followups": SCENARIO_B_FOLLOWUPS,
        "config": {"max_history_messages": 8},
    },
    {
        "name": "revisit_after_compaction",
        "description": "Topic revisited after compaction — episode recovery",
        "seed": SCENARIO_C_SEED_TOPIC + SCENARIO_C_FILLER,
        "followups": SCENARIO_C_REVISIT,
        "config": {"max_history_messages": 8},
    },
    {
        "name": "facts_vs_chronology",
        "description": "Facts vs chronology — concept/observation distinction",
        "seed": SCENARIO_D_SEED,
        "followups": SCENARIO_D_FOLLOWUPS,
        "config": {"max_history_messages": 8},
    },
]


# ── Pipe channel helpers ─────────────────────────────────────────────────


def make_config(home, backend="sqlite", agent_overrides=None):
    """Write ptrclaw config.json."""
    config_dir = os.path.join(home, ".ptrclaw")
    os.makedirs(config_dir, exist_ok=True)
    agent_config = {
        "max_history_messages": 20,
    }
    if agent_overrides:
        agent_config.update(agent_overrides)
    config = {
        "provider": "anthropic",
        "model": MODEL,
        "agent": agent_config,
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
    """Start ptrclaw in pipe mode."""
    env = os.environ.copy()
    env["HOME"] = home
    return subprocess.Popen(
        [binary, "--channel", "pipe"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,
        text=True,
        env=env,
    )


def send_message(proc, content, retries=1):
    """Send a JSONL message and read the response."""
    attempts = retries + 1
    for i in range(attempts):
        line = json.dumps({"content": content}) + "\n"
        proc.stdin.write(line)
        proc.stdin.flush()
        resp_line = proc.stdout.readline()
        if not resp_line:
            raise RuntimeError("pipe: no response (check stderr output above)")
        resp = json.loads(resp_line).get("content", "")
        low = resp.lower()
        if "http 429" in low or "rate limit" in low:
            if i < attempts - 1:
                print(
                    f"    Rate limit hit; cooling down "
                    f"{RATE_LIMIT_COOLDOWN_SECONDS}s and retrying...",
                    file=sys.stderr,
                )
                time.sleep(RATE_LIMIT_COOLDOWN_SECONDS)
                continue
        return resp
    return ""


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


# ── Metric extraction ────────────────────────────────────────────────────


def check_signals(text, signals):
    """Check how many expected signals appear in text. Returns (found, total)."""
    found = 0
    for signal in signals:
        if re.search(signal, text, re.IGNORECASE):
            found += 1
    return found, len(signals)


def inspect_memory_files(home):
    """Read memory files from the ptrclaw home directory.

    Returns a dict with counts and content for analysis.
    """
    memory_dir = os.path.join(home, ".ptrclaw")
    info = {
        "memory_entries": 0,
        "memory_files": [],
        "memory_content": "",
    }

    # JSON memory backend
    json_file = os.path.join(memory_dir, "memory.json")
    if os.path.exists(json_file):
        try:
            with open(json_file) as f:
                data = json.load(f)
            if isinstance(data, list):
                info["memory_entries"] = len(data)
                info["memory_content"] = json.dumps(data, indent=2)
            elif isinstance(data, dict):
                entries = data.get("entries", [])
                info["memory_entries"] = len(entries)
                info["memory_content"] = json.dumps(entries, indent=2)
        except (json.JSONDecodeError, KeyError):
            pass
        info["memory_files"].append(json_file)

    # SQLite memory backend
    sqlite_file = os.path.join(memory_dir, "memory.db")
    if os.path.exists(sqlite_file):
        info["memory_files"].append(sqlite_file)
        try:
            import sqlite3

            conn = sqlite3.connect(sqlite_file)
            cursor = conn.execute("SELECT COUNT(*) FROM memories")
            info["memory_entries"] = cursor.fetchone()[0]
            cursor = conn.execute(
                "SELECT key, content, category FROM memories"
            )
            rows = cursor.fetchall()
            info["memory_content"] = json.dumps(
                [{"key": r[0], "content": r[1], "category": r[2]} for r in rows],
                indent=2,
            )
            conn.close()
        except Exception:
            pass

    return info


# ── LLM judge ────────────────────────────────────────────────────────────


def llm_judge_signals(client, response, expected_signals,
                      input_limiter, output_limiter, request_limiter):
    """Score a response for presence of expected signals using LLM judge.

    Returns a float between 0.0 and 1.0.
    """
    signals_text = "\n".join(f"- {s}" for s in expected_signals)
    request_text = (
        f"Expected signals (key facts/concepts the response should contain):\n"
        f"{signals_text}\n\n"
        f"Response to evaluate:\n{response}"
    )
    input_limiter.wait_for(
        max(JUDGE_CALL_INPUT_RESERVATION, estimate_tokens(request_text))
    )
    output_limiter.wait_for(JUDGE_CALL_OUTPUT_RESERVATION)
    request_limiter.wait_for(1)

    result = client.messages.create(
        model=JUDGE_MODEL,
        max_tokens=JUDGE_MAX_TOKENS,
        system=(
            "You are a scoring assistant. Rate how well a response "
            "demonstrates awareness of specific expected signals.\n\n"
            "Score on a continuous scale from 0.0 to 1.0:\n"
            "- 1.0: All expected signals are clearly present\n"
            "- 0.7-0.9: Most signals present, minor gaps\n"
            "- 0.4-0.6: Some signals present, partially correct\n"
            "- 0.1-0.3: Vague awareness, mostly missing\n"
            "- 0.0: No awareness of the expected signals\n\n"
            "Output only the numeric score, nothing else."
        ),
        messages=[{"role": "user", "content": request_text}],
    )
    try:
        score = float(result.content[0].text.strip())
        return max(0.0, min(1.0, score))
    except ValueError:
        return 0.0


# ── Benchmark runner ─────────────────────────────────────────────────────


def run_scenario(binary, backend, scenario, limiters):
    """Run a single scenario. Returns a dict of metrics."""
    seed = scenario["seed"]
    followups = scenario["followups"]
    config_overrides = scenario.get("config", {})
    home = tempfile.mkdtemp(prefix=f"ptrclaw_redesign_{scenario['name']}_")

    input_limiter, output_limiter, request_limiter = limiters

    try:
        make_config(home, backend, config_overrides)

        # Phase 1: Seed conversation
        print(
            f"\n  Phase 1: Seeding ({len(seed)} messages)...", file=sys.stderr
        )
        proc = start_pipe(binary, home)
        seed_responses = []
        for i, msg in enumerate(seed):
            if i > 0:
                time.sleep(SEED_MESSAGE_DELAY)
            input_limiter.wait_for(
                max(SEED_CALL_INPUT_RESERVATION, estimate_tokens(msg))
            )
            output_limiter.wait_for(SEED_CALL_OUTPUT_RESERVATION)
            request_limiter.wait_for(1)
            resp = send_message(proc, msg, retries=1)
            seed_responses.append(resp)
            print(
                f"    Seed {i + 1}/{len(seed)}: sent ({len(resp)} chars)",
                file=sys.stderr,
            )
        close_pipe(proc)
        print(
            "    Seed phase complete. Cooling down before follow-ups...",
            file=sys.stderr,
        )
        time.sleep(RATE_LIMIT_COOLDOWN_SECONDS)

        # Inspect memory state after seeding
        memory_after_seed = inspect_memory_files(home)

        # Phase 2: Follow-up questions (new process, same HOME)
        print(
            f"\n  Phase 2: Follow-ups ({len(followups)} questions)...",
            file=sys.stderr,
        )
        proc = start_pipe(binary, home)
        followup_responses = []
        for i, fu in enumerate(followups):
            if i > 0:
                time.sleep(MESSAGE_DELAY)
            input_limiter.wait_for(
                max(
                    MODEL_CALL_INPUT_RESERVATION,
                    estimate_tokens(fu["question"]),
                )
            )
            output_limiter.wait_for(MODEL_CALL_OUTPUT_RESERVATION)
            request_limiter.wait_for(1)
            resp = send_message(proc, fu["question"], retries=1)
            followup_responses.append(resp)
            print(f"    Q{i + 1}: {resp[:120]}...", file=sys.stderr)
        close_pipe(proc)

        # Inspect memory state after follow-ups
        memory_after_followup = inspect_memory_files(home)

        # Phase 3: Score follow-ups
        print("\n  Phase 3: Scoring...", file=sys.stderr)
        client = Anthropic(max_retries=5)
        followup_results = []
        for i, (fu, resp) in enumerate(zip(followups, followup_responses)):
            # Regex-based signal check (deterministic)
            regex_found, regex_total = check_signals(
                resp, fu["expected_signals"]
            )
            regex_score = regex_found / regex_total if regex_total > 0 else 0.0

            # LLM judge score
            if i > 0:
                time.sleep(JUDGE_DELAY)
            llm_score = llm_judge_signals(
                client,
                resp,
                fu["expected_signals"],
                input_limiter,
                output_limiter,
                request_limiter,
            )

            # Combined score: average of regex and LLM
            combined = (regex_score + llm_score) / 2.0

            followup_results.append({
                "question": fu["question"],
                "expected_signals": fu["expected_signals"],
                "response": resp,
                "regex_score": round(regex_score, 3),
                "regex_found": regex_found,
                "regex_total": regex_total,
                "llm_score": round(llm_score, 3),
                "combined_score": round(combined, 3),
            })
            print(
                f"    Q{i + 1}: regex={regex_score:.2f} "
                f"({regex_found}/{regex_total}) "
                f"llm={llm_score:.2f} combined={combined:.2f}",
                file=sys.stderr,
            )

        return {
            "scenario": scenario["name"],
            "description": scenario["description"],
            "seed_count": len(seed),
            "followup_count": len(followups),
            "max_history": config_overrides.get("max_history_messages", 20),
            "memory_entries_after_seed": memory_after_seed["memory_entries"],
            "memory_entries_after_followup": memory_after_followup[
                "memory_entries"
            ],
            "followups": followup_results,
            "mean_combined": round(
                sum(r["combined_score"] for r in followup_results)
                / len(followup_results),
                3,
            ),
            "mean_regex": round(
                sum(r["regex_score"] for r in followup_results)
                / len(followup_results),
                3,
            ),
            "mean_llm": round(
                sum(r["llm_score"] for r in followup_results)
                / len(followup_results),
                3,
            ),
        }
    finally:
        shutil.rmtree(home, ignore_errors=True)


# ── Reporting ────────────────────────────────────────────────────────────


def print_scenario_report(result, baseline_scenario=None):
    """Print a human-readable report for one scenario."""
    w = 80
    print(f"\n  {result['description']}")
    print(f"  {'─' * w}")
    print(
        f"  Seeds: {result['seed_count']}  "
        f"Max history: {result['max_history']}  "
        f"Memory entries: {result['memory_entries_after_seed']} → "
        f"{result['memory_entries_after_followup']}"
    )
    print(f"  {'─' * w}")

    has_baseline = baseline_scenario is not None
    if has_baseline:
        print(
            f"   {'#':<3} {'Combined':>8} {'Base':>6} {'Delta':>6} "
            f"{'Regex':>6} {'LLM':>5}  Question"
        )
    else:
        print(
            f"   {'#':<3} {'Combined':>8} {'Regex':>6} {'LLM':>5}  Question"
        )
    print(f"  {'─' * w}")

    base_scores = {}
    if has_baseline:
        for bf in baseline_scenario.get("followups", []):
            base_scores[bf["question"]] = bf["combined_score"]

    for i, fu in enumerate(result["followups"]):
        tag = "+" if fu["combined_score"] >= 0.5 else "-"
        q = fu["question"][:42]
        if has_baseline:
            bs = base_scores.get(fu["question"])
            if bs is not None:
                delta = fu["combined_score"] - bs
                sign = "+" if delta >= 0 else ""
                print(
                    f"   {i + 1:<3} {fu['combined_score']:>8.2f} "
                    f"{bs:>6.2f} {sign}{delta:>5.2f} "
                    f"{fu['regex_score']:>6.2f} {fu['llm_score']:>5.2f}  "
                    f"{tag} {q}"
                )
            else:
                print(
                    f"   {i + 1:<3} {fu['combined_score']:>8.2f} "
                    f"{'—':>6} {'—':>6} "
                    f"{fu['regex_score']:>6.2f} {fu['llm_score']:>5.2f}  "
                    f"{tag} {q}"
                )
        else:
            print(
                f"   {i + 1:<3} {fu['combined_score']:>8.2f} "
                f"{fu['regex_score']:>6.2f} {fu['llm_score']:>5.2f}  "
                f"{tag} {q}"
            )

    print(f"  {'─' * w}")
    print(
        f"   Mean: combined={result['mean_combined']:.2f}  "
        f"regex={result['mean_regex']:.2f}  "
        f"llm={result['mean_llm']:.2f}"
    )


def print_summary(scenario_results, backend, baseline=None):
    """Print overall summary."""
    overall = sum(r["mean_combined"] for r in scenario_results) / len(
        scenario_results
    )
    passed = overall >= 0.5

    print(f"\n{'=' * 60}")
    print(f"  Memory Redesign Benchmark — {backend} backend")
    print(f"{'=' * 60}")
    for r in scenario_results:
        name = r["scenario"]
        print(f"  {name:.<45} {r['mean_combined']:.2f}")
    print(f"  {'─' * 56}")

    if baseline:
        base_overall = baseline.get("overall_mean", 0)
        delta = overall - base_overall
        sign = "+" if delta >= 0 else ""
        print(
            f"  {'Overall':.<45} {overall:.2f}  "
            f"(baseline: {base_overall:.2f}, delta: {sign}{delta:.2f})"
        )
    else:
        print(f"  {'Overall':.<45} {overall:.2f}")

    print(f"\n  Result:  {'PASS' if passed else 'FAIL'} (threshold: 0.50)")
    print(f"  Model:   {MODEL}")
    print(f"  Judge:   {JUDGE_MODEL}")
    print(f"  Backend: {backend}")
    print(f"{'=' * 60}")
    print()

    return overall, passed


def main():
    scenario_names = [s["name"] for s in SCENARIOS]

    binary = None
    backend = "sqlite"
    output_file = None
    compare_file = None
    selected_scenarios = None  # None = all

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
        elif args[i] == "--scenario" and i + 1 < len(args):
            name = args[i + 1]
            if name not in scenario_names:
                print(
                    f"Unknown scenario: {name}\n"
                    f"Available: {', '.join(scenario_names)}",
                    file=sys.stderr,
                )
                sys.exit(1)
            if selected_scenarios is None:
                selected_scenarios = []
            selected_scenarios.append(name)
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
            f"[--backend sqlite|json] [--output FILE] [--compare FILE] "
            f"[--scenario NAME]\n"
            f"Available scenarios: {', '.join(scenario_names)}",
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

    # Load baseline
    baseline = None
    if compare_file:
        with open(compare_file) as f:
            baseline = json.load(f)
        print(f"Comparing against baseline: {compare_file}", file=sys.stderr)

    print("Running memory redesign benchmark...", file=sys.stderr)
    print(f"  Backend: {backend}, Model: {MODEL}", file=sys.stderr)

    # Shared rate limiters
    limiters = (
        TokenRateLimiter(INPUT_TOKEN_BUDGET_PER_MINUTE),
        TokenRateLimiter(OUTPUT_TOKEN_BUDGET_PER_MINUTE),
        TokenRateLimiter(REQUEST_BUDGET_PER_MINUTE),
    )

    # Run scenarios
    run_scenarios = SCENARIOS
    if selected_scenarios:
        run_scenarios = [s for s in SCENARIOS if s["name"] in selected_scenarios]

    scenario_results = []
    for scenario in run_scenarios:
        print(f"\n{'─' * 50}", file=sys.stderr)
        print(f"  Scenario: {scenario['description']}", file=sys.stderr)
        print(f"{'─' * 50}", file=sys.stderr)

        result = run_scenario(binary, backend, scenario, limiters)
        scenario_results.append(result)

        # Print per-scenario report
        base_scenario = None
        if baseline:
            for bs in baseline.get("scenarios", []):
                if bs["scenario"] == scenario["name"]:
                    base_scenario = bs
                    break
        print_scenario_report(result, base_scenario)

    # Overall summary
    overall, passed = print_summary(scenario_results, backend, baseline)

    # Save results
    if output_file:
        output = {
            "backend": backend,
            "model": MODEL,
            "judge_model": JUDGE_MODEL,
            "overall_mean": round(overall, 3),
            "passed": passed,
            "scenarios": scenario_results,
        }
        with open(output_file, "w") as f:
            json.dump(output, f, indent=2)
        print(f"Results saved to {output_file}", file=sys.stderr)

    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
