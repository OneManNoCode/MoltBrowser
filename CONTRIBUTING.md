# Contributing to MoltBrowser

Thank you for your interest in contributing to MoltBrowser!

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Set up the development environment (see README.md)
4. Create a feature branch from `main`
5. Make your changes
6. Submit a pull request

## Development Setup

```bash
git clone https://github.com/YOUR_USERNAME/MoltBrowser.git
cd MoltBrowser
./scripts/setup.sh
./scripts/configure.sh
./scripts/build.sh
```

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

## Architecture Guidelines

- AI modules are isolated in `chrome/browser/molt_ai/`
- All AI operations must go through the `BrowserAIRuntime` class
- Agent actions must pass through the `ActionValidator` security layer
- Privacy-sensitive operations require MoltShield integration
- Local LLM inference uses the llama.cpp runtime exclusively

## Reporting Issues

- Use GitHub Issues for bug reports and feature requests
- Include system information (OS, hardware, model being used)
- For security vulnerabilities, please email security@geneye.ai

## License

By contributing, you agree that your contributions will be licensed under GPLv3.
