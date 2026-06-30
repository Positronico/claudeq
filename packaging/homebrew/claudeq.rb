# Homebrew formula for the Claudeq bridge + launcher.
#
# Lives in the tap repo github.com/Positronico/homebrew-tap as Formula/claudeq.rb.
# Users install with:
#   brew install Positronico/tap/claudeq
#
# To cut a new version: tag the main repo (vX.Y.Z), then update `url` + `sha256` here
# (sha256 = `curl -L <url> | shasum -a 256`). See PUBLISHING.md.
class Claudeq < Formula
  desc "Physical touchscreen control surface for Claude Code (ESP32-S3 deck)"
  homepage "https://github.com/Positronico/claudeq"
  url "https://github.com/Positronico/claudeq/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "REPLACE_WITH_TARBALL_SHA256"
  license "MIT"

  depends_on "node"   # runtime for the bridge
  depends_on "tmux"   # macro/voice injection into Claude sessions
  depends_on "ffmpeg" # alert-sound conversion

  def install
    libexec.install "bridge"
    # Only the pre-built firmware is needed at runtime (for `claudeq flash`), not the IDF sources.
    (libexec/"firmware").install "firmware/dist"

    # The two tiny runtime deps (ws, bonjour-service) are vendored in the tarball, so install is
    # offline (Homebrew sandboxes network during install). Restore from the lock only if absent.
    unless (libexec/"bridge/node_modules").exist?
      cd(libexec/"bridge") { system "npm", "ci", "--omit=dev", "--no-audit", "--no-fund" }
    end

    # Wrapper execs the REAL script path so claudeq's BASH_SOURCE-based lookup of its siblings
    # (bridge.mjs, ccdeck-settings.json, ../firmware/dist/flash.sh) resolves inside libexec.
    # Note: we deliberately do NOT expose the in-repo `cc` shim — it would shadow the C compiler.
    (bin/"claudeq").write <<~SH
      #!/bin/bash
      exec "#{libexec}/bridge/claudeq" "$@"
    SH
  end

  def caveats
    <<~EOS
      Flash the device (no toolchain needed):
        • Browser:  https://positronico.github.io/claudeq/  (Chrome/Edge → Install)
        • CLI:      claudeq flash      (uses esptool/espflash if present)

      Use it, in any project:
        cd your-project && claudeq

      Optional local voice transcription:
        brew install whisper-cpp   # then see the README "Voice" setup
    EOS
  end

  test do
    # `flash` with a dummy port must be recognized and fail gracefully (no toolchain in the sandbox),
    # proving the wrapper resolves the bundled firmware + flash.sh.
    output = shell_output("#{bin}/claudeq flash /dev/null 2>&1", 1)
    assert_match(/flasher|esptool|firmware/i, output)
  end
end
