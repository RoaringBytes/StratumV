# Security Policy

## Supported versions

StratumV is under active development. Security fixes are applied to the
`main` branch and the most recent tagged release (currently the `1.3.x`
line). Older tags are not maintained.

| Version | Supported |
|---------|-----------|
| `main`  | ✅ |
| `1.3.x` | ✅ |
| < 1.3   | ❌ |

## Reporting a vulnerability

Please **do not** open a public issue for security vulnerabilities.

Instead, report privately using GitHub's
[private vulnerability reporting](https://github.com/RoaringBytes/StratumV/security/advisories/new)
("Report a vulnerability" under the repository's **Security** tab). This
keeps the details confidential until a fix is available.

When reporting, please include:

- a description of the issue and its potential impact;
- steps to reproduce, or a proof-of-concept, if available;
- affected version, commit, or branch;
- any suggested mitigation.

## What to expect

- We aim to acknowledge new reports within a few business days.
- We will confirm the issue, determine affected versions, and work on a fix.
- We will coordinate a disclosure timeline with the reporter and credit
  reporters who wish to be named once a fix ships.

## Scope

This policy covers the StratumV engine source in this repository. The
optional proprietary NVIDIA components (DLSS / SHARC / RTXNTC), which are
disabled by default, are governed by NVIDIA's own terms and security
processes.
