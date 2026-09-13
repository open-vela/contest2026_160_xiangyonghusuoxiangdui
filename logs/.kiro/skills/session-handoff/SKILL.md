---
name: session-handoff
description: >-
  Use when a session is long-running and approaching the agent's context limit,
  or when the user asks to "wrap up", "hand off", "save progress", "写个交接",
  "总结一下继续", "上下文快满了", or start a fresh window and continue. Produces a
  single self-contained handoff document saved into the workspace that captures
  the session's outcome, current state, source-tree/file locations, key
  technical facts, known-good build/run commands, and the next TODO — so a brand
  new agent window can read only that one file and resume the work without
  re-discovering anything.
---

# Session Handoff

When a session gets long and context is about to be compacted or lost, distill
everything that matters into **one handoff file in the workspace**. The goal:
**a new window reads that single file and picks up exactly where we left off** —
no re-exploration, no lost decisions, no forgotten file paths.

## When to use

Activate this skill when any of these are true:

- The conversation is long and context is nearing its limit (proactively offer /
  write a handoff before important state is lost).
- The user says things like: wrap up, hand off, save progress, 交接, 总结继续,
  上下文快满了, 新窗口接着干, continue in a new chat.
- A milestone is reached in a multi-session effort and you want a durable
  checkpoint.
- You are about to run a risky/long operation and want state captured first.

Prefer to write the handoff **before** you are forced to compact, not after.

## Where the handoff lives

Save one handoff per work stream:

```
.kiro/handoffs/HANDOFF-<topic>.md
```

Use a short kebab-case `<topic>` (e.g. `HANDOFF-rk3588-rpmsg.md`). If a handoff
for this work already exists, **update it in place** (overwrite stale sections,
keep it a single current snapshot) rather than creating a second file. Note the
date/time at the top so the reader knows how fresh it is.

If the repo already has a domain-specific summary convention (e.g. an existing
`.kiro/board-changes/HANDOFF-*.md`), keep using that location/format instead of
creating a parallel one — one source of truth per work stream.

## What a good handoff must contain

Write it for a competent agent who knows nothing about this session. It must be
**self-contained** (all absolute paths, branches, exact commands) and **honest**
about what is verified vs. unverified. Cover, in this order:

1. **One-line status** — where things stand right now, in one sentence.
2. **Source-tree / file locations** — every repo, its absolute path, git branch,
   and one-line purpose. Include toolchain paths, key config files, log files.
   A reader must be able to `cd` to the right place immediately.
3. **Completed milestones** — what is done and *verified* (say how it was
   verified: built, tested, ran on hardware, etc.).
4. **Current work in progress** — the exact thing being worked on, including
   code that is *written but not yet compiled/tested* (list those files
   explicitly so nothing is assumed done).
5. **Key technical facts / decisions** — hard-won findings, gotchas, magic
   numbers, addresses, root causes of bugs already solved. This is what saves
   the next window hours.
6. **Known-good commands** — build / run / test / flash / deploy commands, copy-
   pasteable, with the exact parameters and working directory.
7. **Next steps / TODO** — an ordered, concrete list of what to do next, plus
   known risks or uncertainties.
8. **Relevant commits / references** — recent commit hashes per repo, links to
   the detailed changelog if one exists.

### Handoff template

```markdown
# 交接总结 — <topic>

> 新窗口读这一份即可接上。最后更新：<YYYY-MM-DD HH:MM>
> <任何全局约定，如：全程中文 / 一次只跑一条命令 / 路径都在工作区外用绝对路径>

## 0. 一句话现状
<one sentence: what works, what's in progress right now>

## 1. 源码树 / 文件位置
| 路径 | 说明 | 分支 |
|------|------|------|
| /abs/path/repo | ... | ... |
- 工具链 / 关键配置 / 日志文件：<abs paths>

## 2. 已完成里程碑（已验证）
- <item> ✅（验证方式：built / tested / 真机）

## 3. 当前进行中
- <exact current task>
- 已写但未编译/验证的文件：<explicit list>

## 4. 关键技术要点 / 决策
- <gotchas, addresses, root causes, magic numbers, why-decisions>

## 5. 已知可用命令（known-good）
​```bash
# build / run / test / flash — copy-pasteable, with cwd
​```

## 6. 下一步 TODO
1. <concrete next action>
- 风险 / 不确定：<...>

## 7. 关键提交 / 参考
- <repo>: <hash> <what>
- 详细历史见：<changelog path>
```

## Process

1. **Scan the session** for: files created/edited, commands that worked, key
   decisions, bugs fixed and their root causes, and anything still open.
2. **Verify locations** cheaply if unsure — confirm a path/branch with a quick
   read or `git status`/`git branch` rather than trusting memory. Do not
   fabricate paths, hashes, or "done" claims.
3. **Write / update** `.kiro/handoffs/HANDOFF-<topic>.md` (or the existing
   domain location) using the template. Keep it a single current snapshot.
4. **Separate verified from unverified** explicitly — unbuilt code, untested
   assumptions, and open questions must be flagged, not hidden.
5. **Tell the user** the handoff path and give them a one-line instruction to
   use it in a new window (e.g. "open a new chat and say: read
   `.kiro/handoffs/HANDOFF-<topic>.md` and continue").

## Rules

- The handoff must be **self-contained**: a new window with zero prior context
  should be able to resume from it alone.
- Use **absolute paths**, exact branch names, and copy-pasteable commands.
- Be **honest**: never mark something done/verified unless it actually is; call
  out written-but-untested code and unknowns.
- Keep **one file per work stream** and update it in place; don't scatter
  multiple stale summaries.
- Don't dump raw transcript — distill to decisions, state, and what's needed to
  continue.
- Writing a handoff is documentation only; **do not** commit, push, or run
  destructive commands as part of it unless the user asks.
