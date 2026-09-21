# AI Assistant adversarial design review

## Verdict

**Do not merge this implementation as a production feature.** It is a useful
proof of concept for editing small text files, but it does not provide the
editor-native agent required to generate assets, build worlds, author
animations, and implement game systems safely and reliably.

The two providers also have materially different capabilities. The Responses
API provider can only list, read, and replace UTF-8 files. The Codex provider
runs an external process with workspace write access. Presenting both behind
one Send button hides important differences in behavior, permissions,
observability, and recoverability.

## Blocking findings

### 1. There is no review or transaction boundary

`write_file` replaces a project file immediately. There is no proposed diff,
per-file approval, atomic multi-file commit, backup, editor undo integration,
or rollback when a later tool call fails. A malformed response can therefore
leave scenes and resources partially rewritten.

Before shipping, changes should be staged in memory or in a temporary
workspace. The dock should show a diff and require explicit approval before
applying a transaction. Applying should use atomic replacement and expose a
single undo/rollback operation.

### 2. The advertised creative workflows are not implemented

The API tool surface only manipulates text. It cannot generate or decode image,
audio, mesh, or other binary assets. It also cannot call Godot's scene,
resource, animation, navigation, terrain, importer, or validation APIs.
Writing `.tscn` or `.tres` text by hand is not equivalent to editor-native
world or animation authoring and is fragile across resource formats.

The external Codex process may invoke arbitrary tools that happen to be
installed, but that is an undeclared environmental dependency rather than a
supported asset pipeline. Results will vary by machine and are not described
by the provider UI.

### 3. Codex execution cannot be cancelled

The Stop button is disabled for Codex. Closing the editor waits for the worker
thread, so a stuck or long-running child process can prevent shutdown. The
implementation must retain the child process ID, stream output without
blocking, support termination, and impose configurable time and output limits.

### 4. The path boundary is lexical, not canonical

Rejecting `..` protects against direct traversal, but it does not prove that a
path resolves inside the project after following symlinks or platform-specific
path aliases. Reads, writes, and directory creation must resolve the canonical
target and verify that it remains under the canonical project root. Sensitive
project metadata and credential-like files also need an explicit policy.

### 5. Untrusted output is applied without validation

Neither provider validates generated GDScript, scenes, resources, shaders, or
project settings before considering the task successful. At minimum, the
transaction should parse all Godot text resources, run script/static checks,
load generated resources in an isolated context, and report diagnostics before
the user can apply it. Game-system changes additionally need project-defined
tests and a run/debug feedback loop.

### 6. The Responses API loop is brittle

The response parser assumes that every output and content item is a dictionary
and silently treats malformed function arguments as an empty dictionary. It
does not handle incomplete, cancelled, or refused responses, rate limits with
retry guidance, continuation after the twelve-round cap, streaming, or context
compaction. A provider adapter needs typed events and explicit terminal states
instead of exposing wire-format assumptions to the dock.

### 7. Authentication and process discovery are incomplete

GUI applications frequently inherit a different `PATH` from interactive
shells. Hard-coding `codex` provides no discovery, configured executable path,
version/capability check, authentication status check, or login flow. The dock
also embeds a raw API key field without a platform credential-store
integration. Provider setup must be a separate, testable settings surface.

### 8. There is no durable session or audit trail

Conversation state is discarded between prompts and editor restarts. Only
successful API writes are shown in the transcript; reads and external-process
changes are not itemized. A production agent needs persisted task history,
tool calls, permissions, changed-file manifests, model/provider metadata, and
validation results, with secrets redacted.

## Capability assessment

| Requested workflow | Current support | What is missing |
| --- | --- | --- |
| Build assets from prompts | **Not supported** by the API provider; incidental with Codex | Image/audio/3D generation adapters, binary artifact transport, licenses/provenance, import configuration, previews, and approval |
| Create worlds | **Very limited** text-file generation | Scene-tree tools, terrain/navigation APIs, resource references, incremental editing, viewport preview, and scene validation |
| Create animations | **Very limited** text-file generation | AnimationPlayer/AnimationLibrary tools, track/key operations, skeleton/retargeting integration, timeline preview, and validation |
| Create mechanics and systems | **Partial** | Project context selection, structured code edits, language diagnostics, tests, run/debug feedback, diff approval, and rollback |

## Required architecture

1. **Provider layer** — a stable interface for authentication, model
   capabilities, streaming events, cancellation, and errors. OpenAI API and
   Codex are separate implementations rather than conditionals in the dock.
2. **Permission broker** — typed capabilities such as read project, propose
   text edits, generate binary assets, modify scenes, run commands, and run the
   game. Permissions are shown and approved independently.
3. **Editor-native tool registry** — typed tools for scenes, nodes, resources,
   animations, scripts, imports, project settings, tests, and game execution.
   Tools validate arguments and return structured diagnostics.
4. **Staging transaction** — all mutations produce a previewable change set.
   The user can accept individual changes, apply atomically, or discard them.
5. **Artifact pipeline** — generated binary data is written to staging,
   accompanied by provider and prompt provenance, scanned for size/type, then
   imported and previewed before approval.
6. **Validation loop** — parse resources, check scripts, import assets, run
   project-defined checks, and optionally launch a controlled playtest. Feed
   diagnostics back to the model without automatically accepting fixes.
7. **Task history** — persist prompts, tool events, diffs, approvals,
   validation output, and rollback points while redacting credentials.

## Recommended delivery sequence

1. Replace direct writes with staged text diffs, approval, atomic apply, and
   rollback.
2. Extract provider adapters and add cancellation, streaming, setup checks, and
   bounded execution.
3. Add structured script and scene tools plus validation; use these to support
   mechanics and small world edits.
4. Add animation tools and timeline preview.
5. Add explicit multimodal asset-generation providers and a provenance-aware
   import pipeline.
6. Only then describe the feature as supporting full asset, world, animation,
   and game-system creation.
