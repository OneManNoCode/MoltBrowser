# Contributing to MoltBrowser

Thank you for your interest in contributing to MoltBrowser!

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Set up the development environment (see Development Setup below)
4. Create a feature branch from `main`
5. Make your changes, with signed-off commits (see below)
6. Submit a pull request

## Development Setup

MoltBrowser is built on one pinned Chromium revision, recorded in
`chromium_version.txt`. `scripts/setup.sh` checks out exactly that revision
(never tip-of-tree), syncs its dependencies and copies the MoltBrowser
sources in `src/` over it. Plan for ~100 GB of disk, 16 GB+ of RAM and
several hours for the first build.

```bash
git clone https://github.com/YOUR_USERNAME/MoltBrowser.git
cd MoltBrowser
./scripts/setup.sh                 # --target-os mac,linux for a shared checkout
./scripts/configure.sh             # auto-detects the host; or e.g. --platform linux --arch x64
./scripts/build.sh
```

On Linux you can also build in Docker: `./build/docker/build-linux-fresh.sh`.

Edit files in `src/`, never directly in `chromium/src/`, and copy them over
(`cp -R src/. chromium/src/`) before building. If `chromium_version.txt`
changes, run `./scripts/setup.sh --resync` to move your checkout to the new
pin. If you change a file that also exists in upstream Chromium, regenerate
and check the patch series:

```bash
tools/chromium/overlay_to_patches.py && tools/chromium/verify_patches.py
```

Moving MoltBrowser to a newer Chromium is described in
[docs/REBASING.md](docs/REBASING.md).

## Code Style

- C++ code follows the Chromium coding style
- Use `clang-format` for formatting
- All new code must include appropriate comments
- AI module code lives in `chrome/browser/molt_ai/`

## Pull Request Process

1. Ensure your code compiles without warnings
2. Add tests for new functionality
3. Update documentation if needed
4. Describe your changes clearly in the PR description
5. Reference any related issues

## Sign your commits (DCO)

We ask contributors to sign off their commits under the
[Developer Certificate of Origin](https://developercertificate.org/) (DCO):
by adding a `Signed-off-by: Your Name <you@example.com>` line you certify
that you wrote the change, or otherwise have the right to submit it under
the project's license. `git commit -s` adds the line for you.

## Architecture Guidelines

- AI modules are isolated in `chrome/browser/molt_ai/`
- All AI operations must go through the `BrowserAIRuntime` class
- Agent, chat-action and automation page actions must go through
  `molt_ai/security/agent_safety.*` (and, for workflows, the trust policy in
  `molt_ai/automation/automation_security.*`)
- Privacy-sensitive operations require MoltShield integration
- Local LLM inference uses the llama.cpp runtime exclusively

## Reporting Issues

- Use GitHub Issues for bug reports and feature requests
- Include system information (OS, hardware, model being used)
- For security vulnerabilities, follow [SECURITY.md](SECURITY.md) (GitHub
  private reporting or security@geneye.ai) - never a public issue

## Code of Conduct

Everyone taking part is expected to follow our
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

By contributing, you agree that your contributions will be licensed under GPLv3.
