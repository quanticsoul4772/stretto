# Enforceable SDLC Harnesses for AI Coding Agents — Deep-Research Report

**Date:** 2026-07-13
**Method:** 103-agent deep-research workflow — 5 search angles, 21 sources fetched, 102 claims extracted, top 25 adversarially verified by 3-voter panels (21 confirmed, 4 refuted, 0 unverified).

## Research question

What exists NOW (mid-2026) that a solo developer can use to make a coding agent
(Claude Code / Claude Agent SDK) structurally unable to skip lifecycle steps or
act without operator approval — given the operator already built devharness
(event-sourced Python runtime; four roles with enforced tool boundaries;
fail-closed gates; verifier-first acceptance requiring both a passing declared
verifier AND a fresh-context reviewer; operator sign-off on specs and
integration)?

1. Which frameworks ENFORCE (not suggest) phase-gated development — structural
   vs prompt-level?
2. Which Claude Code / Agent SDK primitives make an agent structurally incapable
   of unauthorized action, and what are the known bypasses?
3. For someone who already has a working harness: drive work through it, adopt
   an external framework, or wrap Claude Code in SDK-level deny-by-default?

## Executive summary

As of mid-2026 there is a real structural enforcement layer for Claude Code, but
it lives in the SDK/harness primitives, not in the spec-driven frameworks:
PreToolUse hooks (which run before every other permission step and whose denies
survive bypassPermissions), deny rules (disallowed_tools), and the allowedTools
+ permissionMode:"dontAsk" deny-by-default pairing are the only mechanisms that
make an agent structurally unable to act — while Spec Kit, OpenSpec, and similar
frameworks are prompt-template pipelines whose phase gates are advisory and
bypassable. The one framework found that is genuinely hook-enforced rather than
prompt-level is the sd0x-dev-flow Claude Code plugin (9 hook scripts,
fail-closed Stop-hook gate in strict mode), though its gate-satisfaction signal
is agent-emitted text sentinels and therefore spoofable. Known structural gaps
remain: canUseTool approval callbacks are silently shadowed by any earlier
auto-approval, unsandboxed Bash defeats Edit/Write allowlists, tool restrictions
do not propagate to nested subagents, and Anthropic's own auto mode classifier
misses 17% of unauthorized actions — confirming that probabilistic and
prompt-level controls cannot be the gate. For an operator who already has a
working event-sourced role-separated harness, the evidence points clearly to
path (a): keep the harness as the source of truth and drive Claude Code sessions
through it, using PreToolUse hook denies plus scoped deny rules and dontAsk-mode
allowlists as the mechanical tool-surface enforcement layer, with the harness's
own gates deciding what those hooks permit — this matches both the official
docs' guidance (permission system for hard deny, hooks for every-call checks)
and the harness-engineering literature's prescription to iterate on one's own
harness rather than adopt a template.

## Verified findings

### 1. PreToolUse hooks are the only mechanism guaranteed to gate every tool call — HIGH confidence (votes 3-0, 3-0, 2-1)

The SDK evaluates permissions in a fixed six-step order (hooks, deny rules, ask
rules, permission mode, allow rules, canUseTool); hooks run first, and a hook
deny applies even in bypassPermissions mode. A hook blocks deterministically via
exit code 2 or a JSON permissionDecision of "deny" (with allow/ask/defer as
alternatives), and the stated reason is shown to the model.

**Evidence:** Official Anthropic docs verbatim (fetched 2026-07-13): "For checks
that must run on every tool call, use a PreToolUse hook: hooks run before every
other step, and a hook deny applies even in bypassPermissions mode"; hooks docs
confirm exit 2 "Blocks the tool call" and "Claude Code reads the JSON decision,
blocks the tool call, and shows Claude the reason." Adversarial search found
four GitHub issues alleging deny non-enforcement; all closed as not-planned or
reporter error (the strongest, #37210, was the reporter mixing exit-2 with JSON
output; deny worked once signaled correctly).

