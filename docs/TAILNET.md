# Tailnet reach — controlling Claude sessions across networks

## The problem
The deck discovers bridges via **mDNS** (`_claudeq._tcp`), which is **link-local** — it only works when
the deck and the bridges share a LAN. To control sessions on machines elsewhere (office, a cloud VM, a
laptop on cellular) — and to carry the deck somewhere and still reach everything — the deck needs reach
beyond its local segment.

## The decision
The deck is meant to be **portable**, so the overlay runs **on the device**: it becomes a real
[Tailscale](https://tailscale.com) node via **[microlink](https://github.com/CamM2325/microlink)** (MIT) —
a clean-room C implementation of the Tailscale `ts2021` control protocol + WireGuard + DERP + DISCO/STUN
for ESP-IDF. Plug the deck into any WiFi anywhere; it joins your tailnet and can reach every bridge.

Discovery is **hybrid** — we *add* tailnet discovery without removing mDNS:
- **mDNS (local):** unchanged. Bridges on the deck's current LAN are found as today — including machines
  that are **not** on your tailnet (so a home box that never joined Tailscale is still reachable while
  the deck is at home).
- **Tailnet (remote):** when an auth key is configured, the deck also enumerates tailnet peers
  (`microlink_get_peer_count` / `microlink_get_peer_info`, with a peer callback for liveness), filters to
  Claudeq bridges, and opens a WebSocket to each over its `100.x` VPN IP.

A machine reachable both ways has **two different addresses** (its LAN IP and its `100.x` tailnet IP), so
`ip:port` can't dedup it. Instead the bridge sends a stable per-process **`bridge_id`** in its `sessions`
message: the deck keeps **both** connections but elects one **primary** (prefers the LAN path), and only
the primary's messages reach the UI — so the machine's sessions show **once**. If the primary drops (you
leave home and the LAN path dies), the still-connected tailnet shadow is promoted instantly, replaying its
cached chips with no flap and no rediscovery wait. Everything downstream (focus, macros, voice, the
activity feed) is unchanged and routes over the surviving connection.

## What runs where
| Piece | Change |
|---|---|
| **Deck** | adds the `microlink` component; `net.cpp` keeps mDNS discovery and adds tailnet-peer discovery; the setup portal gains a **Tailscale auth key** field (stored in NVS). microlink rides the existing WiFi STA connection — it does **not** manage WiFi. |
| **Bridge machines you want to reach remotely** | join your tailnet (stock Tailscale, or self-hosted Headscale) and bind the bridge to the tailnet IP. Machines you only reach at home need no Tailscale — mDNS covers them. |

## ⚠️ Key security on the device — read before shipping
Putting tailnet credentials on a physical, pocketable device changes the threat model. **Treat the deck
as a device that can be lost or stolen.**

- **What's stored on the deck.** The Tailscale **auth key** (entered during setup) is used once to
  register, then the persistent secret is the generated **WireGuard node key**. Both live in **NVS on the
  ESP32-S3, whose flash is *unencrypted by default*.** Anyone with physical access and a USB cable can
  dump flash and extract the node key — which is a credential that can join/impersonate the deck on your
  tailnet until revoked.
- **Mitigations (do these):**
  - **Scope the key.** Use a **tagged** auth key (e.g. `tag:claudeq-deck`) and an **ACL** that lets that
    tag reach **only** the bridge port (`tcp:8787`) on `tag:claudeq-bridge` — never your whole tailnet.
    Prefer **ephemeral** and/or short-lived **reusable** keys so a leaked key has a small blast radius.
  - **Revoke on loss.** If the deck is lost, **delete the node** in the Tailscale/Headscale admin console
    (and rotate the auth key). That instantly cuts its access regardless of the on-flash key.
  - **Harden the flash for production.** Enable **flash encryption** and **Secure Boot v2** (ESP-IDF) so
    the node key can't be read off a stolen device. This is the single most important hardening step if
    these are distributed beyond personal use.
  - **Don't put the auth key in source or chat.** Enter it on-device via the setup portal, or seed it
    through the local-only secret-file convention used for WiFi creds — never commit it.
- **What's *not* exposed.** Session content (answers, prompts, voice) is end-to-end WireGuard-encrypted
  between the deck and each bridge; DERP relays and the coordination server never see payloads, only
  metadata. The deck↔bridge protocol itself is currently unauthenticated plaintext, which is fine on a
  trusted LAN but should gain a shared-secret/per-message token before it's trusted across the tailnet.

## Status / open items
- Validate the microlink build on this project's **ESP-IDF 5.4.1** (microlink is tested on 5.3).
- Measure SRAM/PSRAM headroom alongside LVGL + WiFi + the WS client on the real board.
- microlink is young and single-maintainer and reverse-implements an evolving protocol — **Headscale**
  self-hosting (supported) removes the dependency on Tailscale's servers and protocol stability.
- Add the deck↔bridge auth token before relying on the cross-tailnet hop.
- **Upgrade bridges together.** Dedup needs the `bridge_id` field; a pre-`bridge_id` bridge reached over
  *both* the LAN and the tailnet would show twice until updated (a stable id can't be inferred safely —
  `host` isn't unique). New bridges always send it, so this only bites during a mixed-version window.
