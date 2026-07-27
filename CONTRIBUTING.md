# Contributing to WinAnanicy

Thank you for improving WinAnanicy.

1. Create a focused branch.
2. Keep user-facing strings in both `Strings.en.xaml` and `Strings.tr.xaml`.
3. Build the C++ engine with warnings enabled and run CTest.
4. Build the WPF project in Release mode with zero warnings.
5. Add or update tests for behavior changes.
6. Explain user impact and safety implications in the pull request.

Use Conventional Commits where practical. Do not add telemetry, network calls,
privileged services, or configuration execution paths without an explicit
security review.
