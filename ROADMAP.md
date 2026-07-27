# WinAnanicy roadmap

This roadmap lists concrete, user-facing work rather than fixed delivery dates.
Items move according to testing results and community feedback.

## Trust and distribution

- [ ] Sign Windows release binaries with Authenticode.
- [ ] Publish WinAnanicy through Windows Package Manager (`winget`).
- [x] Add GitHub artifact attestations to the release workflow.

## Rules and presets

- [ ] Build a reviewed community preset library.
- [ ] Add rule import previews and conflict reporting.
- [ ] Expand sustained-load diagnostics before a throttle is applied.

## User experience

- [ ] Add more interface languages while keeping English and Turkish complete.
- [ ] Add an in-app explanation of each preset's exact Windows controls.
- [ ] Improve accessibility testing for keyboard and high-contrast use.

## Contributing

Open an issue before starting a large change. Good first contributions include
documentation, localization, preset examples, and focused regression tests.
Every behavior change must preserve WinAnanicy's reversible, per-user safety
model.
