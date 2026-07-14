# Security Policy

## Supported Versions

Security fixes are applied to the latest release and the `main` branch.

| Version        | Supported |
| -------------- | --------- |
| `main`         | Yes       |
| `0.1.x`        | Yes       |
| Older versions | No        |

## Reporting a Vulnerability

Use [GitHub private vulnerability reporting](https://github.com/asayed18/icop/security/advisories/new).
Do not open a public issue for a suspected vulnerability.

Include the affected platform, VLC version, icop version or commit,
reproduction steps, and sanitized logs. Do not attach explicit media, private
videos, credentials, or personally identifiable information.

You should receive an acknowledgement within seven days. Disclosure timing
will be coordinated after the issue is understood and a fix is available.

## Scope

Relevant reports include unsafe frame exposure caused by queue or timing bugs,
arbitrary code execution, unsafe dynamic-library loading, path traversal,
malicious model handling, and unintended disclosure of local media metadata.
Model accuracy disagreements without a security boundary bypass should use the
normal bug-report template.
