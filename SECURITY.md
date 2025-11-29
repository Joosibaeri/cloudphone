# Security Policy

## Supported Versions
Security updates and fixes are provided for the latest development branch. Older versions may not receive timely security patches.

## Reporting a Vulnerability
If you discover a security vulnerability, please report it responsibly via email to **contact@joosibaeri.xyz**. Include:
- A clear description of the vulnerability
- Steps to reproduce the issue
- Any potential impact or risk

Do **not** create a public issue for security vulnerabilities to avoid exposing sensitive information before a fix is released.

## Response Process
1. **Acknowledgment** – We will confirm receipt of your report within 48 hours.
2. **Assessment** – The issue will be evaluated for severity and potential impact.
3. **Fix** – A patch or mitigation will be prepared and tested.
4. **Disclosure** – After a fix is released, a public advisory may be published detailing the issue and the resolution.

## Security Best Practices
- Run the server only in trusted environments; it is **not designed for public Internet exposure**.
- Secure SSH access with strong keys and minimal permissions.
- Protect the filesystem containing user data in `/userdata/accounts`.
- Keep the server and client updated to ensure all security patches are applied.

## Responsible Disclosure Commitment
Reporters acting in good faith will not face legal action if they follow the guidelines above.
