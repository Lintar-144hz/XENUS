---
name: xenus-testing
description: Test and verify XENUS changes without unnecessary build overhead.
---

# XENUS Testing

## Workflow
1. Identify affected components.
2. Inspect available tests.
3. Select targeted tests.
4. Build affected targets.
5. Run relevant tests.
6. Report results accurately.

## Rules
- Never claim tests passed without execution.
- Never hide existing failures.
- Distinguish pre-existing failures from new failures.
- Avoid full test suites when targeted tests are sufficient.
- Check compiler warnings.
- Do not modify tests merely to hide failures.

## Report
Include:
- Tests executed.
- Passed checks.
- Failed checks.
- Known limitations.