**Sources:** [agent-sdk/permissions](https://code.claude.com/docs/en/agent-sdk/permissions) ·
[hooks docs](https://code.claude.com/docs/en/hooks) ·
[claudedirectory.org permissions guide](https://www.claudedirectory.org/blog/claude-code-permissions-guide)

### 2. Deny rules survive every permission mode, including bypassPermissions — HIGH confidence (3-0, 3-0)

Scoped rules like `Bash(rm *)` are denied in all modes, and bare-name deny rules
remove the tool definition entirely so the model cannot even see or attempt the
tool. However, deny-rule matching has documented evasion routes (env-variable
prefixes bypassing Bash patterns, issue #31558; subagent gaps, #25000) — they
are pattern guards, not a sandbox.

**Evidence:** Docs verbatim: "disallowed_tools=[\"Bash\"] — The Bash tool
definition is removed from the request. Claude does not see the tool and cannot
attempt it. ... Calls matching rm * are denied in every permission mode,
including bypassPermissions." Known caveats: per-subagent
AgentDefinition.disallowedTools not enforced (SDK issue #172, open),
env-var-prefix pattern bypass (#31558).

**Sources:** [agent-sdk/permissions](https://code.claude.com/docs/en/agent-sdk/permissions) ·
[permission-modes](https://code.claude.com/docs/en/permission-modes) ·
[issue #31558](https://github.com/anthropics/claude-code/issues/31558)

### 3. The deny-by-default lockdown recipe: allowedTools + permissionMode:"dontAsk" — HIGH confidence (3-0)

Listed tools are auto-approved, everything else is hard-denied without
prompting, and canUseTool is never called. A strict lockdown must also control
settingSources and hooks, since settings.json allow rules and hook allows also
pre-approve tools.

**Evidence:** Docs verbatim: "For a locked-down agent, pair allowedTools with
permissionMode: 'dontAsk'. Listed tools are approved; anything else is denied
outright instead of prompting"; for dontAsk, "Everything else is denied without
calling canUseTool." Verified against current docs (v2.1.198/2.1.199).

**Source:** [agent-sdk/permissions](https://code.claude.com/docs/en/agent-sdk/permissions)

### 4. canUseTool callbacks are NOT a reliable operator-approval gate — HIGH confidence (3-0)

Any tool call auto-approved at an earlier step (acceptEdits, bypassPermissions,
or an allow rule) never reaches the callback, so approval logic placed there is
silently bypassed. The SDK now emits a CLAUDE_SDK_CAN_USE_TOOL_SHADOWED warning
(v2.1.198+) for exactly this hazard; PreToolUse hooks are the prescribed
alternative for every-call checks.

**Evidence:** Official Warning block verbatim: "Auto-approved tools never reach
canUseTool. A tool call approved at any earlier step ... skips your canUseTool
callback, so permission checks you put there are silently bypassed for that
tool." Exception: AskUserQuestion and MCP tools with requiresUserInteraction
still reach the callback.

**Source:** [agent-sdk/permissions](https://code.claude.com/docs/en/agent-sdk/permissions)

### 5. Stop hooks can gate turn completion — but fail OPEN — MEDIUM confidence (2-1)

Exit 2 or {"decision":"block"} prevents the agent from ending its turn, making
turn completion gateable at the harness level. But hook crashes, timeouts, and
non-0/2 exit codes are non-blocking, so a broken gate lets the agent stop. The
gate also cannot compel productive compliance (the agent can be held in-turn yet
spin), and user Escape ends the turn regardless.

**Evidence:** Docs verbatim: exit 2 on Stop "Prevents Claude from stopping,
continues the conversation"; the stop_hook_active input flag exists precisely
because an always-blocking Stop hook loops forever — evidence the block is
mechanical. The widely-cited HN "Claude ignoring stop hooks" thread dissolved on
inspection (the hook exited 0).

**Source:** [hooks docs](https://code.claude.com/docs/en/hooks)

### 6. Anthropic's own docs concede hook-level pattern filters are inadequate — HIGH confidence (3-0)

The per-handler `if` filter (permission-rule syntax like "Bash(git *)") is
best-effort and fails open when a Bash command cannot be parsed; the docs
explicitly recommend the permission system, not hooks-with-filters, for hard
allow/deny. This directly validates the finding that regex-style hook filters
are too narrow. (A filterless PreToolUse hook that denies in its own code is not
subject to this fail-open path.)

**Evidence:** Docs verbatim: "The filter also fails open, running your hook
regardless of pattern, when the Bash command can't be parsed. Because the `if`
filter is best-effort, use the permission system rather than a hook to enforce a
hard allow or deny."

**Source:** [hooks docs](https://code.claude.com/docs/en/hooks)

### 7. Claude Code auto mode is probabilistic, not structural — misses 17% — HIGH confidence (2-1, 3-0)

A fixed allowlist covers non-mutating tools, in-project Write/Edit is allowed
with NO review at all, and a transcript classifier gates everything else (shell,
web, subagent spawns, out-of-project filesystem) — including evaluating
agent-authored scripts' assembled commands before execution. The full pipeline
misses 17% of real overeager actions, with the dominant failure mode being
misjudging whether user consent covered the action.

**Evidence:** Primary Anthropic engineering post (Mar 25, 2026) verbatim: "File
writes and edits inside the project directory are allowed without a classifier
call"; "The 17% false-negative rate on real overeager actions is the honest
number"; "What it gets wrong is whether a real consent signal in the session
actually covers this action." (17% measured on a small curated set, n=52 — wide
error bars.)

**Source:** [anthropic.com/engineering/claude-code-auto-mode](https://www.anthropic.com/engineering/claude-code-auto-mode)

### 8. sd0x-dev-flow is the only surveyed framework that is genuinely structural — HIGH confidence (3-0, 3-0, 2-1)

A Claude Code plugin (v3.0.12, May 2026, ~175 stars) enforcing gates via 9 hook
scripts across 5 events (PreToolUse, PostToolUse, Stop, SessionStart,
UserPromptSubmit), a sentinel-driven state machine parsed into durable state, a
stop-guard that exits 2 to block incomplete reviews in strict mode
(fail-closed), and dual independent review by default (Codex MCP + secondary
reviewer, single-reviewer fallback). Caveats: strict mode is opt-in (default is
warn), gate satisfaction is parsed from agent-emitted text sentinels and thus
spoofable in principle, and review dispatch is a hook+behavior hybrid.

**Evidence:** Verified against source code, not just README: hooks/stop-guard.sh
exits 2 with JSON when review steps or gate sentinels are missing/failing;
hooks/post-tool-review-state.sh parses "✅ Ready / ⛔ Blocked / ✅ All Pass"
markers into durable state. Repo tagline: "Quality gates that AI can't skip."

**Source:** [github.com/sd0xdev/sd0x-dev-flow](https://github.com/sd0xdev/sd0x-dev-flow)

### 9. Mainstream spec-driven frameworks are prompt-level, not structural — MEDIUM confidence (2-1, 3-0)

GitHub Spec Kit's phase gates are checklist items in markdown prompt templates —
the agent can proceed by documenting "justified exceptions," and community
Ralph-loop extensions drive tasks.md end-to-end with zero human intervention,
demonstrating the gates are bypassable (Ralph Loop is a community extension, not
a built-in mode). OpenSpec deliberately abandoned its legacy enforced phase
gates: OPSX is "fluid not rigid," artifacts update anytime, /opsx:apply works
through all tasks with minimal gates, and there is no adversarial review or
approval mechanism beyond an advisory tip.

**Evidence:** Verifiers escalated past the blog to primary docs: Spec Kit's
spec-driven.md says templates "act as sophisticated prompts that constrain the
LLM's output" with no hooks or blocking tooling between phases; OpenSpec's own
docs say "no phase gates, work on what makes sense" and that the legacy gated
workflow was explicitly abandoned. Claims covering Kiro, BMAD, and Gangsta
Agents were refuted or did not survive verification — their enforcement status
is unestablished, not confirmed-advisory.

**Sources:** [HackerNoon spec-first showdown](https://hackernoon.com/the-spec-first-development-showdown-spec-kit-openspec-bmad-and-gangsta-agents-compared) ·
[github/spec-kit (spec-driven.md)](https://github.com/github/spec-kit) ·
[Fission-AI/OpenSpec (docs/opsx.md, docs/concepts.md)](https://github.com/Fission-AI/OpenSpec)

### 10. Two known bypass surfaces defeat naive allowlist lockdowns — HIGH confidence (3-0, 3-0)

(1) **Unsandboxed Bash:** excluding Edit/Write while retaining Bash for
diagnostics leaves every Bash-side mutation (rm, git commit, curl -X POST)
prevented only by prompt rules — real deployed plugins (cookys-autopilot)
self-disclose exactly this gap; mechanical closure requires sandboxed Bash,
Bash(cmd:*) deny patterns, or PreToolUse hooks.
(2) **Subagent non-propagation:** a child's toolset comes from the child's own
agent type, not the parent's allowlist — a read-only planner dispatching a
general-purpose child gives that child Edit/Write; Agent(type) allowlist syntax
is ignored inside subagent definitions, and AgentDefinition.tools/
disallowedTools are not enforced for child processes (open SDK bug #172).
Read-only guarantees across the child hop are convention-enforced unless blocked
via settings-level permissions.deny entries like Agent(general-purpose) or
PreToolUse hooks.

**Critically:** the companion claim that subagent `tools:` frontmatter IS
structurally enforced was REFUTED (1-2) — mechanical role separation must come
from session-level deny rules and hooks, not subagent definitions.

**Sources:** [cookys-autopilot README](https://www.claudepluginhub.com/agents/cookys-autopilot/agents/readme) ·
[sub-agents docs](https://code.claude.com/docs/en/sub-agents) ·
[claude-agent-sdk-typescript#172](https://github.com/anthropics/claude-agent-sdk-typescript/issues/172)

### 11. Harness-engineering literature corroborates: iterate your OWN harness — MEDIUM confidence (3-0, 3-0, 2-1)

Böckeler (martinfowler.com, Apr 2026): the taxonomy explicitly rates
computational controls as deterministic/reliable and inferential (AI-based)
controls as non-deterministic — prompt-level "feedforward" guides (AGENTS.md,
Skills, bootstrap scripts) are probabilistic steering with zero mentions of
permissions, denies, sandboxes, or approval gates — and it prescribes continuous
iteration on one's own harness as the human's primary job, converting every
repeated process break into an improved control. It is skeptical of shareable
harness templates ("versioning and contribution problems ... with
non-deterministic guides and sensors").

**Evidence:** Verbatim: "Computational - deterministic and fast ... results are
reliable. Inferential - Semantic analysis, AI code review ... results are more
non-deterministic"; "The human's job in this is to steer the agent by iterating
on the harness. Whenever an issue happens multiple times, the feedforward and
feedback controls should be improved." Full-text sweep found zero occurrences of
sandbox/permission/deny/approval — the path-(a) reading is article-consistent
inference, hence medium.

**Source:** [martinfowler.com/articles/harness-engineering.html](https://martinfowler.com/articles/harness-engineering.html)

### 12. Answer to Q3: path (a) — devharness as the authority, thin SDK shim underneath — MEDIUM confidence (synthesis)

Keep the event-sourced harness as the authority and put Claude Code sessions
under it via a thin SDK-level enforcement shim. The concrete integration pattern
the evidence supports:

- run sessions with **allowedTools + permissionMode:"dontAsk"** (deny-by-default,
  no prompting);
- **scoped disallowed_tools** for hard blocks that survive any mode;
- a **filterless PreToolUse hook that calls into the harness's gate layer** to
  decide every consequential call — the only every-call, bypass-surviving,
  programmable checkpoint;
- a **Stop hook consulting harness phase state** to prevent premature turn
  completion;
- treat canUseTool, hook `if` filters, subagent frontmatter restrictions, and
  unsandboxed Bash as **known-unsound**; close the Bash channel with sandboxing
  or command-scoped denies.

Adopting an external framework buys little: the only structural one
(sd0x-dev-flow) duplicates a subset of what devharness already does, with a
weaker (text-sentinel) state channel.

**Caveat:** no surviving claim directly evaluated end-to-end deployments of
external-harness-gated interactive Claude Code sessions — the recommendation is
inference from mechanism evidence rather than observed outcomes, hence medium
despite high-confidence components.

## Refuted claims (killed in verification)

1. **"Subagent `tools:` frontmatter is structurally enforced"** — REFUTED 1-2.
   Load-bearing for anyone replicating role separation via subagents: treat
   subagent-level restrictions as unreliable until SDK issue #172 is fixed.
2. **"Gangsta Agents offers the strictest gating (manual operator approval at
   every phase)"** — REFUTED 0-3. Its enforcement is pure Markdown/shell
   prompts, and it ships an autonomous mode that runs end-to-end with no
   approval.
3. **"spec-compare's three architectural classes imply structural enforcement
   for spec-anchored/spec-as-source tools"** — REFUTED 0-3.
4. **"Most SDD tools break down on small iterative modifications"** — REFUTED
   1-2 (as stated; not established either way).

## Caveats

- All Claude Code / Agent SDK claims verified against docs current as of
  2026-07-13 (v2.1.198–2.1.205 behavior); the permission stack has changed
  materially in recent versions — specifics may drift.
- **Kiro, BMAD, Superpowers, TrueFoundry produced no surviving claims** — their
  absence means unverified, not advisory-by-confirmation.
- Auto mode's 17% false-negative rate comes from a small curated set (n=52).
- Residual soft spots in "structural" guarantees: Stop hooks fail open on hook
  crash/timeout; deny-rule pattern matching has reported evasions (#31558,
  #25000); sd0x-dev-flow's gate state derives from spoofable agent-emitted text
  sentinels.
- The path-(a) recommendation is mechanism-level inference; no real-world
  outcome data on driving interactive Claude Code sessions through an external
  event-sourced harness.

## Open questions

1. What non-text, tamper-proof channel should carry gate-satisfaction state
   between the agent and the harness? sd0x-dev-flow parses agent-emitted
   sentinels (spoofable); no surviving source documented hooks verifying
   harness-owned state (event-log assertions, verifier exit codes) rather than
   model output. **devharness's hash-chained event log is exactly this
   channel.**
2. When will per-subagent tool restrictions become mechanically enforced
   (claude-agent-sdk-typescript#172 open, no maintainer response)? Until then,
   all role boundaries must live in session-level deny rules and hooks.
3. How escape-resistant is Claude Code's sandboxed Bash in practice? (Ona's
   "How Claude Code escapes its own denylist and sandbox" documents real
   escapes — e.g. /proc/self/root path evasion.) Is it sufficient to close the
   Bash-mutation channel, or is OS-level isolation (worktree + container) per
   role still needed?
4. What do Kiro, BMAD, Superpowers, and TrueFoundry actually enforce
   structurally as of mid-2026? No claims survived verification — survey
   incomplete.

## All sources (21)

**Primary:**
- https://code.claude.com/docs/en/agent-sdk/permissions
- https://code.claude.com/docs/en/hooks
- https://www.anthropic.com/engineering/claude-code-auto-mode
- https://github.com/sd0xdev/sd0x-dev-flow
- https://www.claudepluginhub.com/agents/cookys-autopilot/agents/readme

**Forum / issue tracker:**
- https://github.com/anthropics/claude-code/issues/29709 (PreToolUse:Edit bypassed via Bash)
- https://github.com/anthropics/claude-code/issues/11226
- https://github.com/anthropics/claude-code/issues/54898
- https://github.com/anthropics/claude-code/issues/31558 (env-prefix deny bypass)
- https://github.com/anthropics/claude-agent-sdk-typescript/issues/172 (subagent restrictions unenforced)

**Blog / secondary:**
- https://martinfowler.com/articles/harness-engineering.html
- https://hackernoon.com/the-spec-first-development-showdown-spec-kit-openspec-bmad-and-gangsta-agents-compared
- https://github.com/cameronsjo/spec-compare
- https://www.claudedirectory.org/blog/claude-code-permissions-guide
- https://hidekazu-konishi.com/entry/claude_code_hooks_complete_guide.html
- https://dev.to/bfxavier/a-pretooluse-hook-that-sandboxes-claude-code-agents-by-reading-what-they-actually-do-1bpj
- https://pasqualepillitteri.it/en/news/1832/claude-code-dangerously-skip-permissions-pretooluse-hooks-2026
- https://blog.boucle.sh/posts/what-claude-code-hooks-can-and-cannot-enforce/
- https://ona.com/stories/how-claude-code-escapes-its-own-denylist-and-sandbox
- https://adamkinney.com/aatt/claude-code/deny-rules-dont-protect-you-sandbox-does/
- https://www.claudecodecamp.com/p/claude-code-sandboxing-how-sandbox-works-and-what-it-doesn-t-protect
- https://siddhantkhare.com/writing/claude-code-permission-model-is-broken

## Run stats

103 agent calls · 5 angles · 21 sources fetched · 102 claims extracted · 25
verified (21 confirmed, 4 killed) · 12 findings after synthesis.
