# XENUS - AI Engineering Instructions

## Project Identity
XENUS is a lightweight, modular desktop environment
designed for efficiency and hardware flexibility.

## Engineering Rules
- Use C11 and POSIX interfaces.
- Preserve the existing architecture.
- Prioritize low RAM and CPU overhead.
- Avoid unnecessary dependencies.
- Keep modules independent and maintainable.
- Do not introduce background threads or daemons
  without explicit architectural approval.
- Do not rewrite existing systems without approval.
- Do not claim unsupported hardware capabilities.
- Preserve existing functionality.

## Development Rules
- Inspect existing implementations before modifying them.
- Follow established naming and coding conventions.
- Keep changes focused and minimal.
- Check compiler warnings.
- Run relevant tests after modifications.
- Never hide failing tests.

## Project Decisions
- OpenCode: Engineering and implementation.
- ChatGPT: Architecture review and technical advisor.
- Project owner: Final decision authority.

## Restrictions
- No commits or pushes without authorization.
- No destructive changes without authorization.
- Do not modify unrelated files.
