---
name: xenus-architecture
description: Understand and preserve XENUS architecture, modularity and platform independence.
---

# XENUS Architecture

## Core Principles
- C11 and POSIX-oriented design.
- Lightweight execution.
- Modular architecture.
- Minimal external dependencies.
- Hardware capability awareness.

## Architecture Rules
- Inspect existing interfaces before changing them.
- Keep core logic independent from presentation.
- Do not introduce unnecessary services.
- Unsupported hardware must remain explicitly unavailable.
- Preserve existing public APIs unless approved.

## Platform Flexibility
XENUS components should avoid unnecessary
platform-specific dependencies.

Presentation backends may differ between platforms
without forcing changes to core logic.
