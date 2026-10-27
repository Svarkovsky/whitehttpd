# Security

## Reporting a vulnerability

Email Ivan Svarkovsky <ivansvarkovsky@gmail.com>. Do not open a public issue.
Say which build you tested (`final-4092` or `whitehttpd_c`), what request
triggers the problem, and what you expected instead.

Expect an initial reply within 7 days. If the report is valid you are credited
in the fix commit unless you ask otherwise.

## Attack surface

Both builds read files from a document root and write them to a socket. They
never read a request body, never write to disk, and never contact another host.
What an attacker can influence is therefore four things: the request line, the
headers, the path, and the contents of the files you chose to publish.

Path confinement, header validation and timeout behaviour are described in
`final-4092/README.md` (assembler reference) and `whitehttpd_c/README.md`
(C implementation); the manual page is `final-4092/whitehttpd.1`. Those are
the reference; this file does not restate them, because a second copy of
them drifts from the code.

## Known limitations

By design, but security-relevant, so state them before deploying.

- **No TLS.** Plaintext. Put a terminator in front for anything public.
- **No authentication.** Everything in the root is readable by anyone who can
  reach the port. Do not put secrets there.
- **No privilege drop, no chroot.** The process runs as whoever started it. Use
  an unprivileged user and a directory only that user can read.
- **The assembler build listens on all interfaces,** IPv4 and IPv6, and has no
  `--addr` to narrow it. Firewall the port. The C build has `--addr`.
- **No access log.** Nothing records who requested what. Use a proxy or the
  firewall if you need audit.
- **MIME type is chosen by extension alone.** Do not let untrusted users upload
  into the document root.
- **Dotfiles answer `403`,** with one exception: the C build allows the first
  component `.well-known` for ACME. The assembler build does not, so it cannot
  serve certificate renewals directly.

## Out of scope

- Denial of service by volume. Both builds are single-process, one event loop
  each, with no worker pool.
- Anything requiring a privilege the server does not have.
- The contents of files you chose to publish.
