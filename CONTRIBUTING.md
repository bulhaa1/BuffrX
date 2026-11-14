# Contributing to BuffrX

Make BuffrX better in the ways you care about: fix bugs, add features, improve the
interface, experiment, or help bring it to more platforms. Documentation, tests,
and testing on different hardware are welcome too.

You can start without asking permission. For larger changes, sharing your idea
early can help avoid duplicated work and make review easier. Keep each PR clear
about its goal; draft PRs and work in progress are welcome. A PR is a place to
collaborate, though not every proposal will be merged.

## Get started

1. Check [open issues](https://github.com/bulhaa1/BuffrX/issues) and
   [PRs](https://github.com/bulhaa1/BuffrX/pulls) for existing work.
2. Fork the repo, clone your fork, and create a branch for your change.
3. Choose the setup you need in the [development guide](DEVELOPMENT.md):
   - **Docs:** edit Markdown. No app build required.
   - **Editor:** Node.js 24, then the editor checks. Works on Windows or Linux.
   - **Native engine/panel:** Windows and the minimal toolchain. Logic tests need no OBS download or GPU.
   - **Linux port:** follow the [Linux guide](DEVELOPMENT_LINUX.md) and build on the existing port work.
4. Make one focused change and run its checks. For a bug fix, add a regression
   test where practical. Include the docs affected by your change.
5. Open a PR explaining the problem, the change, and how you checked it.
   Add before/after screenshots for visible changes. Draft PRs are welcome.

An issue is helpful context, not a requirement to start. If your change needs a game,
GPU, controller, or multiple monitors to verify, say what you tested and what
still needs checking. You can ask for help without having every device.

## Using an agent

Give your agent this prompt, followed by your task:

> Read CONTRIBUTOR_AGENT.md, then implement: [describe your change]. Target:
> [platform/branch, if relevant]. Keep changes local; report checks and anything
> that still needs verification.

AI-assisted contributions follow the same checks. Review the diff yourself and
include actual results; explain any checks you could not run.

## Reporting a bug

Open a [bug report](https://github.com/bulhaa1/BuffrX/issues/new?template=bug_report.yml)
with the BuffrX version and steps to reproduce it. GPU/driver details and a short,
redacted log excerpt help when relevant. Logs and clips are optional.

You can also help by reproducing a report, testing a proposed fix, or improving
these instructions. For questions, join [Discord](https://discord.gg/73Js632Ba).

Contributions are licensed under the project's [GPL-2.0-or-later license](LICENSE).
