# Privacy information

Omarchy Media Writer sends a custom User-Agent header when it fetches the Omarchy release information and ISO from omarchy.org and iso.omarchy.org.

This User-Agent string is in the following format: `OmarchyMediaWriter/$VERSION ($OS $OSVERSION; $BUILDARCH; $LOCALE; $DETAILS)`.

You can disable this behavior by using `omarchy-media-writer --no-user-agent` which will make it use the Qt default User-Agent string (likely `Mozilla/5.0`).
