# Contributing to whitehttpd

We welcome contributions, bug reports, and optimizations. To maintain the project's
architectural integrity and dual-licensing business model, all contributions must
comply with the following requirements:

## 1. Quality and Verification Standards
- **Regression Tests are Mandatory**: Every functional change, protocol addition, or bugfix must include an automated regression test. Run `./test_all.sh --mips` (or the relevant tests in `BUILDING.md`) before submitting a pull request.
- **Strict Size Ceiling**: For the assembly reference (`final-4092/`), the binary size must remain strictly within 4,096 bytes (enforced by `make check-size`).
- **Freestanding Architecture**: The C implementation must remain freestanding (zero libc dependencies, zero heap allocation / no malloc, direct Linux system calls via nolibc).
- **Clean-Room Integrity**: No code or data structures from `darkhttpd` or GPL-incompatible projects may be copied or referenced. All code must be independently written.
- **Preserve Notices**: Maintain third-party notices in `NOTICE` and `LICENSES/`.

## 2. Contributor License Agreement (CLA) & Dual-Licensing
The `whitehttpd` project operates under a **Dual-Licensing** model (GNU AGPLv3 for open source, commercial OEM license for hardware and firmware vendors):
- All exclusive proprietary and commercial relicensing rights to the codebase are retained by the author (Ivan Svarkovsky).
- External contributions, pull requests, and patches are accepted strictly under a Contributor License Agreement granting the author a perpetual, worldwide, irrevocable, royalty-free license to relicense, dual-license, and distribute the submitted code under both open-source (AGPL-3.0-or-later) and commercial OEM terms.
- By submitting a pull request or patch, you certify that you are the original author of the code, hold all necessary rights to submit it, and agree to transfer commercial relicensing authority to Ivan Svarkovsky under this policy.
